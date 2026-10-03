#!/usr/bin/env bash
# MarkPeek Linux smoke test: renders sample.md offscreen and verifies the
# WYSIWYG pipeline inside the page (md4c HTML, CSS, editor JS, DOM->Markdown
# export, dirty tracking). No window server required (Xvfb).
#
# v1.3.1+: also checks that the page reports no error, and exercises the
# encoding matrix (UTF-8, UTF-8 BOM, Windows-1251, UTF-16LE) with Cyrillic.
#
# Usage: ci/smoke_test.sh [path-to-markpeek-binary] [sample.md]
set -u

BIN="${1:-dist/markpeek}"
SAMPLE="${2:-sample.md}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/check.js" <<'EOF'
// __mpExport / __mpSetMode / __mpDirty come from the embedded editor JS.
var c = document.getElementById("markpeek-content");
var res = {
  hasContent: !!c && c.children.length > 0,
  hasH1: !!c.querySelector("h1"),
  hasTable: !!c.querySelector("table"),
  hasCode: !!c.querySelector("pre code"),
  hasTaskList: !!c.querySelector(".task-list-item input[type=checkbox]"),
  paragraphs: c.querySelectorAll("p").length
};
try {
  __mpSetMode(true);
  res.editMode = document.body.className === "mp-editing" &&
                 c.getAttribute("contenteditable") === "true";
  __mpClearDirty();
  var md = __mpExport();
  res.exportOk = md.indexOf("# ") === 0;
  res.exportHasTable = md.indexOf("|") !== -1 && md.indexOf("---") !== -1;
  res.exportHasFence = md.indexOf("```") !== -1;
  res.exportHasTask = md.indexOf("- [ ]") !== -1 || md.indexOf("- [x]") !== -1;
  res.exportLen = md.length;
  __mpSetMode(false);
  res.previewMode = document.body.className !== "mp-editing";
} catch (e) {
  res.error = String(e && e.message ? e.message : e);
}
JSON.stringify(res);
EOF

# Check JS for encoding-variant runs: text must survive the round trip.
cat > "$TMP/check-enc.js" <<'EOF'
var c = document.getElementById("markpeek-content");
var t = c.textContent || "";
var res = {
  hasContent: !!c && c.children.length > 0,
  hasCyrillic: /[А-Яа-яЁё]/.test(t),
  textLen: t.length
};
JSON.stringify(res);
EOF

echo "== MarkPeek smoke test =="
echo "binary: $BIN"
ls -l "$BIN"

if [ ! -f "$SAMPLE" ]; then
  echo "FAIL: sample not found: $SAMPLE" >&2
  exit 2
fi

# WebKit2GTK in containers/WSL: disable bubblewrap sandbox and DMABUF renderer
# (no user namespaces / no GPU); force software rendering via Xvfb.
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export WEBKIT_DISABLE_DMABUF_RENDERER=1
export LIBGL_ALWAYS_SOFTWARE=1
export GDK_BACKEND=x11

SMK_RC=0
SMK_OUT=""
run_smoke() {
  local checkjs="$1" samplefile="$2" outfile="$3"
  export MP_SMOKE_JS="$checkjs"
  export MP_SMOKE_OUT="$outfile"
  export MP_SMOKE_FILE="$(cd "$(dirname "$samplefile")" && pwd)/$(basename "$samplefile")"
  xvfb-run -a -s "-screen 0 1280x800x24" "$BIN" "$MP_SMOKE_FILE" >"$TMP/app.log" 2>&1
  SMK_RC=$?
  SMK_OUT=""
  if [ -f "$outfile" ]; then SMK_OUT="$(cat "$outfile")"; fi
  unset MP_SMOKE_JS MP_SMOKE_OUT MP_SMOKE_FILE
}

# --- main sample run ---
run_smoke "$TMP/check.js" "$SAMPLE" "$TMP/result.json"
RC=$SMK_RC
SMOKE_OUT_CONTENT="$SMK_OUT"

echo "--- app log (first 20 lines) ---"
head -20 "$TMP/app.log"

if [ $RC -ne 0 ]; then
  echo "FAIL: app exited with rc=$RC" >&2
  exit $RC
fi

if [ -z "$SMOKE_OUT_CONTENT" ]; then
  echo "FAIL: no smoke result produced (is MP_SMOKE_JS supported?)" >&2
  exit 3
fi

MP_SMOKE_OUT="$TMP/result.json"

echo "--- result ---"
printf '%s\n' "$SMOKE_OUT_CONTENT"
echo

PASS=1
for kv in hasContent:true hasH1:true hasTable:true hasCode:true \
          hasTaskList:true editMode:true exportOk:true exportHasTable:true \
          exportHasFence:true exportHasTask:true previewMode:true; do
  k="${kv%%:*}"; v="${kv##*:}"
  if ! grep -q "\"$k\":$v" "$MP_SMOKE_OUT"; then
    echo "CHECK FAILED: $k expected $v"
    PASS=0
  fi
done
grep -q '"error"' "$MP_SMOKE_OUT" && { echo "CHECK FAILED: page JS error"; PASS=0; }

# --- encoding matrix (UTF-8 / UTF-8 BOM / CP1251 / UTF-16LE with Cyrillic) ---
if command -v python3 >/dev/null 2>&1; then
  python3 - "$TMP" <<'PYEOF'
import sys, pathlib
tmp = pathlib.Path(sys.argv[1])
text = "# Заголовок\n\nКириллица: привет, мир. Ёжик и ёлка.\n"
(tmp / "enc_utf8.md").write_bytes(text.encode("utf-8"))
(tmp / "enc_bom.md").write_bytes(b"\xef\xbb\xbf" + text.encode("utf-8"))
(tmp / "enc_cp1251.md").write_bytes(text.encode("cp1251"))
(tmp / "enc_u16le.md").write_bytes(b"\xff\xfe" + text.encode("utf-16-le"))
PYEOF
  for f in enc_utf8 enc_bom enc_cp1251 enc_u16le; do
    run_smoke "$TMP/check-enc.js" "$TMP/$f.md" "$TMP/$f.json"
    echo "--- encoding $f: rc=$SMK_RC $SMK_OUT ---"
    if [ "$SMK_RC" -ne 0 ]; then echo "CHECK FAILED: $f app rc=$SMK_RC"; PASS=0; continue; fi
    if [ -z "$SMK_OUT" ]; then echo "CHECK FAILED: $f no result"; PASS=0; continue; fi
    grep -q '"hasContent":true' <<<"$SMK_OUT" || { echo "CHECK FAILED: $f hasContent"; PASS=0; }
    grep -q '"hasCyrillic":true' <<<"$SMK_OUT" || { echo "CHECK FAILED: $f hasCyrillic (decoding broken?)"; PASS=0; }
  done
else
  echo "note: python3 not available, skipping encoding matrix"
fi

if [ "$PASS" -eq 1 ]; then
  echo "SMOKE TEST: PASS"
  exit 0
fi
echo "SMOKE TEST: FAIL"
exit 1
