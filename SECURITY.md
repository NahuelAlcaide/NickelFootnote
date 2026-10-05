# Security

## Reporting a vulnerability

Please report security problems privately through GitHub's
[private vulnerability reporting](https://github.com/NahuelAlcaide/NickelFootnote/security/advisories/new),
not in a public issue.

## What NickelFootnote stores, and where

- **Sign-in tokens** (`access_token`, `refresh_token`, `id_token`) are in
  `.adds/nickelfootnote/auth.json` on the Kobo's user storage, which anyone with USB
  access to the device can read. They allow using your ChatGPT plan through
  the Responses API. They don't give access to your ChatGPT conversations.
  On the computer, `tools/signin.py` keeps them in `.secrets/auth.json` until
  you `push` them.
- If you lose the device or think the tokens leaked, remove the app's access
  from your ChatGPT account settings and sign in again.
- `log.txt` never contains tokens or questions. With `log_answers=true` it
  contains answers.
- Network traffic goes only to `auth.openai.com` (token refresh) and
  `api.openai.com` (questions), over HTTPS with the device's OpenSSL.
