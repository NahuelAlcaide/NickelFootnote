# Changelog

All notable changes are listed here. Versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

- The quoted passage in the answer view looks like the one in the ask view:
  a black bar on the left and a "YOU QUOTED" caption.
- Quoted passages keep the book's own quotation marks; NickelFootnote no
  longer adds its own (which doubled them).
- The empty follow-up field no longer shows a stray "0" (Nickel's character
  counter before any text was typed).

## [0.1.0] - 2026-10-05

First public release.

- Ask about the current book from the reading menu, or about a text selection.
- Reading position (series, book, chapter, progress) shown before every
  question and editable per book.
- Streamed answers with web search and sources, follow-up questions, retry.
- Colour tags for characters, places, groups and lore terms.
- `config.ini`: model, reasoning effort, web search, tag style and colours,
  `log_answers` (off by default).
- `tools/signin.py`: Sign in with ChatGPT on the computer; `push`/`pull` find
  the Kobo by themselves on Windows, macOS and Linux.
- Renamed from the pre-release name NickleGPT. An existing `.adds/nicklegpt/`
  folder (sign-in, settings, per-book edits) is moved to
  `.adds/nickelfootnote/` on first start; remove the old plugin first.
