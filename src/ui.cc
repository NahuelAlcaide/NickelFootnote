// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QRegularExpression>
#include <QScreen>
#include <QStringList>
#include <QTextEdit>

#include <string.h>

#include "log.h"
#include "nickel.h"
#include "store.h"
#include "ui.h"

NfnView::NfnView(QString const &title, bool fullView)
    : QFrame(), m_keyboard(nullptr), m_receiver_target(nullptr) {
    m_dialog = N3DialogFactory_getDialog(this, true);
    N3Dialog_setTitle(m_dialog, &title);
    if (fullView)
        N3Dialog_enableFullViewMode(m_dialog);
    QRect screen = QApplication::primaryScreen()->geometry();
    m_dialog->setFixedSize(screen.width(), screen.height());
    MainWindowController_pushView(MainWindowController_sharedInstance(), m_dialog);
    connect(m_dialog, SIGNAL(closeTapped()), m_dialog, SLOT(deleteLater()));
    // N3Dialog shows a back arrow on some views (book info, answer); it only
    // emits backTapped().
    connect(m_dialog, SIGNAL(backTapped()), this, SLOT(back()));
    m_dialog->show();
}

void NfnView::close_view() {
    if (m_dialog)
        m_dialog->deleteLater();
}

void NfnView::show_keyboard() {
    if (m_dialog && m_keyboard)
        N3Dialog_showKeyboard(m_dialog);
}

void NfnView::hide_keyboard() {
    if (m_dialog && m_keyboard)
        N3Dialog_hideKeyboard(m_dialog);
}

int NfnView::px(double scale) {
    int h = qMax(QApplication::primaryScreen()->geometry().height(),
                 QApplication::primaryScreen()->geometry().width());
    return qMax(12, qRound(h * 0.0215 * scale));
}

// --- keyboard ----------------------------------------------------------------------

void NfnView::setup_keyboard(QWidget *first, QString const &goText) {
    QWidget *frame = N3Dialog_keyboardFrame(m_dialog);
    QLocale locale(QLocale::English);
    m_keyboard = KeyboardFrame_createKeyboard(frame, 0, &locale);
    if (!m_keyboard) {
        nfn_log("ui: createKeyboard returned null");
        return;
    }
    SearchKeyboardController_setGoText(m_keyboard, &goText);
    connect(m_keyboard, SIGNAL(commitRequested()), this, SLOT(keyboard_go()));
    // As NickelHardcover does for its first field.
    set_receiver(first, false);
    add_field(first);
    connect(qApp, SIGNAL(focusChanged(QWidget*, QWidget*)), this, SLOT(focus_changed(QWidget*, QWidget*)));
}

void NfnView::add_field(QWidget *edit) {
    if (!m_fields.contains(edit))
        m_fields << edit;
}

void NfnView::set_receiver(QWidget *edit, bool nickelStyle) {
    void *mem = ::operator new(KeyboardReceiver_size);
    memset(mem, 0, KeyboardReceiver_size);
    if (QTextEdit *te = qobject_cast<QTextEdit*>(edit)) {
        KeyboardReceiver_ctor_textEdit(mem, te, nickelStyle);
    } else if (QLineEdit *le = qobject_cast<QLineEdit*>(edit)) {
        KeyboardReceiver_ctor_lineEdit(mem, le, nickelStyle);
    } else {
        ::operator delete(mem);
        return;
    }
    // Nickel's own forms (BookInfoReviewController::onViewFocusChanged) make a
    // new receiver for each focused field and pass true.
    SearchKeyboardController_setReceiver(m_keyboard, mem, nickelStyle);
    m_receiver_target = edit;
}

void NfnView::focus_changed(QWidget *, QWidget *now) {
    if (!m_keyboard || !now || now == m_receiver_target)
        return;
    for (QPointer<QWidget> const &f : m_fields) {
        if (f && f == now) {
            nfn_log("ui: keyboard now types into %s", now->metaObject()->className());
            set_receiver(now, true);
            return;
        }
    }
}

void NfnView::keyboard_go() {
    commit();
}

// --- widgets -----------------------------------------------------------------------

QWidget *NfnView::button(QString const &text, bool primary, const char *slot) {
    void *mem = ::operator new(N3ButtonLabel_size);
    memset(mem, 0, N3ButtonLabel_size);
    QWidget *b = N3ButtonLabel_ctor(mem, this);
    if (primary)
        N3ButtonLabel_setPrimaryButton(b, true);
    if (QLabel *l = qobject_cast<QLabel*>(b))
        l->setText(text);
    if (slot)
        connect(b, SIGNAL(tapped(bool)), this, slot);
    return b;
}

QTextEdit *NfnView::text_edit(QString const &placeholder, QWidget **container) {
    void *mem = ::operator new(TouchTextEdit_size);
    memset(mem, 0, TouchTextEdit_size);
    QWidget *touch = TouchTextEdit_ctor(mem, this);
    if (!placeholder.isEmpty())
        TouchTextEdit_setCustomPlaceholderText(touch, &placeholder);
    connect(touch, SIGNAL(tapped()), this, SLOT(show_keyboard()));
    // TouchTextEdit's character counter starts as "0" and is only emptied by
    // the first text change (we set no minimum or maximum), so an empty field
    // would show a stray "0".
    if (QLabel *count = touch->findChild<QLabel*>(QStringLiteral("characterCount")))
        count->clear();
    else
        nfn_log("ui: TouchTextEdit has no characterCount label");
    *container = touch;
    return touch->findChild<QTextEdit*>();
}

QLineEdit *NfnView::line_edit() {
    void *mem = ::operator new(TouchLineEdit_size);
    memset(mem, 0, TouchLineEdit_size);
    QLineEdit *e = TouchLineEdit_ctor(mem, this);
    connect(e, SIGNAL(tapped()), this, SLOT(show_keyboard()));
    return e;
}

// Styles: "body", "bold", "small", "caption" (small caps heading), "italic".
QLabel *NfnView::label(QString const &text, const char *style) {
    QLabel *l = new QLabel(text, this);
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    QString s = QString::fromLatin1(style);
    QString css;
    if (s == "bold")
        css = QStringLiteral("font-size: %1px; font-weight: bold;").arg(px());
    else if (s == "small")
        css = QStringLiteral("font-size: %1px;").arg(px(0.8));
    else if (s == "caption")
        css = QStringLiteral("font-size: %1px; font-weight: bold; color: #555555;").arg(px(0.72));
    else if (s == "italic")
        css = QStringLiteral("font-size: %1px; font-style: italic;").arg(px(0.9));
    else
        css = QStringLiteral("font-size: %1px;").arg(px());
    l->setStyleSheet(css);
    return l;
}

// --- Markdown ------------------------------------------------------------------------

// Tag settings for the markdown_to_html call in progress.
static Config tag_cfg;

static QString tag_colour(QString const &category) {
    if (category == "char") return tag_cfg.tagChar;
    if (category == "place") return tag_cfg.tagPlace;
    if (category == "group") return tag_cfg.tagGroup;
    if (category == "term") return tag_cfg.tagTerm;
    return QString();
}

// [[char:Name]] -> a coloured span. Unknown categories and malformed tags keep
// just the name. Runs on escaped text, before the link regexes.
static QString convert_tags(QString const &line) {
    static const QRegularExpression tag("\\[\\[(?:([A-Za-z]+):)?([^\\[\\]]+?)\\]\\]");
    QString out;
    int last = 0;
    QRegularExpressionMatchIterator it = tag.globalMatch(line);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        out += line.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        QString name = m.captured(2).trimmed();
        QString colour = tag_colour(m.captured(1).toLower());
        if (colour.isEmpty() || tag_cfg.tagStyle == "plain")
            out += name;
        else if (tag_cfg.tagStyle == "bold")
            out += "<b>" + name + "</b>";
        else
            out += "<span style=\"color:" + colour + "; font-weight:bold\">" + name + "</span>";
    }
    return out + line.mid(last);
}

QString tag_swatch_html() {
    Config c = load_config();
    if (c.tagStyle != "test")
        return QString();
    struct { const char *label, *word; QString colour; } rows[] = {
        {"char", "Egwene", c.tagChar},
        {"place", "Tar Valon", c.tagPlace},
        {"group", "Whitecloaks", c.tagGroup},
        {"term", "saidin", c.tagTerm},
    };
    QString html = QStringLiteral("<p><b>Tag colour test</b> (colour, underline, background):</p><table cellpadding=\"3\">");
    for (auto const &r : rows)
        html += QStringLiteral("<tr><td>%1</td><td><span style=\"color:%2; font-weight:bold\">%3</span></td>"
                               "<td><span style=\"color:%2; text-decoration:underline\">%3</span></td>"
                               "<td><span style=\"background-color:%2; color:#ffffff\">&nbsp;%3&nbsp;</span></td></tr>")
                    .arg(QLatin1String(r.label), r.colour, QLatin1String(r.word));
    return html + "</table><hr>";
}

static QString inline_md(QString line) {
    line = line.toHtmlEscaped();
    line = convert_tags(line);
    // Inline citations like ([en.wikipedia.org](https://...)): the sources are listed below.
    line.replace(QRegularExpression("\\s*\\(\\[[^\\]]*\\]\\([^)]*\\)\\)"), QString());
    // Other links: keep the text.
    line.replace(QRegularExpression("\\[([^\\]]+)\\]\\((?:[^)]*)\\)"), "\\1");
    line.replace(QRegularExpression("\\*\\*(.+?)\\*\\*"), "<b>\\1</b>");
    line.replace(QRegularExpression("__(.+?)__"), "<b>\\1</b>");
    line.replace(QRegularExpression("(?<![\\w*])\\*(?!\\s)(.+?)(?<!\\s)\\*(?![\\w*])"), "<i>\\1</i>");
    line.replace(QRegularExpression("(?<![\\w_])_(?!\\s)(.+?)(?<!\\s)_(?![\\w_])"), "<i>\\1</i>");
    line.replace(QRegularExpression("`([^`]+)`"), "\\1");
    return line;
}

QString markdown_to_html(QString const &mdIn) {
    tag_cfg = load_config();
    // A tag still being streamed ("[[char:Eg") must not show its brackets.
    static const QRegularExpression partial_tag("\\[\\[[^\\]\\n]*\\]?$|\\[$");
    QString md = mdIn;
    md.remove(partial_tag);

    static const QRegularExpression bullet("^\\s*[-*\\x{2022}]\\s+(.*)$");
    static const QRegularExpression numbered("^\\s*(\\d+)[.)]\\s+(.*)$");
    static const QRegularExpression heading("^\\s*#{1,6}\\s+(.*)$");

    QString out;
    QString para;
    QString list; // "ul", "ol" or empty
    auto flush_para = [&] {
        if (!para.isEmpty()) {
            out += "<p>" + para + "</p>";
            para.clear();
        }
    };
    auto close_list = [&] {
        if (!list.isEmpty()) {
            out += "</" + list + ">";
            list.clear();
        }
    };
    for (QString const &raw : md.split('\n')) {
        QString line = raw.trimmed();
        if (line.isEmpty() || line == "---" || line == "***") {
            flush_para();
            close_list();
            continue;
        }
        QRegularExpressionMatch m;
        if ((m = bullet.match(raw)).hasMatch() || (m = numbered.match(raw)).hasMatch()) {
            bool ordered = m.capturedTexts().size() == 3;
            QString kind = ordered ? "ol" : "ul";
            flush_para();
            if (list != kind) {
                close_list();
                out += "<" + kind + ">";
                list = kind;
            }
            out += "<li>" + inline_md(m.captured(ordered ? 2 : 1)) + "</li>";
            continue;
        }
        if ((m = heading.match(raw)).hasMatch()) {
            flush_para();
            close_list();
            out += "<p><b>" + inline_md(m.captured(1)) + "</b></p>";
            continue;
        }
        close_list();
        if (!para.isEmpty())
            para += "<br>";
        para += inline_md(line);
    }
    flush_para();
    close_list();
    return out;
}
