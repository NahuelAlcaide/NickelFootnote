# Developing NickelFootnote

How to build, test and debug the plugin, plus what was learned getting it to
run. Read [Lessons learned](#lessons-learned) before installing a build of
your own: some mistakes look like a bricked device (they aren't).

## Build

Requires Docker and the NickelHook submodule (`git submodule update --init`).

```powershell
.\build.ps1          # libnickelfootnote.so and KoboRoot.tgz
.\build.ps1 clean
```

Or, without PowerShell:

```sh
docker run --rm -v "$PWD:/src" -w /src ghcr.io/pgaskin/nickeltc:1.0 sh -c "make && make koboroot"
```

The version label (in the log and the User-Agent) comes from
`git describe --tags`; `build.ps1` and CI pass it to make as `NFN_VERSION`,
and the Makefile writes it to `src/version.h`. A plain `make` builds as `dev`.

After adding globals, check what the library exports:

```sh
arm-nickel-linux-gnueabihf-nm -D --defined-only libnickelfootnote.so
```

Only `NickelHook`, `_nfn_*`, `nh_*`, `qt_plugin_*`, `_init`/`_fini` and weak
Qt inline functions should be there. CI fails on anything else.

## Code layout

```text
src/nickelfootnote.cc   NickelHook entry: hooks, dlsym table, init
src/nickel.h            resolved libnickel function pointers and struct sizes
src/context.*           reading position (ReadingView, Volume db values, selection)
src/store.*             config.ini, books.json (per-book edits), series from the DB
src/openai.*            ChatClient: Wi-Fi, token refresh, streamed request (moc)
src/ui.*                NfnView: N3Dialog + keyboard base, Nickel widgets, Markdown
src/ask.*               entry points, controller, ask/book info/answer views (moc)
src/log.*               persistent log, startup grace, crash guards
src/chainhook.*         PLT hook that chains to an earlier plugin's hook
tools/signin.py         PC-side sign-in (login/push/pull) and test commands (models/ask)
tools/find_callers.py   call-site scanner for libnickel PLT imports
tools/nickelmenu/       NickelMenu items: debug log dump, plugin recovery
```

The model instructions live in `INSTRUCTIONS` in `src/ask.cc`, mirrored in
`tools/signin.py` so `signin.py ask` can test prompt changes from the PC.
Keep the two identical.

## Device workflow

There is no emulator, so every test runs on a real device.

- Install: copy `KoboRoot.tgz` to `.kobo/` on the device and eject. Check the
  copy (e.g. `md5sum`) before ejecting.
- Plugin files: `.adds/nickelfootnote/` holds `log.txt` (persistent log),
  `config.ini`, `books.json`, `auth.json`, and crash-guard files
  (`guard-*`, `crashed-*.txt`).
- Nickel crash dumps: `.kobo/stack_00.log` (stack of every thread, memory maps,
  syslog). This is the first place to look after a crash.
- Screenshots: NickelMenu can toggle Kobo's screenshot feature.
- Batch changes and log a lot: each round trip to the device is slow.

## Debugging

The plugin logs to syslog with the prefix `NickelFootnote` and to
`.adds/nickelfootnote/log.txt` (fsynced line by line, because syslog lives in RAM
and is lost on a forced power-off). It logs which hooks are active, the
reading menu's widget tree (once), the detected position, views opened, and
each request's steps (Wi-Fi, token refresh, HTTP status, SSL library,
completion). Set `log_answers=true` under `[debug]` in `config.ini` to also log
each answer's raw text, colour tags included.

`tools/nickelmenu/` holds a NickelMenu item that dumps the syslog to
`.adds/nickelfootnote/debug.txt`. Copy its contents into `.adds/nm/` on the device
(giving `.adds/nm/nickelfootnote_debug` and `.adds/nm/scripts/nickelfootnote_debug.sh`);
"NickelFootnote debug log" appears in the main menu after a restart.

## Lessons learned

1. **Never export globals named like libnickel symbols.** Early builds declared
   `QString const *ATTRIBUTE_TITLE` (etc.) with default visibility. ELF symbol
   interposition bound that variable to libnickel's own `ATTRIBUTE_TITLE`, so
   NickelHook's dlsym wrote a pointer into Nickel's string and Nickel crashed
   building the home screen. The Makefile now uses `-fvisibility=hidden` and
   the variables are named `Nickel_ATTRIBUTE_*`.
2. **A crash looks like a hang.** Kobo doesn't restart Nickel; the boot
   animation just stays up. Read `.kobo/stack_00.log`.
3. **A crash early in startup disables other plugins.** NickelHook renames
   each plugin to `*.so.failsafe` while it starts and renames it back after
   its `failsafe_delay`. A crash in that window leaves them renamed and
   NickelHook refuses to activate them. `tools/nickelmenu/recovery/` is a
   one-off NickelMenu item that renames them back and turns NickelFootnote off
   (copy it into `.adds/nm/`, run it once, remove it). It refuses to run in
   the first minute after boot, because every plugin is legitimately renamed
   then.
4. NickelHook's `init` runs while Qt is still loading plugins: do no Qt object
   work or blocking work there.
5. **Two plugins hooking the same symbol:** NickelHook's `nh_hook` takes the
   original from `dlsym`, not from the GOT, so the second hook silently
   bypasses the first. NickelHardcover hooks `ReadingMenuView`'s constructor,
   so NickelFootnote hooks it with `nfn_chain_hook`, which calls the previous GOT
   value. NickelMenu hooks `SelectionMenuController::addMenuItem` (NickelFootnote
   doesn't).
6. Keep wrapping risky new code in the startup grace (`nfn_starting()`) and
   the crash guards (`nfn_guard_enter/leave`).
7. Hooks only work for calls that go through libnickel's PLT. Use
   `tools/find_callers.py` to check that a symbol is actually called that way
   (`setupMainOptions` is only reached by a tail call from `configure()`).
8. **Windows caches the Kobo's FAT.** After the Kobo was unmounted, used and
   mounted again, Windows can show `log.txt` with the new bytes as NULs.
   Eject and reconnect, or read `debug.txt` from the NickelMenu item.
9. Qt is **5.2.1**: no `QTimer::singleShot(ms, functor)` (5.4), no
   `QJsonArray::operator<<`. The context-object lambda `connect` exists.
10. In `ConfirmationDialogFactory::showTextEditDialog` dialogs the accept
    button does not call `QDialog::accept()`: Nickel's controllers connect it
    themselves. That's one reason the views are custom `N3Dialog`s.

## Firmware facts (4.45.23697)

All of these are exported in libnickel's `.dynsym`.

- Open book: the visible widget that `inherits("ReadingView")`.
  `ReadingView::getVolume() const` returns a reference (pointer to a member);
  `getChapterTitle()` returns a QString (sret); `getCalculatedReadProgress()`
  returns the book percentage as an int.
- `Content::getDbValues()` has Title, Attribution, ContentID,
  ChapterIDBookmarked, ___PercentRead, ... but not Series/SeriesNumber for the
  open volume. Those come from `KoboReader.sqlite`.
- Selection: `WebkitView::selectedText()` on the visible child that
  `inherits("WebkitView")` (kepubs). `SelectionMenuController::clearSelection()`
  is a signal.
- Reading menu widget tree: `topToolBarContainer` (time, miniDrawer, battery)
  and `bottomReadingMenuContainer` with `bottomHorizontalLayout` (back label,
  lightIcon, fontIcon, statsIcon, settingsIcon, other plugins' buttons,
  comboButton). Icons are 70x70. `TouchLabel(parent, 0)` emits `tapped(bool)`.
- Allocation sizes from Nickel's own `operator new` calls: `TouchTextEdit`
  0x38, `N3ButtonLabel` 0x98, `KeyboardReceiver` 0x24, `TouchLineEdit` 0x40,
  `TouchLabel` 0x84.
- libnickel links libQt5Network, libQt5Sql and OpenSSL 1.0. Create the
  `QNetworkAccessManager` lazily.

## OpenAI API facts

Verified from the PC with a Plus account (October 2026):

- Sign in with ChatGPT is a browser redirect to
  `http://127.0.0.1:1455/auth/callback`; `ext_agent_host_id` must be
  `urn:uuid:<uuid>`; the callback delivers its parameters in the URL fragment.
  See [SIGN_IN.md](SIGN_IN.md).
- Access tokens last 1 hour; refresh tokens about 30 days and rotate on every
  refresh (`POST https://auth.openai.com/api/accounts/oauth/token`, form
  `grant_type=refresh_token`, `client_id`, `refresh_token`,
  `resource=https://api.openai.com/v1`, no `scope`).
- `POST /v1/responses` requires `"store": false` and `"stream": true`, with
  `input` as an array. Not accepted: `previous_response_id` (resend the
  conversation instead), `temperature`, `max_output_tokens`, `metadata`,
  `user`.
- The `web_search` tool works with plan usage. Typical latency: 10-20 s to
  the first text, 16-27 s in total.
