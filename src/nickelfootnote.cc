// SPDX-License-Identifier: GPL-3.0-or-later

// NickelFootnote: ask about the book you're reading (answered by ChatGPT), with
// the series, book and chapter sent along so the answer can stay spoiler-free.
//
// Entry points: "Ask Footnote" in the text selection menu, and a button in the
// reading menu's icon row. Only exported libnickel symbols are used; all of
// them are checked when the plugin loads.

#include <QByteArray>
#include <QDialog>
#include <QLineEdit>
#include <QLocale>
#include <QTextEdit>
#include <QObject>
#include <QString>
#include <QWidget>

#include <stdio.h>
#include <sys/stat.h>

#include <NickelHook.h>

#include "ask.h"
#include "chainhook.h"
#include "log.h"
#include "nickel.h"

Volume const *(*ReadingView_getVolume)(QWidget const *self);
void (*ReadingView_getChapterTitle)(QString *sret, QWidget *self);
int  (*ReadingView_getCalculatedReadProgress)(QWidget *self);
void (*Content_getDbValues)(QVariantMap *sret, Volume const *self);
void (*Content_getId)(QString *sret, Volume const *self);
QString const *Nickel_ATTRIBUTE_TITLE;
QString const *Nickel_ATTRIBUTE_ATTRIBUTION;
QString const *Nickel_ATTRIBUTE_SERIES;
QString const *Nickel_ATTRIBUTE_SERIES_NUMBER;
void (*WebkitView_selectedText)(QString *sret, QWidget *self);
QWidget *(*SelectionMenuController_menuView)(QObject *self);
QWidget *(*SelectionMenuController_createMenuTextItem)(QObject *self, QWidget *parent, QString const *text);
void (*SelectionMenuView_addMenuItem)(QWidget *self, QWidget *item);
QWidget *(*ConfirmationDialogFactory_showOKDialog)(QString const *title, QString const *text);
QDialog *(*N3DialogFactory_getDialog)(QWidget *content, bool flag);
void (*N3Dialog_setTitle)(QDialog *self, QString const *title);
void (*N3Dialog_enableFullViewMode)(QDialog *self);
QWidget *(*N3Dialog_keyboardFrame)(QDialog *self);
void (*N3Dialog_showKeyboard)(QDialog *self);
void (*N3Dialog_hideKeyboard)(QDialog *self);
QObject *(*MainWindowController_sharedInstance)();
void (*MainWindowController_pushView)(QObject *self, QWidget *view);
QObject *(*KeyboardFrame_createKeyboard)(QWidget *self, int keyboardScript, QLocale const *locale);
void (*SearchKeyboardController_setReceiver)(QObject *self, void *receiver, bool flag);
void (*SearchKeyboardController_setGoText)(QObject *self, QString const *text);
void *(*KeyboardReceiver_ctor_lineEdit)(void *self, QLineEdit *edit, bool flag);
void *(*KeyboardReceiver_ctor_textEdit)(void *self, QTextEdit *edit, bool flag);
QWidget *(*TouchLabel_ctor)(void *self, QWidget *parent, int flags);
QWidget *(*N3ButtonLabel_ctor)(void *self, QWidget *parent);
void (*N3ButtonLabel_setPrimaryButton)(QWidget *self, bool primary);
QWidget *(*TouchTextEdit_ctor)(void *self, QWidget *parent);
void (*TouchTextEdit_setCustomPlaceholderText)(QWidget *self, QString const *text);
QLineEdit *(*TouchLineEdit_ctor)(void *self, QWidget *parent);
QObject *(*WirelessWorkflowManager_sharedInstance)();
bool (*WirelessWorkflowManager_isInternetAccessible)(QObject *self);
void (*WirelessWorkflowManager_connectWireless)(QObject *self, bool a, bool b);
QObject *(*WirelessManager_sharedInstance)();

// hooked (originals, or the hook installed before ours)
static void (*SelectionMenuController_setupMainOptions)(QObject *self);
static QWidget *(*ReadingMenuView_ctor)(QWidget *self, QWidget *parent, QByteArray const *name, bool flag);

static const char *const FEATURES[] = {"selection", "menu", "ask", nullptr};
static int hook_log_budget = 20;

// Nickel clears and refills the selection menu every time it shows it.
extern "C" __attribute__((visibility("default")))
void _nfn_setupMainOptions(QObject *self) {
    SelectionMenuController_setupMainOptions(self);
    bool active = !nfn_starting() && nfn_enabled("selection");
    if (hook_log_budget > 0) {
        hook_log_budget--;
        nfn_log("hook: setupMainOptions controller=%p%s", (void*)self, active ? "" : " (passed through)");
    }
    if (!active)
        return;
    nfn_guard_enter("selection", "add item");
    if (QWidget *view = SelectionMenuController_menuView(self))
        Footnote::instance()->add_selection_item(self, view);
    nfn_guard_leave("selection");
}

// ReadingMenuController::loadView builds a new ReadingMenuView each time the
// reading menu opens.
extern "C" __attribute__((visibility("default")))
QWidget *_nfn_ReadingMenuView_ctor(QWidget *self, QWidget *parent, QByteArray const *name, bool flag) {
    QWidget *ret = ReadingMenuView_ctor(self, parent, name, flag);
    bool active = !nfn_starting() && nfn_enabled("menu");
    if (hook_log_budget > 0) {
        hook_log_budget--;
        nfn_log("hook: ReadingMenuView ctor view=%p%s", (void*)self, active ? "" : " (passed through)");
    }
    if (!active)
        return ret;
    nfn_guard_enter("menu", "add button");
    Footnote::instance()->add_reading_menu_button(self);
    nfn_guard_leave("menu");
    return ret;
}

// The plugin was called NickleGPT before 0.1.0. Move its folder (sign-in
// tokens, settings, per-book edits) to the new name once, before anything
// creates the new folder. Returns a note for the log, or null.
static const char *migrate_old_dir() {
    static const char *OLD_DIR = "/mnt/onboard/.adds/nicklegpt";
    struct stat st;
    if (stat(OLD_DIR, &st) != 0 || !S_ISDIR(st.st_mode))
        return nullptr;
    if (stat(NFN_DIR, &st) == 0)
        return "init: both .adds/nicklegpt and .adds/nickelfootnote exist; left the old folder alone";
    return rename(OLD_DIR, NFN_DIR) == 0 ? "init: moved .adds/nicklegpt to .adds/nickelfootnote"
                                         : "init: moving .adds/nicklegpt failed";
}

static int nfn_init() {
    // No Qt objects or blocking work here: init runs while Qt is loading
    // plugins, early in Nickel's startup.
    const char *migrated = migrate_old_dir();
    nfn_log("init: %s, selection hook %s", NFN_VERSION, SelectionMenuController_setupMainOptions ? "on" : "off");
    if (migrated)
        nfn_log("%s", migrated);
    nfn_guard_init(FEATURES);
    ReadingMenuView_ctor = reinterpret_cast<QWidget *(*)(QWidget*, QWidget*, QByteArray const*, bool)>(
        nfn_chain_hook("libnickel.so.1.0.0", "_ZN15ReadingMenuViewC1EP7QWidgetRK10QByteArrayb",
                        reinterpret_cast<void*>(_nfn_ReadingMenuView_ctor)));
    nfn_log("init: done");
    return 0;
}

static struct nh_info FootnoteInfo = {
    .name           = "NickelFootnote",
    .desc           = "Ask about the book you're reading, without spoilers",
    .uninstall_flag  = "/mnt/onboard/.adds/nickelfootnote/uninstall",
    .uninstall_xflag = NULL,
    .failsafe_delay  = 10,
};

static struct nh_hook FootnoteHook[] = {
    {.sym = "_ZN23SelectionMenuController16setupMainOptionsEv", .sym_new = "_nfn_setupMainOptions", .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(SelectionMenuController_setupMainOptions), .desc = "selection menu item", .optional = true},
    // ReadingMenuView's constructor is hooked in nfn_init with nfn_chain_hook
    // instead, so NickelHardcover's hook on it keeps working.
    {0},
};

static struct nh_dlsym FootnoteDlsym[] = {
    // book and position
    {.name = "_ZNK11ReadingView9getVolumeEv",                    .out = nh_symoutptr(ReadingView_getVolume)},
    {.name = "_ZNK7Content11getDbValuesEv",                      .out = nh_symoutptr(Content_getDbValues)},
    {.name = "ATTRIBUTE_TITLE",                                  .out = nh_symoutptr(Nickel_ATTRIBUTE_TITLE)},
    {.name = "ATTRIBUTE_ATTRIBUTION",                            .out = nh_symoutptr(Nickel_ATTRIBUTE_ATTRIBUTION)},
    {.name = "ATTRIBUTE_SERIES",                                 .out = nh_symoutptr(Nickel_ATTRIBUTE_SERIES)},
    {.name = "ATTRIBUTE_SERIES_NUMBER",                          .out = nh_symoutptr(Nickel_ATTRIBUTE_SERIES_NUMBER)},
    {.name = "_ZN11ReadingView15getChapterTitleEv",              .out = nh_symoutptr(ReadingView_getChapterTitle), .desc = "chapter", .optional = true},
    {.name = "_ZN11ReadingView25getCalculatedReadProgressEv",    .out = nh_symoutptr(ReadingView_getCalculatedReadProgress), .desc = "progress", .optional = true},
    {.name = "_ZNK7Content5getIdEv",                             .out = nh_symoutptr(Content_getId), .desc = "book id", .optional = true},
    {.name = "_ZN10WebkitView12selectedTextEv",                  .out = nh_symoutptr(WebkitView_selectedText), .desc = "selected text", .optional = true},
    // selection menu
    {.name = "_ZN23SelectionMenuController8menuViewEv",           .out = nh_symoutptr(SelectionMenuController_menuView)},
    {.name = "_ZN23SelectionMenuController18createMenuTextItemEP7QWidgetRK7QString", .out = nh_symoutptr(SelectionMenuController_createMenuTextItem)},
    {.name = "_ZN17SelectionMenuView11addMenuItemEP12MenuTextItem", .out = nh_symoutptr(SelectionMenuView_addMenuItem)},
    // dialogs and widgets (the recipe NickelHardcover uses on this firmware)
    {.name = "_ZN25ConfirmationDialogFactory12showOKDialogERK7QStringS2_",    .out = nh_symoutptr(ConfirmationDialogFactory_showOKDialog)},
    {.name = "_ZN15N3DialogFactory9getDialogEP7QWidgetb",                    .out = nh_symoutptr(N3DialogFactory_getDialog)},
    {.name = "_ZN8N3Dialog8setTitleERK7QString",                             .out = nh_symoutptr(N3Dialog_setTitle)},
    {.name = "_ZN8N3Dialog18enableFullViewModeEv",                           .out = nh_symoutptr(N3Dialog_enableFullViewMode)},
    {.name = "_ZN8N3Dialog13keyboardFrameEv",                                .out = nh_symoutptr(N3Dialog_keyboardFrame)},
    {.name = "_ZN8N3Dialog12showKeyboardEv",                                 .out = nh_symoutptr(N3Dialog_showKeyboard)},
    {.name = "_ZN8N3Dialog12hideKeyboardEv",                                 .out = nh_symoutptr(N3Dialog_hideKeyboard)},
    {.name = "_ZN20MainWindowController14sharedInstanceEv",                  .out = nh_symoutptr(MainWindowController_sharedInstance)},
    {.name = "_ZN20MainWindowController8pushViewEP7QWidget",                 .out = nh_symoutptr(MainWindowController_pushView)},
    {.name = "_ZN13KeyboardFrame14createKeyboardE14KeyboardScriptRK7QLocale", .out = nh_symoutptr(KeyboardFrame_createKeyboard)},
    {.name = "_ZN24SearchKeyboardController11setReceiverEP16KeyboardReceiverb", .out = nh_symoutptr(SearchKeyboardController_setReceiver)},
    {.name = "_ZN24SearchKeyboardController9setGoTextERK7QString",           .out = nh_symoutptr(SearchKeyboardController_setGoText)},
    {.name = "_ZN16KeyboardReceiverC1EP9QLineEditb",                         .out = nh_symoutptr(KeyboardReceiver_ctor_lineEdit)},
    {.name = "_ZN16KeyboardReceiverC1EP9QTextEditb",                         .out = nh_symoutptr(KeyboardReceiver_ctor_textEdit)},
    {.name = "_ZN10TouchLabelC1EP7QWidget6QFlagsIN2Qt10WindowTypeEE",        .out = nh_symoutptr(TouchLabel_ctor)},
    {.name = "_ZN13N3ButtonLabelC1EP7QWidget",                               .out = nh_symoutptr(N3ButtonLabel_ctor)},
    {.name = "_ZN13N3ButtonLabel16setPrimaryButtonEb",                       .out = nh_symoutptr(N3ButtonLabel_setPrimaryButton)},
    {.name = "_ZN13TouchTextEditC1EP7QWidget",                               .out = nh_symoutptr(TouchTextEdit_ctor)},
    {.name = "_ZN13TouchTextEdit24setCustomPlaceholderTextERK7QString",      .out = nh_symoutptr(TouchTextEdit_setCustomPlaceholderText)},
    {.name = "_ZN13TouchLineEditC1EP7QWidget",                               .out = nh_symoutptr(TouchLineEdit_ctor)},
    // Wi-Fi
    {.name = "_ZN23WirelessWorkflowManager14sharedInstanceEv",               .out = nh_symoutptr(WirelessWorkflowManager_sharedInstance)},
    {.name = "_ZN23WirelessWorkflowManager20isInternetAccessibleEv",         .out = nh_symoutptr(WirelessWorkflowManager_isInternetAccessible)},
    {.name = "_ZN23WirelessWorkflowManager15connectWirelessEbb",             .out = nh_symoutptr(WirelessWorkflowManager_connectWireless)},
    {.name = "_ZN15WirelessManager14sharedInstanceEv",                       .out = nh_symoutptr(WirelessManager_sharedInstance)},
    {0},
};

NickelHook(
    .init  = nfn_init,
    .info  = &FootnoteInfo,
    .hook  = FootnoteHook,
    .dlsym = FootnoteDlsym,
)
