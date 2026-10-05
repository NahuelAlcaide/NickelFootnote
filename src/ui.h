// SPDX-License-Identifier: GPL-3.0-or-later

// Full-screen views built like NickelHardcover's dialogs: the view is the
// content of an N3Dialog (from N3DialogFactory::getDialog) that
// MainWindowController pushes over the book. Deleting the dialog deletes the
// view. The keyboard is the dialog's own KeyboardFrame.

#ifndef NICKELFOOTNOTE_UI_H
#define NICKELFOOTNOTE_UI_H

#include <QDialog>
#include <QFrame>
#include <QList>
#include <QPointer>
#include <QString>

class QLabel;
class QLineEdit;
class QTextEdit;

class NfnView : public QFrame {
    Q_OBJECT

public:
    QDialog *dialog() const { return m_dialog; }

public slots:
    void close_view();
    void show_keyboard();
    void hide_keyboard();

protected slots:
    virtual void commit() {} // the keyboard's Go key
    virtual void back() { close_view(); } // the dialog's back arrow

private slots:
    void keyboard_go();
    void focus_changed(QWidget *old, QWidget *now);

protected:
    NfnView(QString const &title, bool fullView);

    // Creates the dialog's keyboard, typing into `first`. Other fields added
    // with add_field() take the keyboard when they get the focus.
    void setup_keyboard(QWidget *first, QString const &goText);
    void add_field(QWidget *edit);

    // Nickel widgets
    QWidget *button(QString const &text, bool primary, const char *slot);
    QTextEdit *text_edit(QString const &placeholder, QWidget **container);
    QLineEdit *line_edit();
    QLabel *label(QString const &text, const char *style);

    // Font sizes scaled to the screen (about 31 px on the Clara's 1448 px)
    static int px(double scale = 1.0);

    QPointer<QDialog> m_dialog;

private:
    void set_receiver(QWidget *edit, bool nickelStyle);

    QObject *m_keyboard;
    QWidget *m_receiver_target;
    QList<QPointer<QWidget>> m_fields;
};

// The model's Markdown (bold, italics, lists, headings) as Qt rich text.
// Links become their text; inline citations are dropped (sources are listed
// separately).
// [[char:Name]] style tags are drawn per the [tags] section of config.ini
// (re-read on every call).
QString markdown_to_html(QString const &md);

// The sample block shown above the answer when [tags] style=test, else empty.
QString tag_swatch_html();

#endif
