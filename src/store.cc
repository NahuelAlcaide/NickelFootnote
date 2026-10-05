// SPDX-License-Identifier: GPL-3.0-or-later

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "log.h"
#include "store.h"

static const char *CONFIG_PATH = NFN_DIR "/config.ini";
static const char *BOOKS_PATH = NFN_DIR "/books.json";
static const char *DB_PATH = "/mnt/onboard/.kobo/KoboReader.sqlite";
static const char *DB_CONNECTION = "nickelfootnote-readonly";

static const char DEFAULT_CONFIG[] =
    "; NickelFootnote settings. Changes apply to the next question.\n"
    "[chatgpt]\n"
    "; Model slug. gpt-6.1-sol is the default.\n"
    "model=gpt-6.1-sol\n"
    "; Reasoning effort (low, medium, high). Empty: the model's default.\n"
    "effort=\n"
    "; Let the model search the web (true/false).\n"
    "web_search=true\n"
    "\n"
    "[tags]\n"
    "; How names the model tags ([[char:Name]], [[place:..]], [[group:..]], [[term:..]]) are shown.\n"
    "; style: color (colour + bold), bold, plain, or test (colour + a swatch of options above the answer).\n"
    "style=color\n"
    "; Colours as #RRGGBB. Saturated, medium-dark tones read best on the colour screen.\n"
    "char=#B0207A\n"
    "place=#2E7D32\n"
    "group=#A3262A\n"
    "term=#1A4FD0\n"
    "\n"
    "[debug]\n"
    "; Also write the raw text of each answer to log.txt (true/false).\n"
    "log_answers=false\n";

static QString tag_value(QSettings &s, const char *key, const char *fallback, bool colour) {
    QString v = s.value(QStringLiteral("tags/") + key).toString().trimmed();
    if (colour) {
        static const QRegularExpression hex("^#[0-9a-fA-F]{6}$");
        return hex.match(v).hasMatch() ? v : QString::fromLatin1(fallback);
    }
    v = v.toLower();
    return (v == "color" || v == "bold" || v == "plain" || v == "test") ? v : QString::fromLatin1(fallback);
}

bool write_file_atomic(QString const &path, QByteArray const &data) {
    QByteArray target = QFile::encodeName(path);
    QByteArray tmp = target + ".tmp";
    int fd = open(tmp.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    const char *p = data.constData();
    qint64 left = data.size();
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) {
            close(fd);
            unlink(tmp.constData());
            return false;
        }
        p += n;
        left -= n;
    }
    bool ok = fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(tmp.constData(), target.constData()) != 0) {
        unlink(tmp.constData());
        return false;
    }
    int dir = open(NFN_DIR, O_RDONLY);
    if (dir >= 0) {
        fsync(dir);
        close(dir);
    }
    return true;
}

Config load_config() {
    if (!QFile::exists(CONFIG_PATH))
        write_file_atomic(CONFIG_PATH, QByteArray(DEFAULT_CONFIG));
    QSettings s(CONFIG_PATH, QSettings::IniFormat);
    Config c;
    c.model = s.value("chatgpt/model").toString().trimmed();
    if (c.model.isEmpty())
        c.model = QStringLiteral("gpt-6.1-sol");
    c.effort = s.value("chatgpt/effort").toString().trimmed().toLower();
    QString ws = s.value("chatgpt/web_search", "true").toString().trimmed().toLower();
    c.webSearch = !(ws == "false" || ws == "0" || ws == "no" || ws == "off");
    c.tagStyle = tag_value(s, "style", "color", false);
    c.tagChar = tag_value(s, "char", "#B0207A", true);
    c.tagPlace = tag_value(s, "place", "#2E7D32", true);
    c.tagGroup = tag_value(s, "group", "#A3262A", true);
    c.tagTerm = tag_value(s, "term", "#1A4FD0", true);
    QString la = s.value("debug/log_answers", "false").toString().trimmed().toLower();
    c.logAnswers = la == "true" || la == "1" || la == "yes" || la == "on";
    return c;
}

// --- per-book edits ----------------------------------------------------------------

static QJsonObject read_books() {
    QFile f(BOOKS_PATH);
    if (!f.open(QIODevice::ReadOnly))
        return QJsonObject();
    return QJsonDocument::fromJson(f.readAll()).object();
}

static void write_books(QJsonObject const &books) {
    if (!write_file_atomic(BOOKS_PATH, QJsonDocument(books).toJson()))
        nfn_log("store: writing books.json failed");
}

bool load_book_edit(QString const &key, BookEdit *e) {
    QJsonObject o = read_books().value(key).toObject();
    if (o.isEmpty())
        return false;
    e->series = o.value("series").toString();
    e->number = o.value("number").toString();
    e->title = o.value("title").toString();
    e->chapter = o.value("chapter").toString();
    e->percent = o.contains("percent") ? o.value("percent").toInt(-1) : -1;
    e->savedAtChapter = o.value("savedAtChapter").toString();
    return true;
}

void save_book_edit(QString const &key, BookEdit const &e) {
    QJsonObject o;
    if (!e.series.isEmpty()) o.insert("series", e.series);
    if (!e.number.isEmpty()) o.insert("number", e.number);
    if (!e.title.isEmpty()) o.insert("title", e.title);
    if (!e.chapter.isEmpty()) o.insert("chapter", e.chapter);
    if (e.percent >= 0) o.insert("percent", e.percent);
    o.insert("savedAtChapter", e.savedAtChapter);
    QJsonObject books = read_books();
    books.insert(key, o);
    write_books(books);
}

void clear_book_edit(QString const &key) {
    QJsonObject books = read_books();
    if (books.contains(key)) {
        books.remove(key);
        write_books(books);
    }
}

ReadingContext apply_book_edit(ReadingContext const &d, BookEdit const &e, QStringList *edited) {
    ReadingContext c = d;
    auto set = [&](QString &field, QString const &v, const char *name) {
        if (!v.isEmpty() && v != field) {
            field = v;
            if (edited) *edited << name;
        }
    };
    set(c.series, e.series, "series");
    set(c.seriesNumber, e.number, "number");
    set(c.title, e.title, "title");
    bool samePlace = e.savedAtChapter == d.chapter;
    if (samePlace) {
        set(c.chapter, e.chapter, "chapter");
        if (e.percent >= 0 && e.percent != c.percent) {
            c.percent = e.percent;
            if (edited) *edited << "progress";
        }
    }
    return c;
}

// --- series from KoboReader.sqlite ---------------------------------------------------

void lookup_series(QString const &contentId, QString *series, QString *number) {
    static QHash<QString, QStringList> cache;
    if (contentId.isEmpty())
        return;
    if (cache.contains(contentId)) {
        QStringList v = cache.value(contentId);
        *series = v.value(0);
        *number = v.value(1);
        return;
    }

    if (!QSqlDatabase::contains(DB_CONNECTION)) {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", DB_CONNECTION);
        db.setDatabaseName(DB_PATH);
        db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=1000");
    }
    QString s, n;
    {
        QSqlDatabase db = QSqlDatabase::database(DB_CONNECTION, false);
        if (!db.open()) {
            nfn_log("series: can't open the database");
            return;
        }
        {
            QSqlQuery q(db);
            q.prepare("SELECT Series, SeriesNumber FROM content WHERE ContentID = ? AND ContentType = 6");
            q.addBindValue(contentId);
            if (q.exec() && q.next()) {
                s = q.value(0).toString().trimmed();
                n = q.value(1).toString().trimmed();
            } else if (q.lastError().isValid()) {
                nfn_log("series: query failed: %s", qPrintable(q.lastError().text()));
            }
        }
        db.close(); // don't hold the database while Nickel writes to it
    }
    // "11.0" -> "11"
    if (n.endsWith(".0"))
        n.chop(2);
    nfn_log("series: \"%s\" #%s", qPrintable(s), qPrintable(n));
    cache.insert(contentId, QStringList() << s << n);
    *series = s;
    *number = n;
}
