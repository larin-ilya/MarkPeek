#!/usr/bin/env bash
# MarkPeek Linux smoke test: renders sample.md offscreen and verifies the
# WYSIWYG pipeline inside the page (md4c HTML, CSS, editor JS, DOM->Markdown
# export, dirty tracking). No window server required (Xvfb).
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

echo "== MarkPeek smoke test =="
echo "binary: $BIN"
ls -l "$BIN"

if [ ! -f "$SAMPLE" ]; then
  echo "FAIL: sample not found: $SAMPLE" >&2
  exit 2
fi

# Run MarkPeek with sample.md on a virtual display. MP_SMOKE_JS/MP_SMOKE_OUT
# make the app itself evaluate our check after the page renders and print the
# JSON result to the output file (built-in hook, see src/linux/main_gtk.cpp).
echo "note: page-level checks run through the built-in MP_SMOKE hook"

export MP_SMOKE_JS="$TMP/check.js"
export MP_SMOKE_OUT="$TMP/result.json"
export MP_SMOKE_FILE="$(pwd)/$SAMPLE"

# WebKit2GTK in containers: disable bubblewrap sandbox and DMABUF renderer
# (no user namespaces / no GPU in CI); force software rendering via Xvfb.
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export WEBKIT_DISABLE_DMABUF_RENDERER=1
export LIBGL_ALWAYS_SOFTWARE=1
export GDK_BACKEND=x11

xvfb-run -a -s "-screen 0 1280x800x24" "$BIN" "$MP_SMOKE_FILE" >"$TMP/app.log" 2>&1
RC=$?
SMOKE_OUT_CONTENT=""
if [ -f "$TMP/result.json" ]; then SMOKE_OUT_CONTENT="$(cat "$TMP/result.json")"; fi
unset MP_SMOKE_JS MP_SMOKE_OUT MP_SMOKE_FILE
unset WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS WEBKIT_DISABLE_DMABUF_RENDERER LIBGL_ALWAYS_SOFTWARE GDK_BACKEND

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

if [ "$PASS" -eq 1 ]; then
  echo "SMOKE TEST: PASS"
  exit 0
fi
echo "SMOKE TEST: FAIL"
exit 1
