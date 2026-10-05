# Why sign-in happens on the computer

NickelFootnote uses OpenAI's [Sign in with ChatGPT](https://developers.openai.com/siwc/token-sharing-open-source)
for open-source apps, so questions use your ChatGPT plan. Today the sign-in
runs on a computer (`tools/signin.py login`) and the tokens are then copied to
the Kobo (`push`). This page explains why, and what could make it easier
later. Findings are from October 2026.

## How the sign-in works

`signin.py login` opens OpenAI's authorize page in your browser with a PKCE
challenge, and listens on `http://127.0.0.1:1455/auth/callback`. After you
sign in, the browser is redirected there; the script checks the state,
exchanges the code for tokens and saves them in `.secrets/auth.json`.

`push` copies the tokens to `.adds/nickelfootnote/auth.json` on the Kobo and
deletes them from the computer. The refresh token rotates on every refresh, so
whichever side refreshes first would make the other side's copy useless. The
tokens have to live in exactly one place. `pull` moves them back.

## Why not on the Kobo?

- **The sign-in page can't run in Nickel's browser.** `auth.openai.com` is a
  JavaScript app (ES modules, dynamic `import()`, a bot check). Nickel's
  QtWebKit is from 2013 and runs none of that. A modern browser engine is far
  too large to ship in a plugin.
- **No device-code flow.** OpenAI's OpenID configuration only offers
  `authorization_code` and `refresh_token` grants: there is no "enter this code
  on another device" flow (RFC 8628) for Sign in with ChatGPT apps.
- **The callback must be loopback.** The authorize endpoint only accepts
  `http://127.0.0.1:<port>/auth/callback`. A LAN address (the Kobo) or a
  public relay is rejected, so no server can receive the callback for you.

## A possible easier route: sign in with your phone

This works within those limits but isn't built yet:

1. The Kobo shows a QR code for a small page it serves on the local network
   (`QTcpServer`, only while the sign-in screen is open, at a random path).
2. The phone opens that page, which links to the authorize URL. The Kobo
   builds that URL, keeping the PKCE verifier and state to itself.
3. After signing in, the phone's redirect to `127.0.0.1` fails, but the
   address bar holds the callback URL with the code.
4. The user copies that address and pastes it into the Kobo's page. The Kobo
   checks the state, exchanges the code and saves the tokens.

The verifier never leaves the Kobo, so a code seen on the network is useless
to anyone else, and no third party is involved. Open questions: whether
phones keep the `#fragment` when copying a failed page's address, and Wi-Fi
networks that isolate clients from each other (guest networks, some mesh
routers). The computer route would stay as the fallback.
