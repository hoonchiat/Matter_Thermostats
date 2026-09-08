#!/usr/bin/env python3
"""
gen_userguide.py — build the OLED user guide from the real firmware screens.

Reads the base64 screen framebuffers emitted by firmware/test/host/render_screens
(JSON on stdin), injects them into docs/USER_GUIDE.template.html, and writes the
self-contained docs/USER_GUIDE.html.

Usage (from repo root):
    firmware/test/host/render_screens | python3 tools/gen_userguide.py
"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TEMPLATE = ROOT / "docs" / "USER_GUIDE.template.html"
OUTPUT = ROOT / "docs" / "USER_GUIDE.html"
TOKEN = "__SCREEN_DATA__"


def main() -> int:
    data = json.load(sys.stdin)               # validates the JSON from render_screens
    if not isinstance(data, dict) or not data:
        sys.exit("error: expected a non-empty JSON object of {id: base64}")

    tpl = TEMPLATE.read_text()
    if TOKEN not in tpl:
        sys.exit(f"error: {TOKEN} not found in {TEMPLATE}")

    html = tpl.replace(TOKEN, json.dumps(data, separators=(",", ":")))
    OUTPUT.write_text(html)
    print(f"wrote {OUTPUT.relative_to(ROOT)} ({len(data)} screens, {len(html)} bytes)",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
