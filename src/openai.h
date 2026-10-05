// SPDX-License-Identifier: GPL-3.0-or-later

// ChatGPT over "Sign in with ChatGPT" tokens: Wi-Fi, token refresh, and a
// streamed Responses API request.
//
// The tokens come from the PC (tools/siwc_test.py push) in auth.json. The
// refresh token rotates on every refresh, so the new tokens are written to
// disk before they are used, and only one refresh runs at a time.

#ifndef NICKLEGPT_OPENAI_H
#define NICKLEGPT_OPENAI_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

class ChatClient : public QObject {
    Q_OBJECT

public:
    explicit ChatClient(QObject *parent = nullptr);

    // Sends the conversation (Responses API `input` items). Emits progress()
    // and textChanged() while it runs, then finished() exactly once.
    void send(QJsonArray const &input, QString const &instructions);
    void cancel();
    bool busy() const { return m_busy; }

signals:
    void progress(QString const &status);
    void textChanged(QString const &text);
    void finished(bool ok, QString const &text, QString const &error, QStringList const &sources);

private slots:
    void wifi_connected();
    void wifi_failed();
    void wifi_settled();
    void refresh_finished();
    void reply_ready_read();
    void reply_finished();
    void timed_out();
    void stalled();

private:
    void ensure_network();
    void ensure_token();
    void start_refresh();
    void start_request();
    void handle_event(QJsonObject const &ev);
    void finish(bool ok, QString const &error);
    QNetworkAccessManager *nam();

    QNetworkAccessManager *m_nam;
    QTimer *m_timer;
    QTimer *m_stall_timer; // no response headers/bytes yet
    QPointer<QNetworkReply> m_reply;
    bool m_busy;
    bool m_waiting_wifi;
    bool m_retried_auth;
    bool m_retried_stall;
    int m_generation; // ignores signals from cancelled runs
    int m_settle_generation;

    QJsonObject m_auth;
    QJsonArray m_input;
    QString m_instructions;

    QByteArray m_buffer;
    QByteArray m_error_body;
    QString m_text;
    QStringList m_sources;
    QString m_failure;
    bool m_completed;
};

#endif
