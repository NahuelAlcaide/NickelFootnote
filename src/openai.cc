// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>

#include "log.h"
#include "nickel.h"
#include "openai.h"
#include "store.h"

static const char *AUTH_PATH = NFN_DIR "/auth.json";
static const char *TOKEN_URL = "https://auth.openai.com/api/accounts/oauth/token";
static const char *RESPONSES_URL = "https://api.openai.com/v1/responses";
static const char *API_RESOURCE = "https://api.openai.com/v1";
static const int WIFI_TIMEOUT_MS = 45000;
static const int REFRESH_TIMEOUT_MS = 30000;
static const int REQUEST_TIMEOUT_MS = 150000;
static const int STALL_TIMEOUT_MS = 30000; // the server normally answers within a few seconds
static const qint64 REFRESH_MARGIN_S = 120;

static const char *SIGN_IN_AGAIN =
    "Sign in again on the PC:\npython tools/signin.py login\npython tools/signin.py push";

ChatClient::ChatClient(QObject *parent)
    : QObject(parent), m_nam(nullptr), m_busy(false), m_waiting_wifi(false), m_retried_auth(false), m_retried_stall(false),
      m_generation(0), m_settle_generation(-1), m_completed(false) {
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, SIGNAL(timeout()), this, SLOT(timed_out()));
    m_stall_timer = new QTimer(this);
    m_stall_timer->setSingleShot(true);
    connect(m_stall_timer, SIGNAL(timeout()), this, SLOT(stalled()));
}

// Created on first use: QNetworkAccessManager loads Qt's bearer plugins.
QNetworkAccessManager *ChatClient::nam() {
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    // Qt 5.2 refuses requests when its bearer code thinks we're offline.
    m_nam->setNetworkAccessible(QNetworkAccessManager::Accessible);
    return m_nam;
}

void ChatClient::send(QJsonArray const &input, QString const &instructions) {
    if (m_busy)
        cancel();
    m_busy = true;
    m_generation++;
    m_input = input;
    m_instructions = instructions;
    m_retried_auth = false;
    m_retried_stall = false;
    m_text.clear();
    m_sources.clear();
    m_failure.clear();
    m_completed = false;
    ensure_network();
}

void ChatClient::cancel() {
    if (!m_busy)
        return;
    nfn_log("chat: cancelled");
    m_generation++;
    m_busy = false;
    m_waiting_wifi = false;
    m_timer->stop();
    m_stall_timer->stop();
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
}

void ChatClient::finish(bool ok, QString const &error) {
    if (!m_busy)
        return;
    m_busy = false;
    m_waiting_wifi = false;
    m_timer->stop();
    m_stall_timer->stop();
    nfn_log("chat: %s (%d chars, %d sources)%s%s", ok ? "done" : "failed", m_text.size(), m_sources.size(),
             error.isEmpty() ? "" : ": ", qPrintable(error.left(200)));
    emit finished(ok, m_text, error, m_sources);
}

// A request that gets no answer at all (a dead pooled connection, Wi-Fi
// asleep) is retried once on a fresh connection instead of hanging.
void ChatClient::stalled() {
    if (!m_busy || !m_reply)
        return;
    nfn_log("chat: no response after %ds%s", STALL_TIMEOUT_MS / 1000, m_retried_stall ? "" : ", retrying");
    m_reply->setProperty("stalled", true);
    m_reply->abort(); // reply_finished handles it
}

void ChatClient::timed_out() {
    if (m_waiting_wifi) {
        wifi_failed();
        return;
    }
    nfn_log("chat: timed out");
    if (m_reply) {
        m_reply->setProperty("timedOut", true);
        m_reply->abort(); // reply_finished / refresh_finished reports it
    } else {
        finish(false, QStringLiteral("Timed out."));
    }
}

// --- Wi-Fi ---------------------------------------------------------------------------

void ChatClient::ensure_network() {
    QObject *wfm = WirelessWorkflowManager_sharedInstance ? WirelessWorkflowManager_sharedInstance() : nullptr;
    if (!wfm || WirelessWorkflowManager_isInternetAccessible(wfm)) {
        ensure_token();
        return;
    }
    nfn_log("chat: connecting Wi-Fi");
    emit progress(QStringLiteral("Connecting to Wi-Fi..."));
    m_waiting_wifi = true;
    if (QObject *wm = WirelessManager_sharedInstance())
        connect(wm, SIGNAL(networkConnected()), this, SLOT(wifi_connected()), Qt::UniqueConnection);
    connect(wfm, SIGNAL(connectingFailed()), this, SLOT(wifi_failed()), Qt::UniqueConnection);
    m_timer->start(WIFI_TIMEOUT_MS);
    WirelessWorkflowManager_connectWireless(wfm, false, false);
}

void ChatClient::wifi_connected() {
    if (!m_waiting_wifi)
        return;
    m_waiting_wifi = false;
    m_timer->stop();
    nfn_log("chat: Wi-Fi connected");
    // Let Nickel finish its own network setup (DNS, time) first.
    m_settle_generation = m_generation;
    QTimer::singleShot(1500, this, SLOT(wifi_settled()));
}

void ChatClient::wifi_settled() {
    if (m_busy && m_settle_generation == m_generation)
        ensure_token();
}

void ChatClient::wifi_failed() {
    if (!m_waiting_wifi)
        return;
    finish(false, QStringLiteral("Couldn't connect to Wi-Fi."));
}

// --- tokens ----------------------------------------------------------------------------

static qint64 now_s() {
    return QDateTime::currentMSecsSinceEpoch() / 1000;
}

void ChatClient::ensure_token() {
    QFile f(AUTH_PATH);
    if (!f.open(QIODevice::ReadOnly)) {
        finish(false, QStringLiteral("Not signed in: auth.json is missing.\n") + SIGN_IN_AGAIN);
        return;
    }
    m_auth = QJsonDocument::fromJson(f.readAll()).object();
    if (m_auth.value("refresh_token").toString().isEmpty() || m_auth.value("client_id").toString().isEmpty()) {
        finish(false, QStringLiteral("auth.json has no sign-in tokens.\n") + SIGN_IN_AGAIN);
        return;
    }
    qint64 expires = (qint64)m_auth.value("expires_at").toDouble();
    if (m_auth.value("access_token").toString().isEmpty() || expires - REFRESH_MARGIN_S < now_s())
        start_refresh();
    else
        start_request();
}

void ChatClient::start_refresh() {
    nfn_log("chat: refreshing the access token");
    emit progress(QStringLiteral("Signing in..."));
    QByteArray form;
    auto field = [&form](const char *name, QString const &value) {
        if (!form.isEmpty())
            form += '&';
        form += name;
        form += '=';
        form += QUrl::toPercentEncoding(value);
    };
    field("grant_type", QStringLiteral("refresh_token"));
    field("client_id", m_auth.value("client_id").toString());
    field("refresh_token", m_auth.value("refresh_token").toString());
    field("resource", QString::fromLatin1(API_RESOURCE));
    QNetworkRequest req((QUrl(TOKEN_URL)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    req.setRawHeader("User-Agent", "NickelFootnote/" NFN_VERSION);
    req.setRawHeader("Accept", "application/json");
    m_reply = nam()->post(req, form);
    connect(m_reply, SIGNAL(finished()), this, SLOT(refresh_finished()));
    connect(m_reply, &QNetworkReply::sslErrors, this, [](QList<QSslError> const &errors) {
        for (QSslError const &e : errors)
            nfn_log("chat: SSL error: %s", qPrintable(e.errorString()));
    });
    m_timer->start(REFRESH_TIMEOUT_MS);
}

void ChatClient::refresh_finished() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;
    reply->deleteLater();
    if (reply != m_reply.data())
        return;
    m_reply = nullptr;
    m_timer->stop();

    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray body = reply->readAll();
    nfn_log("chat: refresh status=%d error=%d (%s) ssl=%s", status, reply->error(),
             qPrintable(reply->errorString()),
             QSslSocket::supportsSsl() ? qPrintable(QSslSocket::sslLibraryVersionString()) : "none");
    if (reply->property("timedOut").toBool()) {
        finish(false, QStringLiteral("Timed out while signing in."));
        return;
    }
    if (status == 0) {
        finish(false, QStringLiteral("Network error: ") + reply->errorString());
        return;
    }
    QJsonObject r = QJsonDocument::fromJson(body).object();
    if (status != 200 || r.value("access_token").toString().isEmpty()) {
        QString code = r.value("error").isObject() ? r.value("error").toObject().value("code").toString()
                                                   : r.value("error").toString();
        nfn_log("chat: refresh failed: %s", qPrintable(code.isEmpty() ? QString::number(status) : code));
        finish(false, QStringLiteral("Sign-in expired (%1).\n").arg(code.isEmpty() ? QString::number(status) : code) +
                          SIGN_IN_AGAIN);
        return;
    }

    m_auth.insert("access_token", r.value("access_token"));
    if (!r.value("refresh_token").toString().isEmpty())
        m_auth.insert("refresh_token", r.value("refresh_token"));
    if (!r.value("id_token").toString().isEmpty())
        m_auth.insert("id_token", r.value("id_token"));
    int expires_in = r.value("expires_in").toInt(3600);
    m_auth.insert("expires_at", (double)(now_s() + expires_in));
    // The old refresh token is now spent: save the new one before anything else.
    if (!write_file_atomic(AUTH_PATH, QJsonDocument(m_auth).toJson())) {
        nfn_log("chat: saving auth.json FAILED");
        finish(false, QStringLiteral("Couldn't save the new sign-in tokens.\n") + SIGN_IN_AGAIN);
        return;
    }
    nfn_log("chat: token refreshed, expires in %ds", expires_in);
    start_request();
}

// --- the request ------------------------------------------------------------------------

void ChatClient::start_request() {
    Config cfg = load_config();
    QJsonObject body;
    body.insert("model", cfg.model);
    body.insert("instructions", m_instructions);
    body.insert("input", m_input);
    body.insert("store", false);
    body.insert("stream", true);
    if (cfg.webSearch) {
        QJsonObject tool;
        tool.insert("type", QStringLiteral("web_search"));
        QJsonArray tools;
        tools.append(tool);
        body.insert("tools", tools);
    }
    if (!cfg.effort.isEmpty()) {
        QJsonObject reasoning;
        reasoning.insert("effort", cfg.effort);
        body.insert("reasoning", reasoning);
    }

    nfn_log("chat: request model=%s effort=%s search=%d turns=%d", qPrintable(cfg.model),
             cfg.effort.isEmpty() ? "default" : qPrintable(cfg.effort), cfg.webSearch, m_input.size());
    emit progress(QStringLiteral("Asking ChatGPT..."));

    QNetworkRequest req((QUrl(RESPONSES_URL)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "text/event-stream");
    req.setRawHeader("Connection", "close"); // never reuse a possibly dead pooled connection
    req.setRawHeader("User-Agent", "NickelFootnote/" NFN_VERSION);
    req.setRawHeader("Authorization", "Bearer " + m_auth.value("access_token").toString().toUtf8());

    m_buffer.clear();
    m_error_body.clear();
    m_reply = nam()->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, SIGNAL(readyRead()), this, SLOT(reply_ready_read()));
    connect(m_reply, SIGNAL(finished()), this, SLOT(reply_finished()));
    connect(m_reply, &QNetworkReply::metaDataChanged, this, [this]() { m_stall_timer->stop(); });
    m_stall_timer->start(STALL_TIMEOUT_MS);
    connect(m_reply, &QNetworkReply::sslErrors, this, [](QList<QSslError> const &errors) {
        for (QSslError const &e : errors)
            nfn_log("chat: SSL error: %s", qPrintable(e.errorString()));
    });
    m_timer->start(REQUEST_TIMEOUT_MS);
}

void ChatClient::reply_ready_read() {
    QNetworkReply *reply = m_reply;
    if (!reply || sender() != reply)
        return;
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) {
        m_error_body += reply->readAll();
        return;
    }
    m_buffer += reply->readAll();
    QString before = m_text;
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        QByteArray line = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        if (!line.startsWith("data:"))
            continue;
        QByteArray payload = line.mid(5).trimmed();
        if (payload.isEmpty() || payload == "[DONE]")
            continue;
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
        if (err.error != QJsonParseError::NoError) {
            nfn_log("chat: bad event JSON (%d bytes)", payload.size());
            continue;
        }
        handle_event(doc.object());
    }
    if (m_text != before)
        emit textChanged(m_text);
}

static QString error_message(QJsonObject const &e) {
    QString code = e.value("code").toString();
    QString msg = e.value("message").toString();
    if (code == "subscription_sharing_usage_limit_exceeded" || code == "rate_limit_exceeded" ||
        code == "usage_limit_reached")
        return QStringLiteral("ChatGPT usage limit reached. Try again later.\n(%1)").arg(code);
    if (msg.isEmpty())
        return code.isEmpty() ? QStringLiteral("Unknown error.") : code;
    return code.isEmpty() ? msg : msg + " (" + code + ")";
}

static QString source_label(QJsonObject const &ann) {
    QString url = ann.value("url").toString();
    if (url.isEmpty())
        return QString();
    QString host = QUrl(url).host();
    if (host.startsWith("www."))
        host = host.mid(4);
    QString title = ann.value("title").toString().simplified();
    return title.isEmpty() ? host : title + " (" + host + ")";
}

void ChatClient::handle_event(QJsonObject const &ev) {
    QString type = ev.value("type").toString();
    if (type == "response.output_text.delta") {
        m_text += ev.value("delta").toString();
    } else if (type == "response.output_item.added") {
        if (ev.value("item").toObject().value("type").toString() == "web_search_call")
            emit progress(QStringLiteral("Searching the web..."));
    } else if (type == "response.output_item.done") {
        QJsonObject item = ev.value("item").toObject();
        if (item.value("type").toString() == "web_search_call") {
            QJsonObject action = item.value("action").toObject();
            QString kind = action.value("type").toString();
            QString q = action.value("query").toString();
            QString host = QUrl(action.value("url").toString()).host();
            nfn_log("chat: web %s (%d chars)", kind.isEmpty() ? "search" : qPrintable(kind), q.size());
            if (!q.isEmpty())
                emit progress(QStringLiteral("Searched: ") + q);
            else if (!host.isEmpty())
                emit progress(QStringLiteral("Reading ") + host + "...");
            else
                emit progress(QStringLiteral("Searching the web..."));
        }
    } else if (type == "response.output_text.annotation.added") {
        QString s = source_label(ev.value("annotation").toObject());
        if (!s.isEmpty() && !m_sources.contains(s))
            m_sources << s;
    } else if (type == "response.reasoning_summary_text.delta" || type == "response.reasoning_summary_part.added") {
        emit progress(QStringLiteral("Thinking..."));
    } else if (type == "response.completed") {
        m_completed = true;
        QJsonObject resp = ev.value("response").toObject();
        nfn_log("chat: completed (server model %s)", qPrintable(resp.value("model").toString()));
        for (QJsonValue item : resp.value("output").toArray())
            for (QJsonValue part : item.toObject().value("content").toArray())
                for (QJsonValue ann : part.toObject().value("annotations").toArray()) {
                    QString s = source_label(ann.toObject());
                    if (!s.isEmpty() && !m_sources.contains(s))
                        m_sources << s;
                }
    } else if (type == "response.failed") {
        m_failure = error_message(ev.value("response").toObject().value("error").toObject());
    } else if (type == "response.incomplete") {
        QString reason = ev.value("response").toObject().value("incomplete_details").toObject().value("reason").toString();
        m_failure = QStringLiteral("The answer was cut short (%1).").arg(reason.isEmpty() ? "incomplete" : reason);
    } else if (type == "error") {
        QJsonObject e = ev.value("error").isObject() ? ev.value("error").toObject() : ev;
        m_failure = error_message(e);
    }
}

void ChatClient::reply_finished() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;
    reply->deleteLater();
    if (reply != m_reply.data())
        return;
    reply_ready_read(); // whatever is left
    m_reply = nullptr;
    m_timer->stop();
    m_stall_timer->stop();

    if (reply->property("stalled").toBool()) {
        if (!m_retried_stall) {
            m_retried_stall = true;
            start_request();
        } else {
            finish(false, QStringLiteral("ChatGPT didn't respond. Check the connection and try again."));
        }
        return;
    }
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    nfn_log("chat: reply status=%d error=%d (%s) completed=%d ssl=%s", status, reply->error(),
             qPrintable(reply->errorString()), m_completed,
             QSslSocket::supportsSsl() ? qPrintable(QSslSocket::sslLibraryVersionString()) : "none");

    if (reply->property("timedOut").toBool()) {
        finish(!m_text.isEmpty() && m_completed, QStringLiteral("Timed out waiting for ChatGPT."));
        return;
    }
    if (status == 401 && !m_retried_auth) {
        // The clock or the saved expiry may be off: refresh once and retry.
        m_retried_auth = true;
        start_refresh();
        return;
    }
    if (status >= 400) {
        QJsonObject e = QJsonDocument::fromJson(m_error_body).object().value("error").toObject();
        QString msg = e.isEmpty() ? QString::fromUtf8(m_error_body.left(300)) : error_message(e);
        nfn_log("chat: HTTP %d: %s", status, qPrintable(msg.left(200)));
        if (status == 401)
            msg += QStringLiteral("\n") + SIGN_IN_AGAIN;
        finish(false, QStringLiteral("HTTP %1: %2").arg(status).arg(msg));
        return;
    }
    if (status == 0) {
        finish(false, QStringLiteral("Network error: ") + reply->errorString());
        return;
    }
    if (!m_failure.isEmpty()) {
        finish(false, m_failure);
        return;
    }
    if (!m_completed) {
        finish(false, m_text.isEmpty() ? QStringLiteral("The connection closed before an answer arrived.")
                                       : QStringLiteral("The connection closed before the answer was complete."));
        return;
    }
    finish(true, QString());
}
