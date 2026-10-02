// AUTO-GENERATED from src/main.cpp by tools/gen_shared_assets.py — do not edit.
// Shared Typora-like theme CSS (used by both the Windows and the Linux build).
// Keep in sync with the kCss string in src/main.cpp.
#pragma once

static const char* kCss = R"CSS(
* { box-sizing: border-box; }
html, body { margin: 0; padding: 0; background: #ffffff; }
body { font-family: "Segoe UI", "Helvetica Neue", "Microsoft YaHei", Arial, sans-serif; font-size: 16px; line-height: 1.65; color: #333333; }
#markpeek-content { max-width: 860px; margin: 0 auto; padding: 28px 48px 96px; }
h1, h2, h3, h4, h5, h6 { font-weight: 600; color: #111111; line-height: 1.3; margin: 1.5em 0 0.6em; }
h1 { font-size: 1.9em; padding-bottom: 0.3em; border-bottom: 1px solid #eaecef; }
h2 { font-size: 1.5em; padding-bottom: 0.3em; border-bottom: 1px solid #eaecef; }
h3 { font-size: 1.25em; }
h4 { font-size: 1.05em; }
h5 { font-size: 0.95em; }
h6 { font-size: 0.9em; color: #666666; }
p { margin: 0.8em 0; }
a { color: #0366d6; text-decoration: none; }
a:hover { text-decoration: underline; }
code { font-family: Consolas, "Courier New", monospace; background: #f6f8fa; color: #d73a49; font-size: 0.9em; padding: 0.15em 0.4em; border-radius: 3px; }
pre { background: #f6f8fa; border: 1px solid #e1e4e8; border-radius: 6px; padding: 14px 16px; overflow: auto; line-height: 1.45; }
pre code { background: transparent; color: #24292e; padding: 0; font-size: 0.92em; }
blockquote { margin: 0.8em 0; padding: 0.3em 1.1em; border-left: 4px solid #dfe2e5; color: #6a737d; background: #f8f8f8; }
blockquote p { margin: 0.4em 0; }
ul, ol { padding-left: 2em; margin: 0.8em 0; }
li { margin: 0.25em 0; }
li > ul, li > ol { margin: 0; }
hr { border: none; border-top: 2px solid #eaecef; margin: 1.6em 0; }
img { max-width: 100%; border-radius: 4px; }
table { border-collapse: collapse; margin: 1em 0; }
th, td { border: 1px solid #dfe2e5; padding: 6px 13px; }
th { background: #f6f8fa; font-weight: 600; }
tr:nth-child(2n) { background: #f6f8fa; }
input[type="checkbox"] { margin-right: 0.4em; }
del { color: #6a737d; }
sup { font-size: 0.75em; }

/* ---- Linux fonts: use platform-native stacks when Segoe UI is absent ---- */
html.platform-linux body { font-family: "Segoe UI", "Cantarell", "DejaVu Sans", "Noto Sans", "Helvetica Neue", Arial, sans-serif; }
html.platform-linux code, html.platform-linux pre, html.platform-linux pre code { font-family: "Consolas", "JetBrains Mono", "DejaVu Sans Mono", "Liberation Mono", "Courier New", monospace; }
/* ---- WYSIWYG editing ---- */
body.mp-editing #markpeek-content { cursor: text; }
body.mp-editing a, body.mp-editing img { pointer-events: none; }
#markpeek-content[contenteditable="true"] { outline: none; }
)CSS";
