# Architecture

NickelFootnote is a Qt plugin that NickelHook loads into Nickel, Kobo's reading
software. It uses only symbols exported by `libnickel.so.1.0.0` and resolves
all of them when it loads. A feature whose symbols are missing stays off
instead of crashing.

Target: firmware 4.45.23697 (Kobo Clara Colour). Qt is 5.2.1.

## Entry points

- **Selection menu item.** `SelectionMenuController::setupMainOptions()` is
  hooked. Nickel clears and refills the selection menu there every time; the
  plugin appends its item with `createMenuTextItem` and
  `SelectionMenuView::addMenuItem`. On tap, the selected text comes from
  `WebkitView::selectedText()` (kepubs only), and invoking the
  `clearSelection()` signal closes the menu, like Nickel's own items.
- **Reading menu button.** `ReadingView`'s menu is built by
  `ReadingMenuView`'s constructor, called from `ReadingMenuController::loadView`
  each time the menu opens. NickelHardcover hooks the same constructor, and
  NickelHook's `nh_hook` takes the original function from `dlsym`, so a second
  plain hook would silently bypass the first. NickelFootnote uses its own PLT hook
  (`src/chainhook.cc`) that calls whatever was in the GOT before it. The button
  is a Nickel `TouchLabel` inserted into `bottomHorizontalLayout` before
  `comboButton`.

## Views

The ask, book info and answer views are full-screen `N3Dialog`s, built the way
NickelHardcover builds its own: `N3DialogFactory::getDialog(content)`, then
`MainWindowController::pushView`. Each view uses the dialog's `KeyboardFrame`
with a `KeyboardReceiver` per field and Nickel's own `TouchTextEdit`,
`TouchLineEdit` and `N3ButtonLabel` widgets. Views replace each other (open
the new one, then `deleteLater` the old dialog); they are never stacked.

- **Ask view:** the book info card (series #n · book, chapter · %, edited
  fields marked, Edit button), the quoted passage with Remove, the question
  field, Cancel and Ask. The keyboard's Go key also asks.
- **Book info view:** series, number, book, chapter and progress fields, "Use
  detected", Cancel, Save.
- **Answer view:** the conversation as rich text (Markdown converted to HTML),
  paged with Previous/Next buttons instead of scrolling (e-ink), a status line,
  sources per answer, a follow-up field and Retry after a failure. While
  streaming it redraws at most every 2.5 s.

## Reading position

From the visible widget that `inherits("ReadingView")`:

- `getVolume()` returns the open book; title and author come from
  `Content::getDbValues()`.
- Series name and number are not in those values. They come from
  `KoboReader.sqlite` (`select Series, SeriesNumber from content where
  ContentID=? and ContentType=6`) through a separate read-only Qt SQL
  connection, cached per book.
- `getChapterTitle()` and `getCalculatedReadProgress()` give the chapter and
  the book percentage.

The reader's corrections are stored per book in `books.json`. Series, number
and title stay until reset. Chapter and progress only apply while Nickel still
reports the chapter that was showing when they were saved.

## Requests

`src/openai.cc`:

1. Wi-Fi: if `isInternetAccessible` is false, `WirelessWorkflowManager`
   connects and the request waits for `networkConnected()` (45 s).
2. Token refresh when the access token is within 120 s of expiry. The new
   tokens are written atomically (tmp file, fsync, rename) before they are
   used, because the refresh token rotates. One retry with a refresh on
   HTTP 401.
3. `POST https://api.openai.com/v1/responses`, streamed as server-sent events,
   with `store: false`, the whole conversation as `input`, the instructions,
   and the `web_search` tool unless turned off. 150 s timeout, plus a watchdog
   for streams that stall.

Events used: `response.output_text.delta`, `response.output_item.added/done`
(web search calls, for the status line), `response.output_text.annotation.added`
(sources), `response.completed`, `response.failed`, `response.incomplete`,
`error`.

## Colour tags

The instructions ask the model to wrap names as `[[char:Name]]`,
`[[place:..]]`, `[[group:..]]` and `[[term:..]]`. `inline_md()` in
`src/ui.cc` turns them into coloured `<span>`s after HTML escaping. Unknown or
malformed tags show just the name, and a tag still streaming in is hidden.
Stored answers keep the tags, so follow-ups send them back as they were.

## Safety

- **Startup grace:** for 30 s after init, the hooks only pass calls through.
  A crash early in Nickel's startup would leave other plugins disabled
  (NickelHook renames each plugin while it starts).
- **Crash guards:** risky steps mark themselves with a file on disk
  (`nfn_guard_enter/leave`). If Nickel dies in between, the next start finds
  the file, renames it to `crashed-<feature>.txt` and keeps that feature off
  until the file is deleted.
- **Hidden symbols:** the library is built with `-fvisibility=hidden`; see
  [DEVELOPING.md](DEVELOPING.md#lessons-learned) for why that matters.
