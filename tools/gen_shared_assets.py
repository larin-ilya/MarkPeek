#!/usr/bin/env python3
"""Generate src/shared/{shared_css.h,editor_js.h} from the Windows sources.

The WYSIWYG editor JS and the Typora-like CSS live in src/main.cpp (the
Windows build embeds them via raw string literals). The Linux build reuses
the exact same page logic; this script extracts both blocks so the two
platforms cannot drift apart.

Run from the repo root:  python3 tools/gen_shared_assets.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "src" / "main.cpp"
OUT = ROOT / "src" / "shared"

CSS_RE = re.compile(r'static const char\* kCss = R"CSS\((.*?)\)CSS";', re.S)
JS_RE = re.compile(r'static const char\* kEditorJs = R"JS\((.*?)\)JS";', re.S)

LINUX_CSS_EXTRA = """
/* ---- Linux fonts: use platform-native stacks when Segoe UI is absent ---- */
html.platform-linux body { font-family: "Segoe UI", "Cantarell", "DejaVu Sans", "Noto Sans", "Helvetica Neue", Arial, sans-serif; }
html.platform-linux code, html.platform-linux pre, html.platform-linux pre code { font-family: "Consolas", "JetBrains Mono", "DejaVu Sans Mono", "Liberation Mono", "Courier New", monospace; }
"""


def extract(pattern: re.Pattern, what: str) -> str:
    main = MAIN.read_text(encoding="utf-8")
    m = pattern.search(main)
    if not m:
        sys.exit(f"error: {what} block not found in {MAIN}")
    return m.group(1)


def main() -> None:
    css = extract(CSS_RE, "kCss")
    js = extract(JS_RE, "kEditorJs")

    # Linux font stacks are appended to the shared CSS (kept out of main.cpp
    # so the IE9-compatible Windows sheet stays untouched).
    css = css.replace(
        "/* ---- WYSIWYG editing ---- */",
        LINUX_CSS_EXTRA.rstrip() + "\n/* ---- WYSIWYG editing ---- */",
    )

    OUT.mkdir(parents=True, exist_ok=True)

    (OUT / "shared_css.h").write_text(
        "// AUTO-GENERATED from src/main.cpp by tools/gen_shared_assets.py — do not edit.\n"
        "// Shared Typora-like theme CSS (used by both the Windows and the Linux build).\n"
        "// Keep in sync with the kCss string in src/main.cpp.\n"
        '#pragma once\n\nstatic const char* kCss = R"CSS(' + css + ')CSS";\n',
        encoding="utf-8",
    )
    (OUT / "editor_js.h").write_text(
        "// AUTO-GENERATED from src/main.cpp by tools/gen_shared_assets.py — do not edit.\n"
        "// Shared WYSIWYG editor JS (runs inside the rendered page in both builds).\n"
        "// Keep in sync with the kEditorJs string in src/main.cpp.\n"
        '#pragma once\n\nstatic const char* kEditorJs = R"JS(' + js + ')JS";\n',
        encoding="utf-8",
    )
    print(f"wrote {OUT/'shared_css.h'} ({len(css)} chars of CSS)")
    print(f"wrote {OUT/'editor_js.h'} ({len(js)} chars of JS)")


if __name__ == "__main__":
    main()
