// AUTO-GENERATED from src/main.cpp by tools/gen_shared_assets.py — do not edit.
// Shared WYSIWYG editor JS (runs inside the rendered page in both builds).
// Keep in sync with the kEditorJs string in src/main.cpp.
#pragma once

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
