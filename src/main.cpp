// MarkPeek — a minimal Typora-style Markdown viewer/editor for Windows.
// Win32 + embedded IE (MSHTML) + md4c renderer. MIT License.
// Works on Windows 7 SP1 and Windows 10. Portable, 32-bit, no dependencies.
//
// Editing is WYSIWYG: the same rendered document becomes contenteditable,
// so you edit exactly what you see (Typora style). Saving serializes the DOM
// back to Markdown inside the page. Editing hotkeys (Ctrl+A/C/V/X/Z ...) are
// handled natively by the browser engine, so they work in any keyboard layout.

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
// Emit GUID definitions (IID_IHTMLDocument2 etc.) into this translation unit
// for MinGW, which does not export the MSHTML interface IIDs from a library.
#define INITGUID

#include <windows.h>
#include <ole2.h>
#include <exdisp.h>
#include <oleidl.h>
#include <ocidl.h>
#include <oleauto.h>
#include <mshtml.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>

#include "md4c/md4c.h"
#include "md4c/md4c-html.h"
#include "resource.h"

#define APP_NAME L"MarkPeek"
#define APP_VERSION L"1.2.0"

static HWND g_hwnd = NULL;
static HWND g_hwndBrowser = NULL;
static HWND g_hwndStatus = NULL;
static IWebBrowser2* g_wb = NULL;
static IOleInPlaceActiveObject* g_pioipa = NULL;
static std::wstring g_currentPath;
static std::wstring g_tempHtmlPath;
static bool g_editing = false;
static bool g_dirty = false;
static UINT_PTR g_dirtyTimer = 0;

enum { IDM_OPEN = 1001, IDM_RELOAD, IDM_ASSOC, IDM_UNASSOC, IDM_EXIT, IDM_ABOUT,
       IDM_EDIT, IDM_SAVE };
enum { IDC_STATUS = 2001 };

#define DIRTY_TIMER_ID 1
#define DIRTY_POLL_MS  250

// ---------------------------------------------------------------------------
// String / file helpers

static std::string WideToUtf8(const wchar_t* s, int len = -1) {
    if (len < 0) len = (int)wcslen(s);
    if (len <= 0) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, NULL, 0, NULL, NULL);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, &out[0], n, NULL, NULL);
    return out;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

static bool FileExists(const std::wstring& p) {
    DWORD attr = GetFileAttributesW(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring DirOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? L"" : path.substr(0, p + 1);
}

static std::wstring BaseName(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

// Reads a file as UTF-8 (handles UTF-8 / UTF-16LE / UTF-16BE BOMs).
static std::string ReadFileUtf8(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return std::string();
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 2) {
        unsigned char b0 = (unsigned char)bytes[0], b1 = (unsigned char)bytes[1];
        if (b0 == 0xFF && b1 == 0xFE) {  // UTF-16 LE
            size_t n = (bytes.size() - 2) / 2;
            return WideToUtf8((const wchar_t*)(bytes.data() + 2), (int)n);
        }
        if (b0 == 0xFE && b1 == 0xFF) {  // UTF-16 BE
            size_t n = (bytes.size() - 2) / 2;
            std::vector<wchar_t> w(n);
            for (size_t i = 0; i < n; i++) {
                unsigned char hi = (unsigned char)bytes[2 + i * 2];
                unsigned char lo = (unsigned char)bytes[2 + i * 2 + 1];
                w[i] = (wchar_t)((lo << 8) | hi);
            }
            return WideToUtf8(w.data(), (int)n);
        }
    }
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);
    return bytes;  // UTF-8
}

// ---------------------------------------------------------------------------
// Markdown -> HTML (md4c)

static void MdRenderCb(const MD_CHAR* text, MD_SIZE size, void* userdata) {
    std::string* out = (std::string*)userdata;
    out->append(text, size);
}

static bool MdToHtml(const std::string& md, std::string& outBody) {
    std::string body;
    int rc = md_html(md.data(), (MD_SIZE)md.size(), MdRenderCb, &body,
                     MD_DIALECT_GITHUB, MD_HTML_FLAG_SKIP_UTF8_BOM);
    if (rc != 0) return false;
    outBody = body;
    return true;
}

// Rewrites relative image src="..." to absolute file:/// URLs so that images
// next to the .md file are shown even though the HTML lives in %TEMP%.
static void AbsolutizeImageSrcs(std::string& html, const std::wstring& dirW) {
    if (dirW.empty()) return;
    std::string base = WideToUtf8((L"file:///" + dirW).c_str());
    for (char& c : base) if (c == '\\') c = '/';

    std::string needle = "src=\"";
    size_t pos = 0;
    while ((pos = html.find(needle, pos)) != std::string::npos) {
        size_t start = pos + needle.size();
        size_t end = html.find('"', start);
        if (end == std::string::npos) break;
        std::string src = html.substr(start, end - start);
        bool absolute =
            src.rfind("http://", 0) == 0 || src.rfind("https://", 0) == 0 ||
            src.rfind("data:", 0) == 0 || src.rfind("file:", 0) == 0 ||
            src.rfind("#", 0) == 0 || src.rfind("//", 0) == 0 || src.rfind("/", 0) == 0;
        if (!absolute) {
            std::string abs = base;
            for (char c : src) abs += (c == ' ') ? "%20" : std::string(1, c);
            html.replace(start, end - start, abs);
            pos = start + abs.size();
        } else {
            pos = end + 1;
        }
    }
}

static std::string EscapeHtml(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Typora-like default theme CSS (IE9-compatible).

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
/* ---- WYSIWYG editing ---- */
body.mp-editing #markpeek-content { cursor: text; }
body.mp-editing a, body.mp-editing img { pointer-events: none; }
#markpeek-content[contenteditable="true"] { outline: none; }
)CSS";

// ---------------------------------------------------------------------------
// In-page JS: WYSIWYG helpers + HTML->Markdown serializer (runs in Trident).
// IE11-compatible only (no arrow functions / let / includes).

static const char* kEditorJs = R"JS(
var __mpDirty = false;
var __mpResult = "";
var __mpBase = "";   // "file:///.../<dir>/" of the current file (forward slashes), "" for welcome

function __mpEscapeInline(text, inCell) {
    var esc = "";
    for (var i = 0; i < text.length; i++) {
        var ch = text.charAt(i);
        if (ch === "\\" || ch === "`" || ch === "<" || ch === ">") esc += "\\" + ch;
        else if (inCell && ch === "|") esc += "\\|";
        else esc += ch;
    }
    return esc;
}

// Escape only characters that would start a block construct at line start.
function __mpSafeLineStart(s) {
    if (/^#{1,6}(?=\s|$)/.test(s) || /^>(?=\s|$)/.test(s) ||
        /^[-+*](?=\s|$)/.test(s) || /^\d+[.)](?=\s|$)/.test(s)) return "\\" + s;
    return s;
}

// A plain text node that stands alone as a block (escaped + line-start-safe).
function __mpPlainTextBlock(v) {
    var s = v.replace(/\s+/g, " ").replace(/^ | $/g, "");
    if (s === "") return "";
    s = __mpEscapeInline(s, false);
    return __mpSafeLineStart(s);
}

function __mpCodeSpan(text) {
    var s = text;
    if (s.indexOf("`") < 0) return "`" + s + "`";
    if (s.indexOf("``") < 0) return "`` " + s + " ``";
    if (s.indexOf("```") < 0) return "``` " + s + " ```";
    return "<code>" + s + "</code>";
}

function __mpSupText(el) {
    var a = el.getElementsByTagName("a");
    if (a && a.length > 0) {
        var href = a[0].getAttribute("href") || "";
        var m = /^#fn-(\d+)$/.exec(href);
        if (m) return "[^" + m[1] + "]";
    }
    return "[" + el.textContent + "]";
}

function __mpImgText(el) {
    var src = el.getAttribute("src") || "";
    var alt = el.getAttribute("alt") || "";
    var title = el.getAttribute("title") || "";
    if (__mpBase !== "" && src.indexOf("file:///") === 0) {
        var dec = src.split("%20").join(" ");
        if (dec.indexOf(__mpBase) === 0) src = dec.substring(__mpBase.length);
    }
    src = src.split("%20").join(" ").split("(").join("%28").split(")").join("%29");
    var out = "![" + alt + "](" + src;
    if (title !== "") out += " \"" + title.split("\"").join("\\\"") + "\"";
    out += ")";
    return out;
}

// Serialize a single inline node (a text node or an inline element treated as
// a child) to Markdown, applying the node's own emphasis/tag formatting.
function __mpInlineNode(n, inCell, afterBr) {
    if (n.nodeType === 3) {
        var v = n.nodeValue;
        if (afterBr) v = v.replace(/^[ \t]*\r?\n[ \t]*/, "");
        return __mpEscapeInline(v, inCell);
    }
    if (n.nodeType !== 1) return "";
    var tag = (n.tagName || "").toLowerCase();
    if (tag === "br") return "  \n";
    if (tag === "strong" || tag === "b") return "**" + __mpInline(n, inCell) + "**";
    if (tag === "em" || tag === "i") return "*" + __mpInline(n, inCell) + "*";
    if (tag === "del" || tag === "s" || tag === "strike") return "~~" + __mpInline(n, inCell) + "~~";
    if (tag === "code") return __mpCodeSpan(n.textContent || "");
    if (tag === "sup") return __mpSupText(n);
    if (tag === "img") return __mpImgText(n);
    if (tag === "a") {
        var cls = n.className ? (" " + n.className + " ") : "";
        if (cls.indexOf(" footnote-backref ") >= 0) return "";   // skip
        var href = n.getAttribute("href") || "";
        var title = n.getAttribute("title") || "";
        var txt = __mpInline(n, inCell);
        if (txt === "") return "";
        var mid = "(" + href;
        if (title !== "") mid += " \"" + title.split("\"").join("\\\"") + "\"";
        mid += ")";
        return "[" + txt + "]" + mid;
    }
    // span / font / u / unknown: unwrap, keep inner formatting
    return __mpInline(n, inCell);
}

function __mpInline(el, inCell) {
    var out = "";
    var nodes = el.childNodes;
    for (var i = 0; i < nodes.length; i++) {
        var prevBr = i > 0 && nodes[i - 1].nodeType === 1 &&
                     (nodes[i - 1].tagName || "").toLowerCase() === "br";
        out += __mpInlineNode(nodes[i], inCell, prevBr);
    }
    return out;
}

function __mpParaText(n) {
    var s = __mpInline(n, false);
    var lines = s.split("\n");
    for (var i = 0; i < lines.length; i++) lines[i] = __mpSafeLineStart(lines[i]);
    s = lines.join("\n");
    s = s.replace(/\s+$/g, "");
    return s;
}

function __mpIndent(depth) {
    var s = "";
    for (var i = 0; i < depth; i++) s += "  ";
    return s;
}

function __mpCodeBlock(n, depth) {
    var code = n;
    var cs = n.getElementsByTagName("code");
    if (cs && cs.length > 0) code = cs[0];
    var lang = "";
    var cls = code.getAttribute ? (code.getAttribute("class") || "") : "";
    cls = cls.replace(/^\s+|\s+$/g, "");
    if (cls.indexOf("language-") === 0) lang = cls.substring(9); else lang = cls;
    var txt = code.textContent || "";
    txt = txt.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
    if (txt.charAt(0) === "\n") txt = txt.substring(1);
    if (txt.charAt(txt.length - 1) === "\n") txt = txt.substring(0, txt.length - 1);
    if (txt === "") return "```" + lang + "\n```";
    if (txt.indexOf("```") < 0) return "```" + lang + "\n" + txt + "\n```";
    var lines = txt.split("\n");
    for (var i = 0; i < lines.length; i++) lines[i] = "    " + lines[i];
    return lines.join("\n");
}

function __mpQuote(n, depth) {
    var inner = [];
    var kids = n.childNodes;
    for (var i = 0; i < kids.length; i++) {
        var k = kids[i];
        if (k.nodeType === 3) {
            var s = __mpPlainTextBlock(k.nodeValue);
            if (s) inner.push(s);
            continue;
        }
        if (k.nodeType !== 1) continue;
        var b = __mpBlock(k, depth, true);
        if (b !== null && b !== "") inner.push(b);
    }
    var res = [];
    for (var j = 0; j < inner.length; j++) {
        var lines = inner[j].split("\n");
        for (var L = 0; L < lines.length; L++) res.push("> " + lines[L]);
        if (j < inner.length - 1) res.push(">");
    }
    return res.join("\n");
}

function __mpList(n, depth) {
    var ordered = (n.tagName || "").toLowerCase() === "ol";
    var start = 1;
    if (ordered) {
        var st = n.getAttribute("start");
        if (st !== null && st !== "") { var sv = parseInt(st, 10); if (!isNaN(sv)) start = sv; }
    }
    var lis = [];
    var ch = n.childNodes;
    for (var i = 0; i < ch.length; i++)
        if (ch[i].nodeType === 1 && (ch[i].tagName || "").toLowerCase() === "li") lis.push(ch[i]);
    var out = [];
    for (var j = 0; j < lis.length; j++) {
        var marker;
        if (ordered) { marker = __mpIndent(depth) + start + ". "; start++; }
        else marker = __mpIndent(depth) + "- ";
        out.push(__mpLi(lis[j], depth, marker));
    }
    return out.join("\n");
}

function __mpIsBlockTag(tag) {
    if (tag === "p" || tag === "div" || tag === "ul" || tag === "ol" ||
        tag === "blockquote" || tag === "pre" || tag === "table" ||
        tag === "hr") return true;
    return /^h[1-6]$/.test(tag);
}

function __mpLi(li, depth, marker) {
    var task = "";
    if (li.className && ((" " + li.className + " ").indexOf(" task-list-item ") >= 0)) {
        var inp = li.getElementsByTagName("input");
        if (inp && inp.length > 0 && (inp[0].type || "checkbox") === "checkbox")
            task = inp[0].checked ? "[x] " : "[ ] ";
    }
    var ind = __mpIndent(depth + 1);
    var cur = "";       // current inline run (tight item paragraph)
    var cont = [];      // continuation lines, in order {i, t}
    var head = [];      // first lines from inline runs, in order (usually one)
    var seen = false;

    function flush() {
        if (cur === "") return;
        var lines = cur.split("\n");
        head.push(lines[0]);
        for (var q = 1; q < lines.length; q++) cont.push({ i: ind, t: lines[q] });
        cur = "";
    }

    var kids = li.childNodes;
    for (var j = 0; j < kids.length; j++) {
        var k = kids[j];
        if (k.nodeType === 3) {
            // Skip structural newlines md4c leaves between block children.
            if (/^[ \t]*\r?\n[ \t]*$/.test(k.nodeValue)) continue;
            cur += __mpEscapeInline(k.nodeValue, false);
            seen = true;
            continue;
        }
        if (k.nodeType !== 1) continue;
        var tag = (k.tagName || "").toLowerCase();
        if (tag === "input") continue;
        if (tag === "br") { cur += "  \n"; seen = true; continue; }
        if (!__mpIsBlockTag(tag)) { cur += __mpInlineNode(k, false, false); seen = true; continue; }
        flush();                        // genuine block child below the item text
        if (tag === "ul" || tag === "ol") {
            var nested = __mpList(k, depth + 1);
            if (nested !== "") cont.push({ i: ind, t: nested });
        } else {
            var b = __mpBlock(k, depth + 1, false);
            if (b !== null && b !== "") {
                var bl = b.split("\n");
                cont.push({ i: ind, t: bl[0] });
                for (var x = 1; x < bl.length; x++) cont.push({ i: ind, t: bl[x] });
            }
        }
        seen = true;
    }
    flush();

    var firstLine = marker + task;
    if (head.length) firstLine += head[0];
    for (var h = 1; h < head.length; h++) cont.push({ i: ind, t: head[h] });
    if (!seen) return firstLine;
    if (cont.length === 0) return firstLine;
    var out = firstLine;
    for (var r = 0; r < cont.length; r++) {
        var tl = cont[r].t.split("\n");
        out += "\n" + cont[r].i + tl[0];
        for (var u = 1; u < tl.length; u++) out += "\n" + (tl[u] === "" ? "" : cont[r].i + tl[u]);
    }
    return out;
}

function __mpTable(n) {
    var trs = n.getElementsByTagName("tr");
    var rows = [];
    for (var i = 0; i < trs.length; i++) {
        var tr = trs[i];
        var ths = tr.getElementsByTagName("th");
        var tds = tr.getElementsByTagName("td");
        var cells = [];
        if (ths.length) for (var a = 0; a < ths.length; a++)
            cells.push({ v: __mpInline(ths[a], true), al: ths[a].getAttribute("align") || "" });
        else if (tds.length) for (var b = 0; b < tds.length; b++)
            cells.push({ v: __mpInline(tds[b], true), al: tds[b].getAttribute("align") || "" });
        if (cells.length) rows.push(cells);
    }
    if (!rows.length) return "";
    var out = [];
    var header = rows[0];
    var hd = [], sep = [];
    for (var h = 0; h < header.length; h++) {
        hd.push(" " + header[h].v + " ");
        var al = header[h].al;
        if (al === "center") sep.push(" :---: ");
        else if (al === "right") sep.push(" ---: ");
        else sep.push(" --- ");
    }
    out.push("|" + hd.join("|") + "|");
    out.push("|" + sep.join("|") + "|");
    for (var r = 1; r < rows.length; r++) {
        var c2 = [];
        for (var c3 = 0; c3 < rows[r].length; c3++) c2.push(" " + rows[r][c3].v + " ");
        out.push("|" + c2.join("|") + "|");
    }
    return out.join("\n");
}

function __mpFootnotes(n) {
    var out = [];
    var lis = n.getElementsByTagName("li");
    for (var i = 0; i < lis.length; i++) {
        var li = lis[i];
        var idattr = li.getAttribute("id") || "";
        var m = /^fn-(\d+)$/.exec(idattr);
        if (!m) continue;
        var lines = [];
        var kids = li.childNodes;
        for (var j = 0; j < kids.length; j++) {
            var k = kids[j];
            if (k.nodeType === 3) {
                var s = __mpPlainTextBlock(k.nodeValue);
                if (s) lines.push(s);
                continue;
            }
            if (k.nodeType !== 1) continue;
            var tag = (k.tagName || "").toLowerCase();
            if (tag === "a") continue;             // footnote backref link
            if (tag === "p" || tag === "div") {
                var inner = __mpInline(k, false).replace(/\s+$/g, "");
                if (inner) lines.push(inner);
            } else {
                var b = __mpBlock(k, 0, false);
                if (b) lines.push(b);
            }
        }
        var one = "[^" + m[1] + "]: ";
        if (lines.length) one += lines[0];
        out.push(one);
        for (var z = 1; z < lines.length; z++) {
            var sub = lines[z].split("\n");
            out.push("    " + sub[0]);
            for (var zz = 1; zz < sub.length; zz++) out.push(sub[zz] === "" ? "" : "    " + sub[zz]);
        }
    }
    return out.join("\n");
}

function __mpBlock(n, depth, quote) {
    if (n.nodeType === 3) {
        return __mpPlainTextBlock(n.nodeValue);
    }
    if (n.nodeType !== 1) return "";
    var tag = (n.tagName || "").toLowerCase();
    if (tag === "br" || tag === "input") return "";
    var h = /^h([1-6])$/.exec(tag);
    if (h) {
        var hs = "";
        for (var k = 0; k < parseInt(h[1], 10); k++) hs += "#";
        return hs + " " + __mpInline(n, false);
    }
    if (tag === "p" || tag === "div") return __mpParaText(n);
    if (tag === "ul" || tag === "ol") return __mpList(n, depth);
    if (tag === "blockquote") return __mpQuote(n, depth);
    if (tag === "pre") return __mpCodeBlock(n, depth);
    if (tag === "table") return __mpTable(n);
    if (tag === "hr") return "---";
    return __mpInline(n, false);
}

function __mpSetDirtyFlag() {
    try { document.body.setAttribute("data-markpeek-dirty", "1"); } catch (e) {}
    __mpDirty = true;
}

function __mpClearDirty() {
    __mpDirty = false;
    try { document.body.removeAttribute("data-markpeek-dirty"); } catch (e) {}
}

function __mpSetMode(on) {
    var c = document.getElementById("markpeek-content");
    if (!c) return;
    document.body.className = on ? "mp-editing" : "";
    c.contentEditable = on ? "true" : "false";
    c.oninput = on ? __mpSetDirtyFlag : null;
    __mpDirty = false;
    try { document.body.removeAttribute("data-markpeek-dirty"); } catch (e) {}
    try { c.focus(); } catch (e) {}
}

// Runs the DOM -> Markdown conversion and hands the result to the host.
// document.title collapses newlines, so the result is stored in a BODY
// ATTRIBUTE (newlines are preserved there) and read back via getAttribute:
//   success -> data-markpeek-result = "<markdown>"   (data-markpeek-error removed)
//   error   -> data-markpeek-error = "<message>"     (data-markpeek-result removed)
function __mpExport() {
    try {
        var md = __mpToMd();
        __mpResult = md;
        document.body.setAttribute("data-markpeek-result", md);
        document.body.removeAttribute("data-markpeek-error");
        return md;
    } catch (e) {
        var msg = (e && e.message) ? String(e.message) : String(e);
        document.body.setAttribute("data-markpeek-error", msg);
        document.body.removeAttribute("data-markpeek-result");
        return "";
    }
}

function __mpToMd() {
    var c = document.getElementById("markpeek-content");
    var out = "";
    if (c) {
        var main = [], defs = [];
        var kids = c.childNodes;
        for (var i = 0; i < kids.length; i++) {
            var k = kids[i];
            if (k.nodeType === 3) {
                var s = __mpPlainTextBlock(k.nodeValue);
                if (s) main.push(s);
                continue;
            }
            if (k.nodeType !== 1) continue;
            var tag = (k.tagName || "").toLowerCase();
            if (tag === "section" && k.className && ((" " + k.className + " ").indexOf(" footnotes ") >= 0)) {
                var d = __mpFootnotes(k);
                if (d !== "") defs.push(d);
                continue;
            }
            var b = __mpBlock(k, 0, false);
            if (b !== null && b !== "") main.push(b);
        }
        var parts = [];
        for (var m = 0; m < main.length; m++) if (main[m] !== "") parts.push(main[m]);
        for (var f = 0; f < defs.length; f++) if (defs[f] !== "") parts.push(defs[f]);
        out = parts.join("\n\n");
        if (out !== "" && out.charAt(out.length - 1) !== "\n") out += "\n";
    }
    __mpResult = out;
    return out;
}
)JS";

static std::string BuildHtml(const std::string& bodyHtml, const std::wstring& titleW,
                             const std::wstring& baseDirW) {
    std::string title = EscapeHtml(WideToUtf8(titleW.c_str()));
    std::string jsBase;
    if (!baseDirW.empty()) {
        jsBase = WideToUtf8((L"file:///" + baseDirW).c_str());
        for (char& c : jsBase) if (c == '\\') c = '/';
        jsBase = EscapeHtml(jsBase);
    }
    std::string html;
    html.reserve(bodyHtml.size() + 8192);
    html += "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n";
    html += "<meta http-equiv=\"X-UA-Compatible\" content=\"IE=edge\">\n";
    html += "<title>" + title + "</title>\n<style>\n";
    html += kCss;
    html += "\n</style>\n<script>\n";
    html += kEditorJs;
    html += "\nvar __mpBase = \"";
    html += jsBase;
    html += "\";\n</script>\n</head>\n<body>\n<div id=\"markpeek-content\">\n";
    html += bodyHtml;
    html += "\n</div>\n</body>\n</html>\n";
    return html;
}

// ---------------------------------------------------------------------------
// WebBrowser control + JS bridge

static void ShowHtml(const std::string& html) {
    if (!g_wb) return;
    if (!g_tempHtmlPath.empty()) {
        DeleteFileW(g_tempHtmlPath.c_str());
        g_tempHtmlPath.clear();
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t buf[64];
    swprintf(buf, 64, L"MarkPeek_%lu.html", GetCurrentProcessId());
    g_tempHtmlPath = std::wstring(tmp) + buf;
    std::ofstream out(g_tempHtmlPath.c_str(), std::ios::binary);
    out.write(html.data(), (std::streamsize)html.size());
    out.close();

    std::wstring url = L"file:///" + g_tempHtmlPath;
    for (wchar_t& c : url) if (c == L'\\') c = L'/';

    VARIANT vUrl;
    VariantInit(&vUrl);
    vUrl.vt = VT_BSTR;
    vUrl.bstrVal = SysAllocString(url.c_str());
    g_wb->Navigate2(&vUrl, NULL, NULL, NULL, NULL);
    VariantClear(&vUrl);
}

static IHTMLDocument2* GetDoc() {
    if (!g_wb) return NULL;
    IDispatch* disp = NULL;
    if (FAILED(g_wb->get_Document(&disp)) || !disp) return NULL;
    IHTMLDocument2* d2 = NULL;
    disp->QueryInterface(IID_IHTMLDocument2, (void**)&d2);
    disp->Release();
    return d2;
}

static IHTMLWindow2* GetWin() {
    IHTMLDocument2* d = GetDoc();
    if (!d) return NULL;
    IHTMLWindow2* w = NULL;
    d->get_parentWindow(&w);
    d->Release();
    return w;
}

// Run a snippet of JavaScript in the page. Returns false if the page isn't ready.
static bool JsEval(const wchar_t* code) {
    IHTMLWindow2* w = GetWin();
    if (!w) return false;
    BSTR b = SysAllocString(code);
    BSTR lang = SysAllocString(L"javascript");
    VARIANT ret;
    VariantInit(&ret);
    HRESULT hr = w->execScript(b, lang, &ret);
    SysFreeString(b);
    SysFreeString(lang);
    VariantClear(&ret);
    w->Release();
    return SUCCEEDED(hr);
}

// Read a string attribute from the page's <body> element (plain COM
// getAttribute, reliable in Trident). Attribute values preserve newlines.
static bool GetBodyAttribute(const wchar_t* aname, std::wstring& out) {
    out.clear();
    IHTMLDocument2* d = GetDoc();
    if (!d) return false;
    IHTMLElement* body = NULL;
    d->get_body(&body);
    d->Release();
    if (!body) return false;
    VARIANT v;
    VariantInit(&v);
    BSTR n = SysAllocString(aname);
    bool ok = false;
    HRESULT hr = n ? body->getAttribute(n, 0, &v) : E_FAIL;
    if (SUCCEEDED(hr) && v.vt == VT_BSTR && v.bstrVal) {
        out = v.bstrVal;
        ok = true;
    }
    if (n) SysFreeString(n);
    VariantClear(&v);
    body->Release();
    return ok;
}

// Calls __mpExport() and returns the resulting Markdown.
// The JS stores the full text (with newlines) in a <body> attribute
// data-markpeek-result on success, or data-markpeek-error on failure.
// Returns false (and fills errMsg if known) when the export failed.
static bool JsExportMarkdown(std::wstring& outMd, std::wstring& errMsg) {
    outMd.clear();
    errMsg.clear();

    IHTMLWindow2* w = GetWin();
    if (!w) { errMsg = L"document not ready (no window)"; return false; }
    BSTR code = SysAllocString(L"__mpExport();");
    BSTR lang = SysAllocString(L"javascript");
    VARIANT ret;
    VariantInit(&ret);
    HRESULT hr = w->execScript(code, lang, &ret);
    SysFreeString(code);
    SysFreeString(lang);

    // 1) If execScript returned the text directly, use it (newlines intact).
    if (SUCCEEDED(hr) && ret.vt == VT_BSTR && ret.bstrVal && SysStringLen(ret.bstrVal) > 0) {
        outMd = ret.bstrVal;
        VariantClear(&ret);
        w->Release();
        return true;
    }
    VariantClear(&ret);
    w->Release();

    // 2) error reported by the page?
    std::wstring ea;
    if (GetBodyAttribute(L"data-markpeek-error", ea) && !ea.empty()) {
        errMsg = ea;
        return false;
    }

    // 3) success: read the result (newline-preserving) from the body attribute
    std::wstring val;
    if (GetBodyAttribute(L"data-markpeek-result", val)) { outMd = val; return true; }

    errMsg = L"document not ready (no script result)";
    return false;
}

// Dirty state is flagged by the page on the <body> element's
// data-markpeek-dirty attribute.
static bool JsGetDirty() {
    std::wstring v;
    if (!GetBodyAttribute(L"data-markpeek-dirty", v)) return false;
    return !v.empty() && v != L"0";
}

static void SetStatus(const std::wstring& text) {
    if (g_hwndStatus)
        SendMessageW(g_hwndStatus, SB_SETTEXT, 0, (LPARAM)text.c_str());
}

static void UpdateTitle() {
    if (g_currentPath.empty()) return;
    std::wstring t = BaseName(g_currentPath);
    if (g_dirty) t += L" *";
    t += L" - " APP_NAME;
    SetWindowTextW(g_hwnd, t.c_str());
}

// ---------------------------------------------------------------------------
// Edit mode (WYSIWYG, same rendered document)

static bool PromptSaveIfDirty();
static void EnterEditMode();
static void LeaveEditMode();

static bool SaveCurrentFile() {
    if (!g_editing || g_currentPath.empty()) return false;
    std::wstring mdW, errMsg;
    if (!JsExportMarkdown(mdW, errMsg)) {
        std::wstring msg = L"Could not convert the edited document back to Markdown.";
        if (!errMsg.empty()) msg += L"\n\nDetails: " + errMsg;
        MessageBoxW(g_hwnd, msg.c_str(), APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }
    std::string utf8 = WideToUtf8(mdW.c_str(), (int)mdW.size());
    std::ofstream out(g_currentPath.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        MessageBoxW(g_hwnd, (L"Could not write file:\n" + g_currentPath).c_str(),
                    APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }
    out.write(utf8.data(), (std::streamsize)utf8.size());
    out.close();
    g_dirty = false;
    UpdateTitle();
    SetStatus(L"Saved");
    return true;
}

static bool PromptSaveIfDirty() {
    if (!g_editing || !g_dirty) return true;
    int r = MessageBoxW(g_hwnd, L"Save changes to the document?", APP_NAME,
                        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDYES) return SaveCurrentFile();
    return r == IDNO;
}

static void EnterEditMode() {
    if (g_editing) return;
    if (g_currentPath.empty()) {
        SendMessageW(g_hwnd, WM_COMMAND, IDM_OPEN, 0);
        if (g_currentPath.empty()) return;
    }
    if (!JsEval(L"__mpSetMode(true);")) {
        SetStatus(L"Document not ready yet - try again in a moment");
        return;
    }
    g_editing = true;
    g_dirty = false;
    if (!g_dirtyTimer) g_dirtyTimer = SetTimer(g_hwnd, DIRTY_TIMER_ID, DIRTY_POLL_MS, NULL);
    UpdateTitle();
    SetStatus(L"Editing - Ctrl+S to save, Ctrl+E to preview (WYSIWYG)");
}

static void LeaveEditMode() {
    if (!g_editing) return;
    JsEval(L"__mpSetMode(false);");
    g_editing = false;
    if (g_dirtyTimer) {
        KillTimer(g_hwnd, g_dirtyTimer);
        g_dirtyTimer = 0;
    }
    UpdateTitle();
    SetStatus(L"Preview - Ctrl+E to edit, Ctrl+S to save");
}

static void OpenFile(const std::wstring& path) {
    if (g_editing) LeaveEditMode();       // keep the document; we navigate anyway
    if (!FileExists(path)) {
        std::wstring msg = L"File not found:\n" + path;
        MessageBoxW(g_hwnd, msg.c_str(), APP_NAME, MB_OK | MB_ICONWARNING);
        return;
    }
    std::string md = ReadFileUtf8(path);
    std::string body;
    if (!MdToHtml(md, body)) body = "<p><em>(render error)</em></p>";
    AbsolutizeImageSrcs(body, DirOf(path));

    std::wstring name = BaseName(path);
    std::string html = BuildHtml(body, name, DirOf(path));
    ShowHtml(html);
    g_currentPath = path;
    g_dirty = false;
    UpdateTitle();

    int lines = 0;
    for (char c : md) if (c == '\n') lines++;
    wchar_t st[2600];
    swprintf(st, 2600, L"%s   |   %d lines", path.c_str(), lines);
    SetStatus(st);
}

static void ShowWelcome() {
    const wchar_t* welcome =
        L"# Welcome to MarkPeek\n\n"
        L"A minimal, Typora-style Markdown viewer & editor for Windows.\n\n"
        L"## Get started\n\n"
        L"- Press **Ctrl+O** or drag & drop a `.md` file into this window\n"
        L"- Press **Ctrl+E** to edit right in the rendered view (WYSIWYG)\n"
        L"- While editing, **Ctrl+S** saves; **Ctrl+A**, **Ctrl+C/V/X/Z** and other\n"
        L"  shortcuts work in any keyboard layout\n"
        L"- Right-click a `.md` file in Explorer -> **Open with** -> MarkPeek\n\n"
        L"## Features\n\n"
        L"- Clean, Typora-like design\n"
        L"- CommonMark + GitHub tables, task lists, strikethrough\n"
        L"- UTF-8 / UTF-16 files\n"
        L"- Portable: no installation, no dependencies\n\n"
        L"> Tip: use *File -> Set as default viewer for .md* to open `.md` files from Explorer.";
    std::string md = WideToUtf8(welcome);
    std::string body;
    MdToHtml(md, body);
    std::string html = BuildHtml(body, L"Welcome", L"");
    ShowHtml(html);
    g_currentPath.clear();
    g_dirty = false;
    SetWindowTextW(g_hwnd, (std::wstring(APP_NAME) + L" - drop or open a Markdown file").c_str());
    SetStatus(L"Ready - open a .md file (Ctrl+O) or drop it here");
}

// ---------------------------------------------------------------------------
// Registry: IE document mode for this exe (best CSS on Win7/10)

static void SetIeEmulation() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    name = name ? name + 1 : exe;
    HKEY key = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Internet Explorer\\Main\\FeatureControl\\FEATURE_BROWSER_EMULATION",
            0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        DWORD v = 11001;  // IE11 edge mode; clamps down on older IE versions
        RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(key);
    }
}

// ---------------------------------------------------------------------------
// .md file association (per-user, no admin needed)

static void RegWriteString(HKEY root, const wchar_t* subkey, const wchar_t* name,
                           const std::wstring& value) {
    HKEY key = NULL;
    if (RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                       (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

static bool IsOurAssoc() {
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.md", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        wchar_t buf[64] = {0};
        DWORD sz = sizeof(buf);
        LONG r = RegQueryValueExW(key, L"", NULL, NULL, (BYTE*)buf, &sz);
        RegCloseKey(key);
        return r == ERROR_SUCCESS && wcscmp(buf, L"MarkPeek.md") == 0;
    }
    return false;
}

static void SetAssociation(bool on) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring exeStr = exe;
    if (on) {
        RegWriteString(HKEY_CURRENT_USER, L"Software\\Classes\\.md", L"", L"MarkPeek.md");
        RegWriteString(HKEY_CURRENT_USER, L"Software\\Classes\\MarkPeek.md", L"", L"Markdown Document");
        RegWriteString(HKEY_CURRENT_USER, L"Software\\Classes\\MarkPeek.md\\DefaultIcon", L"",
                       exeStr + L",0");
        RegWriteString(HKEY_CURRENT_USER, L"Software\\Classes\\MarkPeek.md\\shell\\open\\command", L"",
                       L"\"" + exeStr + L"\" \"%1\"");
        RegWriteString(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\MarkPeek.exe\\shell\\open\\command", L"",
                       L"\"" + exeStr + L"\" \"%1\"");
        MessageBoxW(g_hwnd, L"MarkPeek is now the default viewer for .md files (current user).",
                    APP_NAME, MB_OK | MB_ICONINFORMATION);
    } else {
        SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\MarkPeek.md");
        if (IsOurAssoc())
            SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\.md");
        SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\MarkPeek.exe");
        MessageBoxW(g_hwnd, L".md association removed.", APP_NAME, MB_OK | MB_ICONINFORMATION);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

// ---------------------------------------------------------------------------
// WebBrowser control host (IOleClientSite / IOleInPlaceSite / IOleInPlaceFrame
// / IOleControlSite) — classic EXEBrowser pattern.

class BrowserSite : public IOleClientSite, public IOleInPlaceSite,
                    public IOleInPlaceFrame, public IOleControlSite {
public:
    BrowserSite(HWND hwnd) : m_ref(1), m_hwnd(hwnd) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (riid == IID_IUnknown || riid == IID_IOleClientSite) *ppv = (IOleClientSite*)this;
        else if (riid == IID_IOleInPlaceSite) *ppv = (IOleInPlaceSite*)this;
        else if (riid == IID_IOleInPlaceFrame || riid == IID_IOleInPlaceUIWindow) *ppv = (IOleInPlaceFrame*)this;
        else if (riid == IID_IOleControlSite) *ppv = (IOleControlSite*)this;
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() { return ++m_ref; }
    STDMETHODIMP_(ULONG) Release() {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    // IOleClientSite
    STDMETHODIMP SaveObject() { return E_NOTIMPL; }
    STDMETHODIMP GetMoniker(DWORD, DWORD, IMoniker** ppMk) { if (ppMk) *ppMk = NULL; return E_NOTIMPL; }
    STDMETHODIMP GetContainer(IOleContainer** ppC) { if (ppC) *ppC = NULL; return E_NOINTERFACE; }
    STDMETHODIMP ShowObject() { return S_OK; }
    STDMETHODIMP OnShowWindow(BOOL) { return S_OK; }
    STDMETHODIMP RequestNewObjectLayout() { return E_NOTIMPL; }

    // IOleInPlaceSite / IOleWindow
    STDMETHODIMP GetWindow(HWND* phwnd) { if (phwnd) *phwnd = m_hwnd; return S_OK; }
    STDMETHODIMP ContextSensitiveHelp(BOOL) { return E_NOTIMPL; }
    STDMETHODIMP CanInPlaceActivate() { return S_OK; }
    STDMETHODIMP OnInPlaceActivate() { return S_OK; }
    STDMETHODIMP OnUIActivate() { return S_OK; }
    STDMETHODIMP GetWindowContext(IOleInPlaceFrame** ppFrame, IOleInPlaceUIWindow** ppDoc,
                                  LPRECT prcPos, LPRECT prcClip,
                                  LPOLEINPLACEFRAMEINFO pFrameInfo) {
        if (ppFrame) { *ppFrame = (IOleInPlaceFrame*)this; AddRef(); }
        if (ppDoc) *ppDoc = NULL;
        if (prcPos) GetClientRect(m_hwnd, prcPos);
        if (prcClip) GetClientRect(m_hwnd, prcClip);
        if (pFrameInfo) {
            pFrameInfo->cb = sizeof(OLEINPLACEFRAMEINFO);
            pFrameInfo->fMDIApp = FALSE;
            pFrameInfo->hwndFrame = m_hwnd;
            pFrameInfo->haccel = NULL;
            pFrameInfo->cAccelEntries = 0;
        }
        return S_OK;
    }
    STDMETHODIMP Scroll(SIZE) { return E_NOTIMPL; }
    STDMETHODIMP OnUIDeactivate(BOOL) { return S_OK; }
    STDMETHODIMP OnInPlaceDeactivate() { return S_OK; }
    STDMETHODIMP DiscardUndoState() { return E_NOTIMPL; }
    STDMETHODIMP DeactivateAndUndo() { return E_NOTIMPL; }
    STDMETHODIMP OnPosRectChange(LPCRECT) { return S_OK; }

    // IOleInPlaceFrame / IOleInPlaceUIWindow
    STDMETHODIMP GetBorder(LPRECT) { return E_NOTIMPL; }
    STDMETHODIMP RequestBorderSpace(LPCBORDERWIDTHS) { return E_NOTIMPL; }
    STDMETHODIMP SetBorderSpace(LPCBORDERWIDTHS) { return E_NOTIMPL; }
    STDMETHODIMP SetActiveObject(IOleInPlaceActiveObject*, LPCOLESTR) { return S_OK; }
    STDMETHODIMP InsertMenus(HMENU, LPOLEMENUGROUPWIDTHS) { return E_NOTIMPL; }
    STDMETHODIMP SetMenu(HMENU, HOLEMENU, HWND) { return S_OK; }
    STDMETHODIMP RemoveMenus(HMENU) { return E_NOTIMPL; }
    STDMETHODIMP SetStatusText(LPCOLESTR) { return S_OK; }
    STDMETHODIMP EnableModeless(BOOL) { return S_OK; }
    STDMETHODIMP TranslateAccelerator(LPMSG, WORD) { return E_NOTIMPL; }

    // IOleControlSite
    STDMETHODIMP OnControlInfoChanged() { return S_OK; }
    STDMETHODIMP LockInPlaceActive(BOOL) { return S_OK; }
    STDMETHODIMP GetExtendedControl(IDispatch** ppDisp) { if (ppDisp) *ppDisp = NULL; return E_NOTIMPL; }
    STDMETHODIMP TransformCoords(POINTL*, POINTF*, DWORD) { return E_NOTIMPL; }
    STDMETHODIMP TranslateAccelerator(LPMSG, DWORD) { return E_NOTIMPL; }
    STDMETHODIMP OnFocus(BOOL) { return S_OK; }
    STDMETHODIMP ShowPropertyFrame() { return E_NOTIMPL; }

private:
    ULONG m_ref;
    HWND m_hwnd;
};

static BrowserSite* g_site = NULL;

static void CreateBrowser(HWND hwnd) {
    g_site = new BrowserSite(hwnd);
    HRESULT hr = CoCreateInstance(CLSID_WebBrowser, NULL, CLSCTX_INPROC_SERVER,
                                  IID_IWebBrowser2, (void**)&g_wb);
    if (FAILED(hr) || !g_wb) return;
    IOleObject* ole = NULL;
    g_wb->QueryInterface(IID_IOleObject, (void**)&ole);
    if (ole) {
        ole->SetClientSite((IOleClientSite*)g_site);
        RECT rc;
        GetClientRect(hwnd, &rc);
        ole->DoVerb(OLEIVERB_INPLACEACTIVATE, NULL, (IOleClientSite*)g_site, 0, hwnd, &rc);
        ole->Release();
    }
    IOleWindow* ow = NULL;
    g_wb->QueryInterface(IID_IOleWindow, (void**)&ow);
    if (ow) {
        ow->GetWindow(&g_hwndBrowser);
        ow->Release();
    }
    g_wb->put_Left(0);
    g_wb->put_Top(0);
    g_wb->put_Visible(TRUE);
    g_wb->QueryInterface(IID_IOleInPlaceActiveObject, (void**)&g_pioipa);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hwndStatus = CreateStatusWindowW(WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                           L"", hwnd, IDC_STATUS);
        DragAcceptFiles(hwnd, TRUE);
        CreateBrowser(hwnd);
        return 0;

    case WM_SIZE:
        if (g_hwndBrowser) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (g_hwndStatus) {
                SendMessageW(g_hwndStatus, WM_SIZE, 0, 0);
                RECT sr;
                GetWindowRect(g_hwndStatus, &sr);
                rc.bottom -= (sr.bottom - sr.top);
            }
            SetWindowPos(g_hwndBrowser, NULL, 0, 0, rc.right, rc.bottom, SWP_NOZORDER);
        }
        return 0;

    case WM_TIMER:
        if (wParam == DIRTY_TIMER_ID && g_editing) {
            if (JsGetDirty()) {
                g_dirty = true;
                UpdateTitle();
                JsEval(L"__mpClearDirty();");
            }
        }
        return 0;

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        wchar_t buf[MAX_PATH];
        if (DragQueryFileW(hDrop, 0, buf, MAX_PATH) > 0) {
            if (g_editing && !PromptSaveIfDirty()) break;
            OpenFile(buf);
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_CLOSE:
        if (g_editing && !PromptSaveIfDirty()) return 0;   // cancelled
        DestroyWindow(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_OPEN: {
            if (g_editing && !PromptSaveIfDirty()) return 0;
            wchar_t file[MAX_PATH] = {0};
            OPENFILENAMEW ofn;
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter =
                L"Markdown files (*.md;*.markdown;*.txt)\0*.md;*.markdown;*.txt\0"
                L"All files (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            if (GetOpenFileNameW(&ofn)) {
                LeaveEditMode();
                OpenFile(file);
            }
            return 0;
        }
        case IDM_RELOAD:
            if (g_editing && !PromptSaveIfDirty()) return 0;
            if (!g_currentPath.empty()) OpenFile(g_currentPath);
            return 0;
        case IDM_EDIT:
            if (g_editing) {
                if (!PromptSaveIfDirty()) return 0;      // cancelled
                bool wasDirty = g_dirty;                 // still dirty if user chose No
                LeaveEditMode();
                if (wasDirty && !g_currentPath.empty()) OpenFile(g_currentPath);
            } else {
                EnterEditMode();
            }
            return 0;
        case IDM_SAVE:
            if (g_editing) SaveCurrentFile();
            else { EnterEditMode(); SaveCurrentFile(); }
            return 0;
        case IDM_ASSOC:
            SetAssociation(true);
            return 0;
        case IDM_UNASSOC:
            SetAssociation(false);
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        case IDM_ABOUT:
            MessageBoxW(hwnd,
                L"MarkPeek " APP_VERSION L"\n"
                L"A minimal Typora-style Markdown viewer & editor.\n\n"
                L"Rendered with md4c + embedded IE (MSHTML).\n"
                L"MIT License. Windows 7 / 10, portable.",
                APP_NAME, MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        return 0;

    case WM_DESTROY:
        if (g_wb) {
            g_wb->Stop();
            g_wb->Release();
            g_wb = NULL;
        }
        if (g_pioipa) {
            g_pioipa->Release();
            g_pioipa = NULL;
        }
        if (g_site) {
            g_site->Release();
            g_site = NULL;
        }
        if (g_dirtyTimer) {
            KillTimer(hwnd, g_dirtyTimer);
            g_dirtyTimer = 0;
        }
        if (!g_tempHtmlPath.empty()) {
            DeleteFileW(g_tempHtmlPath.c_str());
            g_tempHtmlPath.clear();
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    SetIeEmulation();
    OleInitialize(NULL);

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_ICON1));
    wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_ICON1));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MarkPeekWindow";
    if (!RegisterClassExW(&wc)) return 1;

    HMENU menu = CreateMenu();
    HMENU fileMenu = CreatePopupMenu();
    AppendMenuW(fileMenu, MF_STRING, IDM_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(fileMenu, MF_STRING, IDM_RELOAD, L"&Reload\tF5");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(fileMenu, MF_STRING, IDM_EDIT, L"&Edit / Preview\tCtrl+E");
    AppendMenuW(fileMenu, MF_STRING, IDM_SAVE, L"&Save\tCtrl+S");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(fileMenu, MF_STRING, IDM_ASSOC, L"Set as default viewer for &MD files");
    AppendMenuW(fileMenu, MF_STRING, IDM_UNASSOC, L"&Remove MD association");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(fileMenu, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)fileMenu, L"&File");
    HMENU helpMenu = CreatePopupMenu();
    AppendMenuW(helpMenu, MF_STRING, IDM_ABOUT, L"&About");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)helpMenu, L"&Help");

    g_hwnd = CreateWindowExW(0, L"MarkPeekWindow", APP_NAME, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 960, 720,
                             NULL, menu, hInst, NULL);
    if (!g_hwnd) return 1;

    ACCEL accels[] = {
        { FCONTROL | FVIRTKEY, 'O', IDM_OPEN },
        { FVIRTKEY, VK_F5, IDM_RELOAD },
        { FCONTROL | FVIRTKEY, 'E', IDM_EDIT },
        { FCONTROL | FVIRTKEY, 'S', IDM_SAVE },
    };
    HACCEL hAccel = CreateAcceleratorTableW(accels, 4);

    int nArgs = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    std::wstring initial;
    if (nArgs > 1 && argv) initial = argv[1];
    if (argv) LocalFree(argv);

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    if (!initial.empty() && FileExists(initial))
        OpenFile(initial);
    else
        ShowWelcome();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!TranslateAcceleratorW(g_hwnd, hAccel, &msg)) {
            BOOL handled = FALSE;
            if (!g_editing && g_pioipa)
                handled = (g_pioipa->TranslateAccelerator(&msg) == S_OK);
            if (!handled) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    if (hAccel) DestroyAcceleratorTable(hAccel);
    OleUninitialize();
    return (int)msg.wParam;
}
