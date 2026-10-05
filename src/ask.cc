// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QScrollBar>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <new>

#include <NickelHook.h>

#include "ask.h"
#include "log.h"
#include "nickel.h"
#include "openai.h"

static const char *SELECTION_CONTROLLER_PROP = "nickelFootnoteController";
static const char *BUTTON_NAME = "nickelFootnoteButton";
static const int MAX_QUOTE_CHARS = 3000;     // sent to the model
static const int SHOWN_QUOTE_CHARS = 280;    // shown in the ask view
static const int REPAINT_INTERVAL_MS = 2500; // e-ink: redraw the streaming answer rarely

static int selection_log_budget = 5;
static int tree_log_budget = 1;

static const char INSTRUCTIONS[] =
    "You answer questions from someone who is in the middle of reading a book series on an e-reader.\n"
    "They want reminders about characters, places, events and lore they have already read about.\n"
    "Never reveal anything that happens after their current position: no later events, deaths, identities,\n"
    "twists, or hints that something matters later. Treat their position as the end of everything they know.\n"
    "Your wording must not hint at the future either. Avoid words that imply a situation will or won't change,\n"
    "such as \"still\", \"yet\", \"so far\", \"for now\", \"remains\", \"currently\", \"at this point\", \"not until\",\n"
    "and don't frame answers relative to their position (\"at your point\", \"as of the prologue\"). State things as\n"
    "plain facts of the story: \"the Wicked Witch of the West rules the Winkies\", not \"the Wicked Witch still rules the Winkies\".\n"
    "If a fair answer would need information from later in the series, say so instead of answering.\n"
    "Use web search to check facts against reliable sources (wikis, chapter summaries), but only use what is\n"
    "established by their current point. Be concise: the answer is read on a small e-ink screen.\n"
    "A quoted passage, if present, is text the reader selected on the page they are reading.\n"
    "Format: short paragraphs, **bold** for emphasis if useful, simple bullet lists. No tables, no links,\n"
    "no headings longer than a few words.\n"
    "Wrap the names of characters, places, groups/factions and lore terms in colour tags: [[char:Dorothy]],\n"
    "[[place:Emerald City]], [[group:Munchkins]], [[term:Silver Shoes]]. Only these four categories. Tag only the\n"
    "important mentions, for example the first mention of each name in an answer. Never put a tag inside\n"
    "another tag or inside **bold** markers.";

Footnote *Footnote::instance() {
    static Footnote *self = new Footnote(qApp);
    return self;
}

// Nothing network-related is created here: the instance may be created while
// Qt is still loading plugins.
Footnote::Footnote(QObject *parent) : QObject(parent), m_chat(nullptr) {}

ChatClient *Footnote::chat() {
    if (!m_chat) {
        m_chat = new ChatClient(this);
        connect(m_chat, SIGNAL(progress(QString)), this, SLOT(chat_progress(QString)));
        connect(m_chat, SIGNAL(textChanged(QString)), this, SLOT(chat_text(QString)));
        connect(m_chat, SIGNAL(finished(bool, QString, QString, QStringList)), this,
                SLOT(chat_finished(bool, QString, QString, QStringList)));
    }
    return m_chat;
}

bool Footnote::busy() const {
    return m_chat && m_chat->busy();
}

// --- entry points --------------------------------------------------------------

void Footnote::add_selection_item(QObject *controller, QWidget *menuView) {
    QString text = QStringLiteral("Ask Footnote");
    QWidget *item = SelectionMenuController_createMenuTextItem(controller, menuView, &text);
    if (!item) {
        nfn_log("selection: createMenuTextItem returned null");
        return;
    }
    item->setObjectName("nickelFootnoteSelectionItem");
    item->setProperty(SELECTION_CONTROLLER_PROP, QVariant::fromValue<QObject*>(controller));
    SelectionMenuView_addMenuItem(menuView, item);
    bool ok = connect(item, SIGNAL(tapped(bool)), this, SLOT(selection_item_tapped()));
    if (selection_log_budget > 0) {
        selection_log_budget--;
        nfn_log("selection: added item=%p view=%p connected=%d", (void*)item, (void*)menuView, ok);
    }
}

void Footnote::selection_item_tapped() {
    QObject *item = sender();
    QObject *controller = item ? item->property(SELECTION_CONTROLLER_PROP).value<QObject*>() : nullptr;
    nfn_log("selection: Ask tapped (controller=%p)", (void*)controller);

    if (!nfn_enabled("ask"))
        return;
    // Read the selection before Nickel clears it.
    nfn_guard_enter("ask", "read context (selection)");
    ReadingContext ctx = read_context(true);
    nfn_guard_leave("ask");

    // SelectionMenuController::clearSelection is a signal: it clears the
    // highlight and closes the menu, like Nickel's own "Undo Highlight".
    if (controller && !QMetaObject::invokeMethod(controller, "clearSelection"))
        nfn_log("selection: clearSelection failed");

    start(ctx);
}

// The NickelFootnote mark: a speech bubble with a footnote asterisk, in the
// thin line style of the reading menu's icons. Coordinates are on the 24-unit
// grid of docs/images/icon.svg; the drawing spans 60% of the side.
static QPixmap ask_icon(int side) {
    QPixmap pm(side, side);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    qreal s = side;
    qreal f = s * 0.6 / 18; // grid unit; the bubble is 18 units wide
    auto at = [s, f](qreal u, qreal v) { return QPointF(s / 2 + (u - 12) * f, s / 2 + (v - 12.25) * f); };
    qreal pen = qMax<qreal>(2, s / 26.0);

    QPainterPath path;
    path.addRoundedRect(QRectF(at(3, 4.5), at(21, 17)), 2 * f, 2 * f);
    QPainterPath tail;
    tail.moveTo(at(11, 16.5));
    tail.lineTo(at(6.8, 20));
    tail.lineTo(at(6.8, 16.5));
    tail.closeSubpath();
    path = path.united(tail);
    p.setPen(QPen(Qt::black, pen, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::white);
    p.drawPath(path);

    // Six-armed asterisk
    p.drawLine(at(12, 8), at(12, 13.2));
    p.drawLine(at(9.75, 9.3), at(14.25, 11.9));
    p.drawLine(at(14.25, 9.3), at(9.75, 11.9));
    return pm;
}

static void log_tree(QWidget *w, int depth, int *budget) {
    if (*budget <= 0)
        return;
    (*budget)--;
    QRect g = w->geometry();
    nfn_log("tree %*s%s \"%s\" %d,%d %dx%d vis=%d", depth * 2, "", w->metaObject()->className(),
           qPrintable(w->objectName()), g.x(), g.y(), g.width(), g.height(), w->isVisible());
    for (QObject *c : w->children())
        if (c->isWidgetType())
            log_tree(static_cast<QWidget*>(c), depth + 1, budget);
}

// The icon row (back label, light, font, stats, settings, NickelHardcover's
// icon, comboButton) is bottomHorizontalLayout. NickelHardcover's hook runs
// before ours and inserts its icon before the last item; ours goes right
// before comboButton, so after Hardcover's.
void Footnote::add_reading_menu_button(QWidget *view) {
    if (tree_log_budget > 0)
        view->installEventFilter(this); // logs the widget tree when first shown

    if (view->findChild<QWidget*>(BUTTON_NAME))
        return;
    QHBoxLayout *row = view->findChild<QHBoxLayout*>("bottomHorizontalLayout");
    if (!row) {
        nfn_log("menu: no bottomHorizontalLayout in %s", view->metaObject()->className());
        return;
    }

    QWidget *combo = view->findChild<QWidget*>("comboButton");
    int index = combo ? row->indexOf(combo) : -1;
    if (index < 0)
        index = row->count() > 0 ? row->count() - 1 : 0;

    QWidget *ref = view->findChild<QWidget*>("settingsIcon");
    if (!ref)
        ref = view->findChild<QWidget*>("fontIcon");
    QSize ref_size = ref ? ref->sizeHint() : QSize();
    if (!ref_size.isValid() || ref_size.width() < 32 || ref_size.height() < 32)
        ref_size = QSize(70, 70);

    QWidget *container = row->parentWidget() ? row->parentWidget() : view;
    void *mem = ::operator new(TouchLabel_size);
    QWidget *btn = TouchLabel_ctor(mem, container, 0);
    btn->setObjectName(BUTTON_NAME);
    if (QLabel *label = qobject_cast<QLabel*>(btn)) {
        label->setPixmap(ask_icon(qMin(ref_size.width(), ref_size.height())));
        label->setAlignment(Qt::AlignCenter);
    }
    btn->setFixedSize(ref_size);
    row->insertWidget(index, btn);
    bool ok = connect(btn, SIGNAL(tapped(bool)), this, SLOT(reading_menu_button_tapped()));
    nfn_log("menu: button added at %d of %d (combo=%d ref=%s %dx%d) connected=%d", index, row->count(),
             combo != nullptr, ref ? qPrintable(ref->objectName()) : "none", ref_size.width(), ref_size.height(), ok);
}

bool Footnote::eventFilter(QObject *obj, QEvent *ev) {
    if (ev->type() == QEvent::Show && obj->isWidgetType() && tree_log_budget > 0) {
        tree_log_budget--;
        int budget = 150;
        log_tree(static_cast<QWidget*>(obj), 0, &budget);
        obj->removeEventFilter(this);
    }
    return false;
}

void Footnote::reading_menu_button_tapped() {
    nfn_log("menu: Ask tapped");
    if (!nfn_enabled("ask"))
        return;
    nfn_guard_enter("ask", "read context");
    ReadingContext ctx = read_context(false);
    nfn_guard_leave("ask");
    start(ctx);
}

// --- state ---------------------------------------------------------------------

void Footnote::start(ReadingContext const &ctx) {
    end_conversation();
    m_detected = ctx;
    m_quote = ctx.selection.left(MAX_QUOTE_CHARS);
    m_draft.clear();
    // Let Nickel finish closing the menu first.
    QTimer::singleShot(0, this, SLOT(open_ask_later()));
}

void Footnote::open_ask_later() {
    open_ask(nullptr);
}

QString Footnote::book_key() const {
    return m_detected.contentId.isEmpty() ? m_detected.title : m_detected.contentId;
}

BookEdit Footnote::book_edit() const {
    BookEdit e;
    if (!book_key().isEmpty())
        load_book_edit(book_key(), &e);
    return e;
}

ReadingContext Footnote::effective(QStringList *edited) const {
    return apply_book_edit(m_detected, book_edit(), edited);
}

void Footnote::save_edit(BookEdit const &e) {
    if (book_key().isEmpty())
        return;
    BookEdit clean = e;
    // Only keep what differs from what Nickel reports.
    if (clean.series == m_detected.series) clean.series.clear();
    if (clean.number == m_detected.seriesNumber) clean.number.clear();
    if (clean.title == m_detected.title) clean.title.clear();
    if (clean.chapter == m_detected.chapter) clean.chapter.clear();
    if (clean.percent == m_detected.percent) clean.percent = -1;
    clean.savedAtChapter = m_detected.chapter;
    if (clean.series.isEmpty() && clean.number.isEmpty() && clean.title.isEmpty() && clean.chapter.isEmpty() &&
        clean.percent < 0) {
        clear_book_edit(book_key());
        nfn_log("book info: reset");
    } else {
        save_book_edit(book_key(), clean);
        nfn_log("book info: saved");
    }
}

void Footnote::reset_edit() {
    if (!book_key().isEmpty())
        clear_book_edit(book_key());
    nfn_log("book info: reset");
}

// Opens the new view, then closes `replacing`, in the order NickelHardcover
// uses to go from one dialog to the next.
void Footnote::open_ask(NfnView *replacing) {
    nfn_guard_enter("ask", "ask view");
    AskView *v = new AskView(this);
    nfn_guard_leave("ask");
    nfn_log("view: ask (%p)", (void*)v);
    if (replacing)
        replacing->close_view();
}

void Footnote::open_book_info(NfnView *replacing) {
    nfn_guard_enter("ask", "book info view");
    BookInfoView *v = new BookInfoView(this);
    nfn_guard_leave("ask");
    nfn_log("view: book info (%p)", (void*)v);
    if (replacing)
        replacing->close_view();
}

// --- the conversation ------------------------------------------------------------

static QString position_text(ReadingContext const &c) {
    QStringList parts;
    if (!c.series.isEmpty())
        parts << QStringLiteral("Series: ") + c.series +
                     (c.seriesNumber.isEmpty() ? QString() : QStringLiteral(", book ") + c.seriesNumber);
    if (!c.title.isEmpty())
        parts << QStringLiteral("Book: ") + c.title +
                     (c.author.isEmpty() ? QString() : QStringLiteral(" (by ") + c.author + ")");
    if (!c.chapter.isEmpty())
        parts << QStringLiteral("Current chapter: ") + c.chapter;
    if (c.percent >= 0)
        parts << QStringLiteral("Progress in this book: %1%").arg(c.percent);
    if (parts.isEmpty())
        return QStringLiteral("Reading position: unknown (assume they may be anywhere; avoid spoilers).");
    return QStringLiteral("Reading position:\n") + parts.join("\n");
}

static QJsonObject message(const char *role, QString const &text) {
    QJsonObject m;
    m.insert("role", QString::fromLatin1(role));
    m.insert("content", text);
    return m;
}

void Footnote::ask(QString const &question, NfnView *replacing) {
    ReadingContext c = effective();
    QString first = position_text(c) + "\n\n";
    if (!m_quote.isEmpty())
        first += QStringLiteral("Quoted passage from the book (selected by the reader):\n\"\"\"\n") + m_quote +
                 "\n\"\"\"\n\n";
    first += QStringLiteral("Question: ") + question;
    m_first_input = QJsonArray();
    m_first_input.append(message("user", first));

    nfn_log("ask: question %d chars, quote %d chars, position \"%s\"", question.size(), m_quote.size(),
             qPrintable(c.summary().left(120)));
    Turn t;
    t.question = question;
    t.quote = m_quote;
    t.pending = true;
    m_turns.clear();
    m_turns << t;
    m_draft.clear();

    nfn_guard_enter("ask", "answer view");
    m_answer = new AnswerView(this);
    nfn_guard_leave("ask");
    if (replacing)
        replacing->close_view();
    send();
}

void Footnote::follow_up(QString const &question) {
    if (busy() || m_turns.isEmpty())
        return;
    nfn_log("ask: follow-up %d chars", question.size());
    Turn t;
    t.question = question;
    t.pending = true;
    m_turns << t;
    send();
}

void Footnote::retry_last() {
    if (busy() || m_turns.isEmpty() || m_turns.last().error.isEmpty())
        return;
    Turn &t = m_turns.last();
    t.error.clear();
    t.answer.clear();
    t.sources.clear();
    t.pending = true;
    send();
}

// The input is the first message (with the position) followed by the
// answered turns; failed turns are left out.
void Footnote::send() {
    QJsonArray input;
    for (int i = 0; i < m_turns.size(); i++) {
        Turn const &t = m_turns[i];
        bool last = i == m_turns.size() - 1;
        if (!last && !t.error.isEmpty()) {
            if (i == 0) // keep the reading position even if the first answer failed
                input.append(m_first_input.at(0));
            continue;
        }
        if (i == 0)
            input.append(m_first_input.at(0));
        else
            input.append(message("user", t.question));
        if (!last)
            input.append(message("assistant", t.answer));
    }
    if (m_answer) {
        m_answer->set_status(QStringLiteral("Starting..."));
        m_answer->render(true);
    }
    chat()->send(input, QString::fromUtf8(INSTRUCTIONS));
}

void Footnote::end_conversation() {
    if (m_chat)
        m_chat->cancel();
    m_turns.clear();
    m_first_input = QJsonArray();
}

void Footnote::chat_progress(QString const &status) {
    if (m_answer)
        m_answer->set_status(status);
}

void Footnote::chat_text(QString const &text) {
    if (m_turns.isEmpty())
        return;
    m_turns.last().answer = text;
    if (m_answer) {
        m_answer->set_status(QStringLiteral("Writing..."));
        m_answer->text_changed();
    }
}

void Footnote::chat_finished(bool ok, QString const &text, QString const &error, QStringList const &sources) {
    if (m_turns.isEmpty())
        return;
    Turn &t = m_turns.last();
    t.pending = false;
    t.answer = text;
    t.sources = sources;
    t.error = ok ? QString() : (error.isEmpty() ? QStringLiteral("Unknown error.") : error);
    // The raw answer (tags and Markdown included) for debugging, only when
    // config.ini asks for it; lines are limited in length.
    if (load_config().logAnswers) {
        QString raw = QString(text).replace('\n', QStringLiteral("\\n"));
        for (int i = 0; i < raw.size(); i += 150)
            nfn_log("answer[%d]: %s", i / 150, raw.mid(i, 150).toUtf8().constData());
    }
    if (m_answer) {
        m_answer->set_status(ok ? QString() : QStringLiteral("Failed."));
        m_answer->render(true);
    }
}

// --- ask view --------------------------------------------------------------------------

static QString line1(ReadingContext const &c) {
    QStringList p;
    if (!c.series.isEmpty())
        p << (c.seriesNumber.isEmpty() ? c.series : c.series + " #" + c.seriesNumber);
    if (!c.title.isEmpty())
        p << c.title;
    return p.join(QString::fromUtf8(" \xc2\xb7 "));
}

static QString line2(ReadingContext const &c) {
    QStringList p;
    if (!c.chapter.isEmpty())
        p << c.chapter;
    if (c.percent >= 0)
        p << QString::number(c.percent) + "%";
    return p.join(QString::fromUtf8(" \xc2\xb7 "));
}

AskView::AskView(Footnote *app) : NfnView(QStringLiteral("Ask Footnote"), false), m_app(app), m_quote_box(nullptr) {
    int m = px(0.8);
    QVBoxLayout *col = new QVBoxLayout(this);
    col->setContentsMargins(m, m / 2, m, m / 2);
    col->setSpacing(m / 2);

    // What will be sent: always visible.
    QStringList edited;
    ReadingContext c = app->effective(&edited);
    QFrame *card = new QFrame(this);
    card->setObjectName("nfnCard");
    card->setStyleSheet(QStringLiteral("QFrame#nfnCard { border: 2px solid black; border-radius: %1px; }").arg(m / 2));
    QHBoxLayout *cardRow = new QHBoxLayout(card);
    cardRow->setContentsMargins(m / 2, m / 3, m / 2, m / 3);
    QVBoxLayout *info = new QVBoxLayout();
    info->setSpacing(2);
    info->addWidget(label(edited.isEmpty() ? QStringLiteral("SENT WITH YOUR QUESTION")
                                           : QStringLiteral("SENT WITH YOUR QUESTION (EDITED: %1)")
                                                 .arg(edited.join(", ").toUpper()),
                          "caption"));
    QString l1 = line1(c), l2 = line2(c);
    if (l1.isEmpty() && l2.isEmpty()) {
        info->addWidget(label(QStringLiteral("No book detected. Tap Edit to say where you are."), "italic"));
    } else {
        if (!l1.isEmpty())
            info->addWidget(label(l1, "bold"));
        if (c.series.isEmpty())
            info->addWidget(label(QStringLiteral("Series unknown: tap Edit to add it."), "italic"));
        if (!l2.isEmpty())
            info->addWidget(label(l2, "small"));
    }
    cardRow->addLayout(info, 1);
    cardRow->addWidget(button(QStringLiteral("Edit"), false, SLOT(edit_tapped())), 0, Qt::AlignVCenter);
    col->addWidget(card);

    // The selection, as a quote (read-only), not in the question.
    QString quote = app->quote();
    if (!quote.isEmpty()) {
        m_quote_box = new QFrame(this);
        m_quote_box->setObjectName("nfnQuote");
        m_quote_box->setStyleSheet(QStringLiteral("QFrame#nfnQuote { border-left: 4px solid black; }"));
        QHBoxLayout *qrow = new QHBoxLayout(m_quote_box);
        qrow->setContentsMargins(m / 2, 0, 0, 0);
        QVBoxLayout *qcol = new QVBoxLayout();
        qcol->setSpacing(2);
        qcol->addWidget(label(QStringLiteral("QUOTED PASSAGE (SENT ALONG)"), "caption"));
        QString shown = quote.size() > SHOWN_QUOTE_CHARS ? quote.left(SHOWN_QUOTE_CHARS) + QString::fromUtf8("\xe2\x80\xa6")
                                                         : quote;
        qcol->addWidget(label(QString::fromUtf8("\xe2\x80\x9c") + shown + QString::fromUtf8("\xe2\x80\x9d"), "italic"));
        qrow->addLayout(qcol, 1);
        qrow->addWidget(button(QStringLiteral("Remove"), false, SLOT(remove_quote())), 0, Qt::AlignTop);
        col->addWidget(m_quote_box);
    }

    QWidget *box;
    m_question = text_edit(QStringLiteral("Your question"), &box);
    if (m_question) {
        m_question->setPlainText(app->draft());
        m_question->setStyleSheet(QStringLiteral("font-size: %1px;").arg(px()));
        m_question->moveCursor(QTextCursor::End);
    }
    box->setMinimumHeight(px() * 4);
    col->addWidget(box, 1);

    QHBoxLayout *buttons = new QHBoxLayout();
    buttons->addStretch(1);
    buttons->addWidget(button(QStringLiteral("Cancel"), false, SLOT(cancel())));
    buttons->addWidget(button(QStringLiteral("Ask"), true, SLOT(commit())));
    col->addLayout(buttons);

    if (m_question) {
        setup_keyboard(m_question, QStringLiteral("Ask"));
        m_question->setFocus();
        show_keyboard();
    }
}

void AskView::commit() {
    QString q = m_question ? m_question->toPlainText().trimmed() : QString();
    nfn_log("ask view: commit (%d chars)", q.size());
    if (q.isEmpty())
        return;
    hide_keyboard();
    m_app->ask(q, this);
}

void AskView::edit_tapped() {
    if (m_question)
        m_app->set_draft(m_question->toPlainText());
    m_app->open_book_info(this);
}

void AskView::remove_quote() {
    m_app->drop_quote();
    if (m_quote_box)
        m_quote_box->hide();
}

void AskView::cancel() {
    nfn_log("ask view: cancel");
    close_view();
}

// --- book info view ------------------------------------------------------------------------

BookInfoView::BookInfoView(Footnote *app) : NfnView(QStringLiteral("Book info"), false), m_app(app) {
    int m = px(0.8);
    QVBoxLayout *col = new QVBoxLayout(this);
    col->setContentsMargins(m, m / 2, m, m / 2);
    col->setSpacing(m / 2);

    col->addWidget(label(QStringLiteral("Sent with your questions. Changes are kept for this book; the chapter and "
                                        "progress only until Nickel shows another chapter."),
                         "small"));

    ReadingContext c = app->effective();
    ReadingContext d = app->detected();
    QFormLayout *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setVerticalSpacing(m / 2);
    auto add = [&](const char *name, QString const &value, QString const &placeholder) {
        QLineEdit *e = line_edit();
        e->setText(value);
        e->setPlaceholderText(placeholder);
        e->setStyleSheet(QStringLiteral("font-size: %1px;").arg(px()));
        form->addRow(label(QString::fromLatin1(name), "small"), e);
        return e;
    };
    m_series = add("Series", c.series, d.series.isEmpty() ? QStringLiteral("e.g. The Wheel of Time") : d.series);
    m_number = add("Number", c.seriesNumber, QStringLiteral("e.g. 11"));
    m_title = add("Book", c.title, d.title);
    m_chapter = add("Chapter", c.chapter, d.chapter);
    m_percent = add("Progress %", c.percent >= 0 ? QString::number(c.percent) : QString(), QStringLiteral("0-100"));
    m_number->setInputMethodHints(Qt::ImhDigitsOnly);
    m_percent->setInputMethodHints(Qt::ImhDigitsOnly);
    col->addLayout(form);

    QString detected = d.summary();
    col->addWidget(label(QStringLiteral("Detected: ") + (detected.isEmpty() ? QStringLiteral("nothing") : detected),
                         "small"));
    col->addStretch(1);

    QHBoxLayout *buttons = new QHBoxLayout();
    buttons->addWidget(button(QStringLiteral("Use detected"), false, SLOT(use_detected())));
    buttons->addStretch(1);
    buttons->addWidget(button(QStringLiteral("Cancel"), false, SLOT(cancel())));
    buttons->addWidget(button(QStringLiteral("Save"), true, SLOT(commit())));
    col->addLayout(buttons);

    setup_keyboard(m_series, QStringLiteral("Save"));
    for (QLineEdit *e : {m_number, m_title, m_chapter, m_percent})
        add_field(e);
    m_series->setFocus();
    show_keyboard();
}

void BookInfoView::commit() {
    BookEdit e;
    e.series = m_series->text().simplified();
    e.number = m_number->text().simplified();
    e.title = m_title->text().simplified();
    e.chapter = m_chapter->text().simplified();
    bool ok;
    int pct = m_percent->text().trimmed().remove('%').toInt(&ok);
    e.percent = ok ? qBound(0, pct, 100) : -1;
    hide_keyboard();
    m_app->save_edit(e);
    m_app->open_ask(this);
}

void BookInfoView::use_detected() {
    hide_keyboard();
    m_app->reset_edit();
    m_app->open_ask(this);
}

void BookInfoView::cancel() {
    hide_keyboard();
    m_app->open_ask(this);
}

// --- answer view --------------------------------------------------------------------------------

AnswerView::AnswerView(Footnote *app)
    : NfnView(QStringLiteral("Footnote"), false), m_app(app), m_followup(nullptr),
      m_text_dirty(false), m_want_latest(false) {
    int m = px(0.8);
    QVBoxLayout *col = new QVBoxLayout(this);
    col->setContentsMargins(m, m / 3, m, m / 3);
    col->setSpacing(m / 3);

    ReadingContext c = app->effective();
    QString where = line1(c);
    QString l2 = line2(c);
    if (!l2.isEmpty())
        where += (where.isEmpty() ? QString() : QString::fromUtf8(" \xc2\xb7 ")) + l2;
    QLabel *pos = label(where.isEmpty() ? QStringLiteral("Position unknown") : where, "caption");
    col->addWidget(pos);

    m_view = new QTextEdit(this);
    m_view->setReadOnly(true);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setTextInteractionFlags(Qt::NoTextInteraction);
    m_view->setFocusPolicy(Qt::NoFocus);
    m_view->setStyleSheet(QStringLiteral("QTextEdit { font-size: %1px; background: white; border: none; }").arg(px()));
    m_view->installEventFilter(this); // resizes: redo the paging
    col->addWidget(m_view, 1);

    m_status = label(QString(), "italic");
    col->addWidget(m_status);

    QHBoxLayout *pager = new QHBoxLayout();
    m_prev = button(QString::fromUtf8("\xe2\x80\xb9 Previous"), false, SLOT(prev_page()));
    m_next = button(QString::fromUtf8("Next \xe2\x80\xba"), false, SLOT(next_page()));
    m_page = label(QString(), "small");
    m_page->setAlignment(Qt::AlignCenter);
    pager->addWidget(m_prev);
    pager->addWidget(m_page, 1);
    pager->addWidget(m_next);
    col->addLayout(pager);

    QHBoxLayout *ask = new QHBoxLayout();
    QWidget *box;
    m_followup = text_edit(QStringLiteral("Follow-up question"), &box);
    if (m_followup)
        m_followup->setStyleSheet(QStringLiteral("font-size: %1px;").arg(px()));
    // Room for two lines inside TouchTextEdit's own padding and arrows.
    box->setFixedHeight(px() * 5);
    ask->addWidget(box, 1);
    m_retry = button(QStringLiteral("Retry"), false, SLOT(retry()));
    ask->addWidget(m_retry);
    ask->addWidget(button(QStringLiteral("Ask"), true, SLOT(commit())));
    col->addLayout(ask);

    m_repaint = new QTimer(this);
    m_repaint->setInterval(REPAINT_INTERVAL_MS);
    connect(m_repaint, SIGNAL(timeout()), this, SLOT(flush()));
    m_repaint->start();

    // The keyboard only comes up when the follow-up field is tapped.
    if (m_followup)
        setup_keyboard(m_followup, QStringLiteral("Ask"));
    connect(m_dialog, SIGNAL(closeTapped()), this, SLOT(close_tapped()));
    render(true);
}

void AnswerView::back() {
    close_tapped();
    close_view();
}

bool AnswerView::eventFilter(QObject *obj, QEvent *ev) {
    if (obj == m_view && ev->type() == QEvent::Resize) {
        if (m_want_latest)
            QTimer::singleShot(0, this, SLOT(scroll_to_latest()));
        else
            QTimer::singleShot(0, this, SLOT(update_pager()));
    }
    return NfnView::eventFilter(obj, ev);
}

void AnswerView::close_tapped() {
    nfn_log("answer view: closed");
    m_app->end_conversation();
}

void AnswerView::set_status(QString const &status) {
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
}

void AnswerView::text_changed() {
    m_text_dirty = true;
}

// Builds the whole conversation. With toLatest, pages to the start of the
// latest answer.
void AnswerView::render(bool toLatest) {
    m_text_dirty = false;
    QString html = tag_swatch_html();
    QList<Footnote::Turn> const &turns = m_app->turns();
    for (int i = 0; i < turns.size(); i++) {
        Footnote::Turn const &t = turns[i];
        if (i > 0)
            html += "<hr>";
        html += QStringLiteral("<p><b>You:</b> ") + t.question.toHtmlEscaped().replace("\n", "<br>") + "</p>";
        if (!t.quote.isEmpty()) {
            QString q = t.quote.size() > SHOWN_QUOTE_CHARS * 2
                            ? t.quote.left(SHOWN_QUOTE_CHARS * 2) + QString::fromUtf8("\xe2\x80\xa6")
                            : t.quote;
            html += QStringLiteral("<p style=\"margin-left: %1px; font-size: %2px; color: #444444;\">"
                                   "<i>Quoted: \xe2\x80\x9c").arg(px()).arg(px(0.85)) +
                    q.toHtmlEscaped() + QString::fromUtf8("\xe2\x80\x9d</i></p>");
        }
        if (!t.answer.isEmpty())
            html += markdown_to_html(t.answer);
        else if (t.pending)
            html += QStringLiteral("<p><i>Waiting for the answer...</i></p>");
        if (!t.error.isEmpty())
            html += QStringLiteral("<p><i>Couldn't get an answer: ") + t.error.toHtmlEscaped().replace("\n", "<br>") +
                    "</i></p>";
        if (!t.sources.isEmpty())
            html += QStringLiteral("<p style=\"font-size: %1px; color: #555555;\">Sources: ").arg(px(0.75)) +
                    t.sources.join("; ").toHtmlEscaped() + "</p>";
    }
    int keep = m_view->verticalScrollBar()->value();
    m_view->setHtml(html);
    if (toLatest) {
        m_want_latest = true;
        scroll_to_latest();
    } else {
        m_view->verticalScrollBar()->setValue(keep);
    }

    bool failed = !turns.isEmpty() && !turns.last().error.isEmpty();
    m_retry->setVisible(failed && !m_app->busy());
    update_pager();
}

// The streamed answer is redrawn at most every REPAINT_INTERVAL_MS (e-ink),
// keeping the page the reader is on.
void AnswerView::flush() {
    if (m_text_dirty)
        render(false);
}

// Pages to the top of the latest question.
void AnswerView::scroll_to_latest() {
    QTextBlock found;
    for (QTextBlock b = m_view->document()->begin(); b.isValid(); b = b.next())
        if (b.text().startsWith(QStringLiteral("You:")))
            found = b;
    int y = found.isValid() ? qRound(m_view->document()->documentLayout()->blockBoundingRect(found).top()) : 0;
    m_view->verticalScrollBar()->setValue(y);
    update_pager();
}

static int page_step(QTextEdit *v) {
    int line = v->fontMetrics().lineSpacing();
    return qMax(line, v->viewport()->height() - line);
}

void AnswerView::update_pager() {
    QScrollBar *sb = m_view->verticalScrollBar();
    int step = page_step(m_view);
    int total = sb->maximum() / step + (sb->maximum() % step ? 1 : 0) + 1;
    int current = qMin(total, sb->value() / step + (sb->value() % step ? 1 : 0) + 1);
    m_prev->setVisible(sb->value() > 0);
    m_next->setVisible(sb->value() < sb->maximum());
    m_page->setText(total > 1 ? QStringLiteral("%1 / %2").arg(current).arg(total) : QString());
}

void AnswerView::prev_page() {
    m_want_latest = false;
    QScrollBar *sb = m_view->verticalScrollBar();
    sb->setValue(qMax(0, sb->value() - page_step(m_view)));
    update_pager();
}

void AnswerView::next_page() {
    m_want_latest = false;
    QScrollBar *sb = m_view->verticalScrollBar();
    sb->setValue(qMin(sb->maximum(), sb->value() + page_step(m_view)));
    update_pager();
}

void AnswerView::retry() {
    m_app->retry_last();
}

void AnswerView::commit() {
    if (!m_followup)
        return;
    QString q = m_followup->toPlainText().trimmed();
    nfn_log("answer view: follow-up commit (%d chars, busy=%d)", q.size(), m_app->busy());
    if (q.isEmpty() || m_app->busy())
        return;
    m_followup->clear();
    hide_keyboard();
    m_app->follow_up(q);
}
