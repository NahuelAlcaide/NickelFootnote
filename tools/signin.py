#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sign in with ChatGPT on the PC and move the tokens to the Kobo for NickelFootnote.

Standard library only. Commands:

  python tools/signin.py login     # browser sign-in, saves .secrets/auth.json
  python tools/signin.py push      # move the tokens to the Kobo (found automatically, or --drive)
  python tools/signin.py pull      # move them back from the Kobo to the PC
  python tools/signin.py models    # list the models your plan allows
  python tools/signin.py ask "Who is the Tin Woodman?" --series "Oz" --number 1 \
        --book "The Wonderful Wizard of Oz" --chapter "Chapter 5" --percent 20
  python tools/signin.py ask "..." --no-search   # same question without web search

The refresh token rotates on every refresh, so the tokens live on one side at a
time: `push` removes them from the PC, `pull` from the Kobo.

Docs: https://developers.openai.com/siwc/token-sharing-open-source/sign-in
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import http.server
import json
import os
import secrets
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import webbrowser
from collections import Counter
from pathlib import Path

AUTHORIZE_URL = "https://auth.openai.com/api/accounts/authorize"
TOKEN_URL = "https://auth.openai.com/api/accounts/oauth/token"
API = "https://api.openai.com/v1"
SCOPE = "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"
APP_NAME = "NickelFootnote"
PORT = 1455
REDIRECT_URI = f"http://127.0.0.1:{PORT}/auth/callback"

AUTH_FILE = Path(__file__).resolve().parent.parent / ".secrets" / "auth.json"
TOKEN_KEYS = ("access_token", "refresh_token", "id_token", "expires_at", "scope")
DEVICE_AUTH = Path(".adds") / "nickelfootnote" / "auth.json"


# --- storage -----------------------------------------------------------------

def load_auth() -> dict:
    if AUTH_FILE.exists():
        return json.loads(AUTH_FILE.read_text(encoding="utf-8"))
    return {}


def save_auth(auth: dict) -> None:
    AUTH_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = AUTH_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(auth, indent=2), encoding="utf-8")
    tmp.replace(AUTH_FILE)  # atomic: the refresh token rotates on every use


def jwt_claims(token: str) -> dict:
    try:
        payload = token.split(".")[1]
        payload += "=" * (-len(payload) % 4)
        return json.loads(base64.urlsafe_b64decode(payload))
    except (IndexError, ValueError):
        return {}


# --- OAuth -------------------------------------------------------------------

def post_form(url: str, fields: dict) -> dict:
    data = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(url, data=data, method="POST",
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        sys.exit(f"{url} -> HTTP {e.code}: {e.read().decode(errors='replace')}")


def store_tokens(auth: dict, tokens: dict) -> None:
    auth["access_token"] = tokens["access_token"]
    auth["refresh_token"] = tokens.get("refresh_token", auth.get("refresh_token"))
    if tokens.get("id_token"):
        auth["id_token"] = tokens["id_token"]
    auth["expires_at"] = int(time.time()) + int(tokens.get("expires_in", 3600))
    auth["scope"] = tokens.get("scope", auth.get("scope"))
    save_auth(auth)


def cmd_login(_args) -> None:
    auth = load_auth()
    # Opaque per-host id; accepted forms are urn:uuid:, urn:ietf:params:oauth:jwk-thumbprint: and did:key:.
    auth.setdefault("ext_agent_host_id", f"urn:uuid:{uuid.uuid4()}")
    reauth = bool(auth.get("client_id"))

    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    state = secrets.token_urlsafe(24)
    params = {
        "client_id": auth["client_id"] if reauth else "dynamic_agent_client",
        "ext_agent_host_id": auth["ext_agent_host_id"],
        "response_type": "code",
        "redirect_uri": REDIRECT_URI,
        "scope": SCOPE,
        "resource": API,
        "state": state,
        "nonce": secrets.token_urlsafe(24),
        "code_challenge_method": "S256",
        "code_challenge": challenge,
    }
    if reauth:
        if auth.get("id_token"):
            params["id_token_hint"] = auth["id_token"]
    else:
        params["agent_name_hint"] = APP_NAME

    result: dict = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            if url.path != "/auth/callback":
                self.send_response(404)
                self.end_headers()
                return
            params = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
            print(f"Callback received, parameters: {', '.join(k for k in params if k != 'code') or '(none)'}",
                  flush=True)
            if not params:
                # Parameters may be in the URL fragment, which browsers don't send:
                # bounce them back as a query string.
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(b"""<!doctype html><p id=m>NickelFootnote: finishing sign-in...</p><script>
var h = location.hash.slice(1);
if (h) location.replace('/auth/callback?' + h + '&_fragment=1');
else document.getElementById('m').textContent = 'NickelFootnote: the callback had no parameters.';
</script>""")
                return
            result.update(params)
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            self.wfile.write("NickelFootnote: you can close this tab.".encode())

        def do_POST(self):
            # In case the response is delivered as a form post.
            length = int(self.headers.get("Content-Length", 0))
            params = {k: v[0] for k, v in urllib.parse.parse_qs(self.rfile.read(length).decode()).items()}
            print(f"POST callback received, parameters: {', '.join(k for k in params if k != 'code') or '(none)'}",
                  flush=True)
            result.update(params)
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            self.wfile.write("NickelFootnote: you can close this tab.".encode())

        def log_message(self, *_):
            pass

    server = http.server.HTTPServer(("127.0.0.1", PORT), Handler)
    url = AUTHORIZE_URL + "?" + urllib.parse.urlencode(params)
    print("Opening the ChatGPT sign-in page. If it doesn't open, visit:\n" + url + "\n")
    webbrowser.open(url)
    while not result:
        server.handle_request()
    server.server_close()

    print("Callback parameters:", ", ".join(k for k in result if k != "code"))
    if "error" in result:
        sys.exit(f"Sign-in failed: {result.get('error')}: {result.get('error_description', '')}")
    if result.get("state") != state:
        sys.exit("State mismatch, aborting.")

    client_id = result.get("client_id") or auth.get("client_id")
    if not client_id:
        sys.exit("The callback had no client_id. Parameters received: " + ", ".join(result))
    auth["client_id"] = client_id

    tokens = post_form(TOKEN_URL, {
        "grant_type": "authorization_code",
        "client_id": client_id,
        "code": result["code"],
        "code_verifier": verifier,
        "redirect_uri": REDIRECT_URI,
        "resource": API,
    })
    store_tokens(auth, tokens)
    claims = jwt_claims(auth.get("id_token", ""))
    print(f"Signed in as {claims.get('email', '?')}. Client id: {client_id}")
    print(f"Granted scopes: {tokens.get('scope')}")
    print(f"Saved to {AUTH_FILE}")


def access_token() -> str:
    auth = load_auth()
    if not auth.get("refresh_token"):
        if auth.get("pushed_at"):
            sys.exit("The tokens are on the Kobo (pushed). Run `pull` to get them back, or `login` again.")
        sys.exit("Not signed in. Run: python tools/signin.py login")
    if auth.get("expires_at", 0) - 60 < time.time():
        tokens = post_form(TOKEN_URL, {
            "grant_type": "refresh_token",
            "client_id": auth["client_id"],
            "refresh_token": auth["refresh_token"],
            "resource": API,
        })
        store_tokens(auth, tokens)
        print("(refreshed access token)", file=sys.stderr)
    return auth["access_token"]


# --- API ---------------------------------------------------------------------

def api_request(method: str, path: str, body: dict | None = None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(API + path, data=data, method=method, headers={
        "Authorization": "Bearer " + access_token(),
        "Content-Type": "application/json",
    })
    try:
        return urllib.request.urlopen(req, timeout=180)
    except urllib.error.HTTPError as e:
        sys.exit(f"{method} {path} -> HTTP {e.code}: {e.read().decode(errors='replace')}")


def cmd_models(_args) -> None:
    with api_request("GET", "/models") as resp:
        data = json.loads(resp.read())
    models = data.get("models", data.get("data", []))
    for m in models:
        print(f"{m.get('slug', m.get('id')):30} {m.get('display_name', ''):30} visibility={m.get('visibility')}")


INSTRUCTIONS = """You answer questions from someone who is in the middle of reading a book series on an e-reader.
They want reminders about characters, places, events and lore they have already read about.
Never reveal anything that happens after their current position: no later events, deaths, identities,
twists, or hints that something matters later. Treat their position as the end of everything they know.
Your wording must not hint at the future either. Avoid words that imply a situation will or won't change,
such as "still", "yet", "so far", "for now", "remains", "currently", "at this point", "not until",
and don't frame answers relative to their position ("at your point", "as of the prologue"). State things as
plain facts of the story: "the Wicked Witch of the West rules the Winkies", not "the Wicked Witch still rules the Winkies".
If a fair answer would need information from later in the series, say so instead of answering.
Use web search to check facts against reliable sources (wikis, chapter summaries), but only use what is
established by their current point. Be concise: the answer is read on a small e-ink screen.
A quoted passage, if present, is text the reader selected on the page they are reading.
Format: short paragraphs, **bold** for emphasis if useful, simple bullet lists. No tables, no links,
no headings longer than a few words.
Wrap the names of characters, places, groups/factions and lore terms in colour tags: [[char:Dorothy]],
[[place:Emerald City]], [[group:Munchkins]], [[term:Silver Shoes]]. Only these four categories. Tag only the
important mentions, for example the first mention of each name in an answer. Never put a tag inside
another tag or inside **bold** markers."""


def build_context(a) -> str:
    parts = []
    if a.series:
        parts.append(f"Series: {a.series}" + (f", book {a.number}" if a.number else ""))
    if a.book:
        parts.append(f"Book: {a.book}")
    if a.chapter:
        parts.append(f"Current chapter: {a.chapter}")
    if a.percent is not None:
        parts.append(f"Progress in this book: {a.percent}%")
    return "Reading position:\n" + "\n".join(parts) if parts else ""


def cmd_ask(a) -> None:
    auth = load_auth()
    model = a.model or auth.get("model")
    if not model:
        sys.exit("Pass --model <slug> (see the `models` command). It is remembered afterwards.")
    if a.model:
        auth["model"] = a.model
        save_auth(auth)

    context = build_context(a)
    body = {
        "model": model,
        "instructions": INSTRUCTIONS,
        "input": [{"role": "user", "content": (context + "\n\n" if context else "") + "Question: " + a.question}],
        "store": False,
        "stream": True,
    }
    if not a.no_search:
        body["tools"] = [{"type": "web_search"}]

    events: Counter = Counter()
    searches: list[str] = []
    citations: list[str] = []
    start = time.time()
    first_text = None
    outcome = "stream ended without response.completed"

    with api_request("POST", "/responses", body) as resp:
        for raw in resp:
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if not line.startswith("data:"):
                continue
            payload = line[5:].strip()
            if payload == "[DONE]":
                break
            try:
                ev = json.loads(payload)
            except ValueError:
                continue
            kind = ev.get("type", "?")
            events[kind] += 1
            if kind == "response.output_text.delta":
                if first_text is None:
                    first_text = time.time() - start
                    print()
                print(ev.get("delta", ""), end="", flush=True)
            elif kind == "response.output_item.done":
                item = ev.get("item", {})
                if item.get("type") == "web_search_call":
                    action = item.get("action", {})
                    searches.append(action.get("query") or json.dumps(action))
                    print(f"[web search: {searches[-1]}]", flush=True)
            elif kind == "response.output_text.annotation.added":
                ann = ev.get("annotation", {})
                if ann.get("url"):
                    citations.append(ann["url"])
            elif kind == "response.completed":
                outcome = "completed (server model: %s)" % ev.get("response", {}).get("model")
                for item in ev.get("response", {}).get("output", []):
                    for part in item.get("content", []) or []:
                        for ann in part.get("annotations", []) or []:
                            if ann.get("url") and ann["url"] not in citations:
                                citations.append(ann["url"])
            elif kind in ("response.failed", "error", "response.incomplete"):
                outcome = kind + ": " + json.dumps(ev.get("response", {}).get("error") or ev.get("error") or ev)

    total = time.time() - start
    print("\n\n--- diagnostics ---")
    print(f"model: {model}   web search requested: {not a.no_search}   outcome: {outcome}")
    print(f"time to first text: {first_text:.1f}s" if first_text else "no text received", f"  total: {total:.1f}s")
    print(f"web searches performed: {len(searches)}")
    for u in dict.fromkeys(citations):
        print("  cited:", u)
    print("event types:", dict(events))


# --- moving the tokens to and from the Kobo ------------------------------------

def find_kobos() -> list[Path]:
    """Mounted volumes that look like a Kobo (a .kobo folder at the root)."""
    if os.name == "nt":
        candidates = [Path(f"{letter}:/") for letter in "DEFGHIJKLMNOPQRSTUVWXYZ"]
    else:
        user = os.environ.get("USER", "")
        candidates = []
        for base in ("/Volumes", f"/media/{user}", f"/run/media/{user}", "/media", "/mnt"):
            try:
                candidates += [p for p in Path(base).iterdir() if p.is_dir()]
            except OSError:
                pass
    found: list[Path] = []
    for root in candidates:
        try:
            if (root / ".kobo").is_dir() and root.resolve() not in [f.resolve() for f in found]:
                found.append(root)
        except OSError:
            pass
    return found


def device_file(drive: str | None) -> Path:
    if drive:
        root = Path(drive.rstrip("\\/") + "/")
        if not (root / ".kobo").is_dir():
            sys.exit(f"No Kobo at {root} (no .kobo folder). Check the drive or mount point.")
    else:
        found = find_kobos()
        if not found:
            sys.exit("No Kobo found. Connect it over USB (tap Connect on the Kobo) or pass --drive.")
        if len(found) > 1:
            sys.exit("More than one Kobo found (" + ", ".join(map(str, found)) + "); pick one with --drive.")
        root = found[0]
        print(f"Kobo found at {root}")
    return root / DEVICE_AUTH


def write_synced(path: Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    tmp.replace(path)


def cmd_push(a) -> None:
    target = device_file(a.drive)
    access_token()  # refreshes if needed: checks the refresh token still works before handing it over
    auth = load_auth()
    if target.exists():
        print(f"Replacing the tokens already on the Kobo ({target}).")
    data = json.dumps({k: v for k, v in auth.items() if k != "pushed_at"}, indent=2)
    write_synced(target, data)
    if json.loads(target.read_text(encoding="utf-8")).get("refresh_token") != auth["refresh_token"]:
        sys.exit(f"Verification of {target} failed; the PC copy was kept.")
    # Stop using them here: a refresh on either side spends the other's refresh token.
    for k in TOKEN_KEYS:
        auth.pop(k, None)
    auth["pushed_at"] = int(time.time())
    save_auth(auth)
    print(f"Pushed to {target}. The PC no longer has the tokens; eject the Kobo safely.")


def cmd_pull(a) -> None:
    source = device_file(a.drive)
    if not source.exists():
        sys.exit(f"No tokens on the Kobo ({source}).")
    device = json.loads(source.read_text(encoding="utf-8"))
    if not device.get("refresh_token"):
        sys.exit(f"{source} has no refresh token.")
    auth = load_auth()
    auth.update(device)
    auth.pop("pushed_at", None)
    save_auth(auth)
    os.remove(source)
    print(f"Pulled the tokens from {source} to {AUTH_FILE}; the Kobo no longer has them.")


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("login").set_defaults(func=cmd_login)
    sub.add_parser("models").set_defaults(func=cmd_models)
    ask = sub.add_parser("ask")
    ask.add_argument("question")
    ask.add_argument("--model")
    ask.add_argument("--series")
    ask.add_argument("--number")
    ask.add_argument("--book")
    ask.add_argument("--chapter")
    ask.add_argument("--percent", type=int)
    ask.add_argument("--no-search", action="store_true")
    ask.set_defaults(func=cmd_ask)
    for name, func in (("push", cmd_push), ("pull", cmd_pull)):
        sp = sub.add_parser(name)
        sp.add_argument("--drive", help="the Kobo's drive or mount point (default: found automatically)")
        sp.set_defaults(func=func)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
