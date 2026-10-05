// SPDX-License-Identifier: GPL-3.0-or-later

// The ask flow: entry points (selection menu item, reading menu button), the
// ask view, the book-info editor and the answer view.

#ifndef NICKLEGPT_ASK_H
#define NICKLEGPT_ASK_H

#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "context.h"
#include "store.h"
#include "ui.h"

class QLabel;
class QLineEdit;
class QTextEdit;
class QTimer;
class ChatClient;
class NickleGPT;

// Question, book info (always visible) and the quoted passage.
class AskView : public NgptView {
    Q_OBJECT
public:
    explicit AskView(NickleGPT *app);
protected slots:
    void commit() override;
private slots:
    void edit_tapped();
    void remove_quote();
    void cancel();
private:
    NickleGPT *m_app;
    QTextEdit *m_question;
    QWidget *m_quote_box;
};

// One field per part of the book info. Saved per book.
class BookInfoView : public NgptView {
    Q_OBJECT
public:
    explicit BookInfoView(NickleGPT *app);
protected slots:
    void commit() override;
    void back() override { cancel(); }
private slots:
    void cancel();
    void use_detected();
private:
    NickleGPT *m_app;
    QLineEdit *m_series, *m_number, *m_title, *m_chapter, *m_percent;
};

// The conversation, paged (no scrolling on e-ink), and a follow-up field.
class AnswerView : public NgptView {
    Q_OBJECT
public:
    explicit AnswerView(NickleGPT *app);
    void render(bool toLatest);
    void set_status(QString const &status);
    void text_changed(); // redrawn on the next repaint tick
protected slots:
    void commit() override;
    void back() override;
protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;
private slots:
    void prev_page();
    void next_page();
    void retry();
    void close_tapped();
    void flush();
    void scroll_to_latest();
    void update_pager();
private:
    NickleGPT *m_app;
    QTextEdit *m_view;
    QLabel *m_status;
    QLabel *m_page;
    QWidget *m_prev, *m_next, *m_retry;
    QTextEdit *m_followup;
    QTimer *m_repaint;
    bool m_text_dirty;
    bool m_want_latest; // page to the latest question once the layout settles
};

class NickleGPT : public QObject {
    Q_OBJECT

public:
    static NickleGPT *instance();

    // Appends the Ask item to a selection menu Nickel just filled.
    void add_selection_item(QObject *controller, QWidget *menuView);
    // Adds the Ask button to a reading menu Nickel just built.
    void add_reading_menu_button(QWidget *readingMenuView);

    // For the views
    struct Turn {
        QString question;
        QString quote;   // the selection sent with the first question
        QString answer;
        QStringList sources;
        QString error;   // set when it failed
        bool pending = false;
    };
    ReadingContext const &detected() const { return m_detected; }
    ReadingContext effective(QStringList *edited = nullptr) const;
    BookEdit book_edit() const;
    QString quote() const { return m_quote; }
    QString draft() const { return m_draft; }
    QList<Turn> const &turns() const { return m_turns; }
    bool busy() const;

    void set_draft(QString const &q) { m_draft = q; }
    void drop_quote() { m_quote.clear(); }
    void save_edit(BookEdit const &e);
    void reset_edit();

    void open_ask(NgptView *replacing);
    void open_book_info(NgptView *replacing);
    void ask(QString const &question, NgptView *replacing); // first question
    void follow_up(QString const &question);
    void retry_last();
    void end_conversation();

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private slots:
    void selection_item_tapped();
    void reading_menu_button_tapped();
    void open_ask_later();
    void chat_progress(QString const &status);
    void chat_text(QString const &text);
    void chat_finished(bool ok, QString const &text, QString const &error, QStringList const &sources);

private:
    explicit NickleGPT(QObject *parent = nullptr);
    void start(ReadingContext const &ctx);
    void send();
    QString book_key() const;
    ChatClient *chat();

    ReadingContext m_detected;
    QString m_quote;
    QString m_draft;
    QList<Turn> m_turns;
    QJsonArray m_first_input; // first user message with the reading position (fixed for the conversation)

    QPointer<AnswerView> m_answer;
    ChatClient *m_chat;
};

#endif
