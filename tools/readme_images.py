#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the README images from device screenshots.

Puts each screenshot in docs/images/screens/ into a drawn e-reader frame and
renders it with headless Chrome (or Edge):

  python tools/readme_images.py

Writes docs/images/hero.png (two devices) and docs/images/frame-<name>.png
(one device each). Screenshots are 1072x1448 (Kobo Clara Colour).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IMAGES = ROOT / "docs" / "images"
SCREENS = IMAGES / "screens"

HERO = ("reading-menu", "spoiler-guard")
SINGLES = ("selection", "ask", "book-info")

STYLE = """
html, body { margin: 0; background: transparent; }
.stage { display: flex; align-items: center; justify-content: center; }
.device {
  background: #2a2927; border-radius: 44px; padding: 34px 34px 58px;
  box-shadow: 0 18px 40px rgba(0,0,0,.28), inset 0 0 0 2px #3a3936;
}
.device img { display: block; width: 536px; height: 724px; border-radius: 4px; }
"""


def find_chrome() -> str:
    candidates = [
        shutil.which("chrome"), shutil.which("google-chrome"), shutil.which("chromium"), shutil.which("msedge"),
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    ]
    for c in candidates:
        if c and os.path.exists(c):
            return c
    sys.exit("Chrome, Chromium or Edge is needed to render the images.")


def device(name: str, extra: str = "") -> str:
    return f'<div class="device" style="{extra}"><img src="{(SCREENS / (name + ".png")).as_uri()}"></div>'


def render(chrome: str, html: str, out: Path, width: int, height: int) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        page = Path(tmp) / "page.html"
        page.write_text(f"<!doctype html><style>{STYLE}</style>{html}", encoding="utf-8")
        subprocess.run([chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                        "--allow-file-access-from-files", "--default-background-color=00000000",
                        f"--window-size={width},{height}", f"--screenshot={out}", page.as_uri()],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("wrote", out.relative_to(ROOT))


def main() -> None:
    chrome = find_chrome()
    # Device: 536 + 2*34 = 604 wide, 724 + 34 + 58 = 816 tall, plus room for the shadow.
    w, h = 1400, 920
    left, right = HERO
    render(chrome, f'<div class="stage" style="width:{w}px;height:{h}px;gap:64px">'
                   f'{device(left)}{device(right)}</div>',
           IMAGES / "hero.png", w, h)
    for name in SINGLES:
        render(chrome, f'<div class="stage" style="width:680px;height:880px">{device(name)}</div>',
               IMAGES / f"frame-{name}.png", 680, 880)


if __name__ == "__main__":
    main()
