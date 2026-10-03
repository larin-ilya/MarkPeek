// MarkPeek GTK — a minimal Typora-style Markdown viewer/editor for Linux.
// GTK3 + WebKitGTK + md4c renderer. MIT License.
//
// Same WYSIWYG model as the Windows version: the rendered document becomes
// contenteditable (Ctrl+E), and a small JS inside the page serializes the
// edited DOM back to Markdown on save (Ctrl+S). The editor JS/CSS are shared
// with the Windows build: tools/gen_shared_assets.py extracts them from
// src/main.cpp into src/shared/*.h, so both platforms stay in sync.

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <gdk/gdkkeysyms.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "md4c/md4c.h"
#include "md4c/md4c-html.h"
#include "shared/shared_css.h"
#include "shared/editor_js.h"

#define APP_NAME "MarkPeek"
#define APP_VERSION "1.3.1"

// Optional self-test hook (used by ci/smoke_test.sh): when MP_SMOKE_JS is set
// the app evaluates that JS after the requested file renders and writes the
// JSON result to MP_SMOKE_OUT, then exits. Production runs never set these.
static std::string g_smokeJsFile;
static std::string g_smokeOutFile;

static GtkWidget* g_window = NULL;
static GtkWidget* g_webview = NULL;
static GtkWidget* g_status = NULL;
static GtkWidget* g_btn_edit = NULL;
static std::string g_currentPath;
static bool g_editing = false;
static bool g_dirty = false;
// Files passed on the command line are opened from an idle callback: webkit_web_view_load_html()
// issued directly inside the GApplication "open" handler can race the main loop startup (seen with
// WebKitGTK 2.5x: the web process never spawns and the page stays empty). Deferring fixes it.
static std::vector<std::string> g_pendingOpenFiles;

// ---------------------------------------------------------------------------
// Small helpers

static void UpdateTitle() {
    if (g_currentPath.empty()) {
        gtk_window_set_title(GTK_WINDOW(g_window),
                             APP_NAME " - drop or open a Markdown file");
        return;
    }
    const char* base = strrchr(g_currentPath.c_str(), G_DIR_SEPARATOR);
    base = base ? base + 1 : g_currentPath.c_str();
    std::string t = std::string(base) + (g_dirty ? " *" : "") + " - " APP_NAME;
    gtk_window_set_title(GTK_WINDOW(g_window), t.c_str());
}

static void SetStatus(const std::string& text) {
    if (!g_status) return;
    GtkStatusbar* sb = GTK_STATUSBAR(g_status);
    static guint ctx = gtk_statusbar_get_context_id(sb, "markpeek-status");
    gtk_statusbar_pop(sb, ctx);
    gtk_statusbar_push(sb, ctx, text.c_str());
}

// Icon-theme helper: in minimal/container environments (WSL1, some CI images)
// gdk-pixbuf cannot decode theme icons (no memfd_create in the kernel) and GTK
// aborts on any image widget render. Probe-load each icon ONCE; only attach an
// image widget when the theme can actually decode it, else fall back to text.
static bool IconThemeCanLoad(const char* name) {
    GtkIconTheme* theme = gtk_icon_theme_get_default();
    if (!theme) return false;
    GError* err = NULL;
    GdkPixbuf* pb = gtk_icon_theme_load_icon(theme, name, 16,
                                             GTK_ICON_LOOKUP_USE_BUILTIN, &err);
    if (err) {
        g_error_free(err);
        // gtk_icon_theme_load_icon() with an error may have already scheduled a
        // g_error abort inside GTK on broken platforms - it does not: the abort
        // happens at RENDER time of a failed GdkPixbuf, not at load. Safe.
        return false;
    }
    if (pb) { g_object_unref(pb); return true; }
    return false;
}

#define IconThemeHas IconThemeCanLoad

static GtkWidget* MakeIconButton(const char* icon_name, const char* label) {
    if (IconThemeHas(icon_name))
        return gtk_button_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
    return gtk_button_new_with_mnemonic(label);
}

static void UpdateEditButton() {
    if (!g_btn_edit) return;
    gtk_button_set_label(GTK_BUTTON(g_btn_edit), g_editing ? "Preview" : "Edit");
    const char* icon = g_editing ? "view-paged-symbolic" : "document-edit-symbolic";
    if (IconThemeHas(icon))
        gtk_button_set_image(GTK_BUTTON(g_btn_edit),
                             gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU));
    else
        gtk_button_set_image(GTK_BUTTON(g_btn_edit), NULL);
}


// Icon-theme helper: in minimal/container environments (WSL1, some CI images)
// gdk-pixbuf cannot decode SVG theme icons (no memfd_create) and GTK aborts.
// Only use named icons when the theme can actually load them; fall back to a
// plain text button so the app stays alive everywhere.
static bool FileExists(const std::string& p) {
    return g_file_test(p.c_str(), G_FILE_TEST_EXISTS) &&
           !g_file_test(p.c_str(), G_FILE_TEST_IS_DIR);
}

static std::string DirOf(const std::string& path) {
    size_t p = path.find_last_of(G_DIR_SEPARATOR);
    return (p == std::string::npos) ? std::string() : path.substr(0, p + 1);
}

static std::string BaseName(const std::string& path) {
    size_t p = path.find_last_of(G_DIR_SEPARATOR);
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

static bool HasMdExt(const std::string& p) {
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = p.substr(dot + 1);
    for (char& c : ext) c = (char)g_ascii_tolower(c);
    return ext == "md" || ext == "markdown" || ext == "txt";
}

// Escapes & < > " for embedding in HTML attributes/text.
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

// Heuristic detection of BOM-less Windows-1251 (Cyrillic ANSI) text:
// valid UTF-8 would not contain 0xC0-0xFF followed by invalid continuation
// bytes. Counts bytes that form invalid UTF-8 sequences in the Cyrillic
// uppercase/lowercase CP1251 range and checks a marker byte frequency.
static bool LooksLikeWindows1251(const std::string& bytes) {
    size_t n = bytes.size();
    if (n == 0) return false;
    size_t badSeq = 0;      // invalid UTF-8 sequences
    size_t hiCyr = 0;       // bytes in 0xC0..0xFF (CP1251 Cyrillic range)
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c < 0x80) continue;
        if (c >= 0xC0) hiCyr++;
        size_t need = 0;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else { badSeq++; continue; }
        bool ok = (i + need < n);
        for (size_t k = 1; ok && k <= need; k++)
            if (((unsigned char)bytes[i + k] & 0xC0) != 0x80) ok = false;
        if (!ok) { badSeq++; continue; }
        i += need;
    }
    // CP1251 text: many invalid sequences in the Cyrillic byte range;
    // valid UTF-8: no invalid sequences at all.
    if (badSeq == 0) return false;
    return hiCyr > 0 && badSeq >= (hiCyr / 2);
}

static std::string Windows1251ToUtf8(const std::string& bytes) {
    GError* err = NULL;
    gsize written = 0;
    gchar* conv = g_convert(bytes.data(), (gsize)bytes.size(),
                            "UTF-8", "WINDOWS-1251", NULL, &written, &err);
    std::string out = conv ? std::string(conv, written) : std::string();
    g_free(conv);
    if (err) g_error_free(err);
    return out;
}

// Reads a file as UTF-8 (handles UTF-8 / UTF-16LE / UTF-16BE BOMs, and
// Windows-1251 (Cyrillic ANSI) documents without a BOM).
static std::string ReadFileUtf8(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string bytes;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) bytes.append(buf, n);
    fclose(f);

    if (bytes.size() >= 2) {
        unsigned char b0 = (unsigned char)bytes[0], b1 = (unsigned char)bytes[1];
        if (b0 == 0xFF && b1 == 0xFE) {  // UTF-16 LE
            GError* err = NULL;
            gsize written = 0;
            gchar* conv = g_convert(bytes.data() + 2, (gsize)(bytes.size() - 2),
                                    "UTF-8", "UTF-16LE", NULL, &written, &err);
            std::string out = conv ? std::string(conv, written) : std::string();
            g_free(conv);
            if (err) g_error_free(err);
            return out;
        }
        if (b0 == 0xFE && b1 == 0xFF) {  // UTF-16 BE
            GError* err = NULL;
            gsize written = 0;
            gchar* conv = g_convert(bytes.data() + 2, (gsize)(bytes.size() - 2),
                                    "UTF-8", "UTF-16BE", NULL, &written, &err);
            std::string out = conv ? std::string(conv, written) : std::string();
            g_free(conv);
            if (err) g_error_free(err);
            return out;
        }
    }
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);
    else if (!bytes.empty() && LooksLikeWindows1251(bytes))
        return Windows1251ToUtf8(bytes);
    return bytes;  // UTF-8
}

static bool WriteFileUtf8(const std::string& path, const std::string& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t w = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return w == data.size();
}

// ---------------------------------------------------------------------------
// Markdown -> HTML (md4c) — same call as the Windows version

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

// Base64 encoder for data: URI embedding of local images.
static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string Base64Encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) |
                     (unsigned char)in[i + 2];
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];  out += kB64[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63];
        out += "=";
    }
    return out;
}

static const char* MimeTypeFor(const std::string& path) {
    size_t dot = path.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? std::string() : path.substr(dot + 1);
    for (char& c : ext) c = (char)g_ascii_tolower(c);
    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")  return "image/gif";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "webp") return "image/webp";
    if (ext == "bmp")  return "image/bmp";
    if (ext == "ico")  return "image/x-icon";
    return "application/octet-stream";
}

// Replaces relative image src="..." with data: URIs (Web pages hosted via
// load_html() have an about: origin, and WebKit may refuse file:// subresources
// from it). The original relative path is kept in data-mp-src so the editor JS
// can serialize the image back to its Markdown path on export.
static void EmbedLocalImages(std::string& html, const std::string& dir) {
    if (dir.empty()) return;

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
        if (absolute) { pos = end + 1; continue; }

        std::string rel = src;
        for (char& c : rel) if (c == '\\') c = '/';
        std::string path = dir + rel;  // dir ends with '/'
        std::string data;
        if (FileExists(path)) {
            FILE* f = fopen(path.c_str(), "rb");
            if (f) {
                char buf[65536]; size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
                fclose(f);
            }
        }
        if (data.empty()) { pos = end + 1; continue; }  // keep as-is (broken img)

        std::string uri = std::string("data:") + MimeTypeFor(path) +
                          ";base64," + Base64Encode(data);
        // Replace the src value and inject data-mp-src with the original path.
        html.replace(start, end - start, uri);
        size_t imgTag = html.rfind("<img", start);
        size_t insertAt = (imgTag == std::string::npos) ? start : start + uri.size() + 1;
        std::string attr = " data-mp-src=\"" + rel + "\"";
        html.insert(insertAt, attr);
        pos = insertAt + attr.size() + 1;
    }
}

// Overrides the shared __mpImgText so that locally embedded images
// (data: URI + data-mp-src attribute) serialize back to their original
// relative Markdown path instead of a giant data: URI.
static const char* kLinuxImgOverrideJs = R"JS(
(function(){
  var orig = __mpImgText;
  __mpImgText = function(el) {
    var msrc = el.getAttribute ? el.getAttribute("data-mp-src") : null;
    if (!msrc) return orig(el);
    var alt = el.getAttribute("alt") || "";
    var title = el.getAttribute("title") || "";
    var p = msrc.split("(").join("%28").split(")").join("%29");
    var out = "![" + alt + "](" + p;
    if (title !== "") out += " \"" + title.split("\"").join("\\\"") + "\"";
    out += ")";
    return out;
  };
})();
)JS";

static std::string BuildHtml(const std::string& bodyHtml, const std::string& title,
                             const std::string& baseDir) {
    std::string html;
    html.reserve(bodyHtml.size() + 8192);
    html += "<!DOCTYPE html>\n<html class=\"platform-linux\">\n<head>\n<meta charset=\"utf-8\">\n";
    html += "<title>" + EscapeHtml(title) + "</title>\n<style>\n";
    html += kCss;
    html += "\n</style>\n<script>\n";
    html += kEditorJs;
    // Linux-side additions to the shared editor JS.
    html += "\nfunction __mpIsDirty(){ return __mpDirty ? \"1\" : \"0\"; }\n";
    html += "function __mpExportChecked(){"
            " var r; try { r = __mpExport(); } catch(e) { return \"\\u0001ERR\\u0001\" + e; }"
            " var err = document.body.getAttribute(\"data-markpeek-error\");"
            " if (err) return \"\\u0001ERR\\u0001\" + err;"
            " return (r === null || r === undefined) ? \"\" : r;"
            "}\n";
    html += kLinuxImgOverrideJs;
    html += "\nvar __mpBase = \"";
    if (!baseDir.empty()) html += EscapeHtml("file://" + baseDir);
    html += "\";\n</script>\n</head>\n<body>\n<div id=\"markpeek-content\">\n";
    html += bodyHtml;
    html += "\n</div>\n</body>\n</html>\n";
    return html;
}

// ---------------------------------------------------------------------------
// JS bridge (WebKit2 async run_javascript; works on 2.4x Soup2/Soup3 alike)

struct JsCtx {
    std::string task;        // "save" | "check-dirty" | "set-mode"
    std::string sourcePath;  // used by "save"
};

static void RunJs(const std::string& code, const char* task, const std::string& sourcePath);

static void OnJsFinished(WebKitWebView*, GAsyncResult* res, gpointer userdata) {
    JsCtx* ctx = (JsCtx*)userdata;
    GError* err = NULL;
    WebKitJavascriptResult* jr = webkit_web_view_run_javascript_finish(
        WEBKIT_WEB_VIEW(g_webview), res, &err);

    if (err || !jr) {
        if (ctx->task != "check-dirty")
            SetStatus(std::string("JS error: ") + (err && err->message ? err->message : "no result"));
        if (err) g_error_free(err);
        delete ctx;
        return;
    }

    JSCValue* value = webkit_javascript_result_get_js_value(jr);
    char* s = jsc_value_to_string(value);
    std::string result = s ? s : "";
    g_free(s);
    webkit_javascript_result_unref(jr);

    // Copy what we need from the context BEFORE freeing it.
    std::string task = ctx->task;
    std::string srcPath = ctx->sourcePath;
    delete ctx;

    if (result == "undefined" || result == "null") result.clear();

    if (task == "smoke-probe") {
        if (result == "true") {
            std::string js = ReadFileUtf8(g_smokeJsFile);
            if (!js.empty()) {
                RunJs(js, "smoke-run", "");
            } else {
                g_warning("smoke test: cannot read MP_SMOKE_JS file");
                gtk_main_quit();
            }
        }
        return;
    }
    if (task == "smoke-run") {
        const char* out = g_getenv("MP_SMOKE_OUT");
        if (out && !result.empty()) {
            GError* e = NULL;
            g_file_set_contents(out, result.c_str(), (gssize)result.size(), &e);
            if (e) g_error_free(e);
        }
        gtk_main_quit();
        return;
    }

    if (task == "check-dirty" || task == "set-mode") {
        if (task == "check-dirty") {
            bool dirty = (result == "1");
            if (dirty != g_dirty) {
                g_dirty = dirty;
                UpdateTitle();
            }
        }
        return;
    }

    if (task == "save") {
        if (result.rfind("\001ERR\001", 0) == 0) {
            SetStatus("Could not convert the edited document back to Markdown. (" +
                      result.substr(5) + ")");
            return;
        }
        if (srcPath.empty() || !WriteFileUtf8(srcPath, result)) {
            SetStatus("Could not write file: " + srcPath);
            return;
        }
        g_dirty = false;
        UpdateTitle();
        SetStatus("Saved " + srcPath);
    }
}

static void RunJs(const std::string& code, const char* task, const std::string& sourcePath) {
    if (!g_webview) return;
    JsCtx* ctx = new JsCtx();
    ctx->task = task;
    ctx->sourcePath = sourcePath;
    webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(g_webview), code.c_str(), NULL,
                                   (GAsyncReadyCallback)OnJsFinished, ctx);
}

// Readiness probe: waits until the page has non-trivial content, then runs the
// smoke-test script (see MP_SMOKE_JS in main()).
static gboolean SmokeStart(gpointer) {
    static int tries = 0;
    if (++tries > 60) {  // ~30 s without a rendered page
        g_warning("smoke test: page never became ready (uri=%s, title=%s)",
                webkit_web_view_get_uri(WEBKIT_WEB_VIEW(g_webview)) ? webkit_web_view_get_uri(WEBKIT_WEB_VIEW(g_webview)) : "(null)",
                webkit_web_view_get_title(WEBKIT_WEB_VIEW(g_webview)) ? webkit_web_view_get_title(WEBKIT_WEB_VIEW(g_webview)) : "(null)");
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }
    std::string probe =
        "(document.readyState === 'complete' || document.readyState === 'interactive') && "
        "document.getElementById('markpeek-content') && "
        "document.getElementById('markpeek-content').children.length > 0";
    JsCtx* ctx = new JsCtx();
    ctx->task = "smoke-probe";
    ctx->sourcePath = "";
    webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(g_webview), probe.c_str(), NULL,
                                   (GAsyncReadyCallback)OnJsFinished, ctx);
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// Actions

static void OpenFile(const std::string& path);
static void EnterEditMode();
static void LeaveEditMode();
static void StartSmoke(GtkApplication* app);
static void OnWebViewLoadFailed(WebKitWebView*, WebKitLoadEvent, const gchar*, const gchar*, gpointer);

static void ActionSave(GtkWidget*, gpointer) {
    if (g_currentPath.empty()) {
        SetStatus("Nothing to save - open a .md file first");
        return;
    }
    if (!g_editing) EnterEditMode();  // mode JS is queued before the export JS
    RunJs("__mpExportChecked();", "save", g_currentPath);
}

static void ActionEditToggle(GtkWidget*, gpointer) {
    if (g_editing) {
        LeaveEditMode();
    } else {
        if (g_currentPath.empty()) {
            SetStatus("Open a .md file first (Ctrl+O)");
            return;
        }
        EnterEditMode();
    }
}

static void ActionOpen(GtkWidget*, gpointer) {
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Open Markdown file", GTK_WINDOW(g_window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Markdown files (*.md; *.markdown; *.txt)");
    gtk_file_filter_add_pattern(filter, "*.md");
    gtk_file_filter_add_pattern(filter, "*.markdown");
    gtk_file_filter_add_pattern(filter, "*.txt");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    GtkFileFilter* all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files (*)");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (file) {
            if (g_editing) LeaveEditMode();
            OpenFile(file);
            g_free(file);
        }
    }
    gtk_widget_destroy(dialog);
}

static void ActionReload(GtkWidget*, gpointer) {
    if (!g_currentPath.empty()) {
        if (g_editing) LeaveEditMode();
        OpenFile(g_currentPath);
    }
}

static void ActionAbout(GtkWidget*, gpointer) {
    GtkWidget* dialog = gtk_message_dialog_new(
        GTK_WINDOW(g_window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "%s %s\n\nA minimal Typora-style Markdown viewer & editor for Linux.\n\n"
        "Rendered with md4c + WebKitGTK.\nMIT License.",
        APP_NAME, APP_VERSION);
    gtk_window_set_title(GTK_WINDOW(dialog), "About MarkPeek");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void EnterEditMode() {
    if (g_editing) return;
    if (g_currentPath.empty()) {
        SetStatus("Open a .md file first (Ctrl+O)");
        return;
    }
    RunJs("__mpSetMode(true);", "set-mode", "");
    g_editing = true;
    g_dirty = false;
    UpdateTitle();
    UpdateEditButton();
    SetStatus("Editing - Ctrl+S to save, Ctrl+E to preview (WYSIWYG)");
}

static void LeaveEditMode() {
    if (!g_editing) return;
    RunJs("__mpSetMode(false);", "set-mode", "");
    g_editing = false;
    UpdateTitle();
    UpdateEditButton();
    SetStatus("Preview - Ctrl+E to edit, Ctrl+S to save");
}

// Returns the base URI for load_html(). Deliberately an about: URI for all
// pages: opaque-origin pages spawn the web process reliably on every WebKitGTK
// build we tested (2.36..2.52), while a file:// base can silently fail to load
// (observed on WebKitGTK 2.52/WSL1: no load events at all). Relative image
// paths are inlined as data: URIs before this point, and the document
// directory is passed to the page JS via __mpBase, so nothing needs file://.
static std::string BaseUriFor(const std::string& dir) {
    (void)dir;
    return "about:blank";
}

static void OpenFile(const std::string& path) {
    if (g_editing) LeaveEditMode();
    if (!FileExists(path)) {
        SetStatus("File not found: " + path);
        return;
    }
    std::string md = ReadFileUtf8(path);
    std::string body;
    if (!MdToHtml(md, body)) body = "<p><em>(render error)</em></p>";

    std::string dir = DirOf(path);
    for (char& c : dir) if (c == '\\') c = '/';
    EmbedLocalImages(body, dir);

    std::string html = BuildHtml(body, BaseName(path), dir);
    webkit_web_view_load_html(WEBKIT_WEB_VIEW(g_webview), html.c_str(),
                              BaseUriFor(dir).c_str());
    g_currentPath = path;
    g_dirty = false;
    UpdateTitle();

    int lines = 0;
    for (char c : md) if (c == '\n') lines++;
    SetStatus(path + "   |   " + std::to_string(lines) + " lines");
}

static void ShowWelcome() {
    const char* welcome =
        "# Welcome to MarkPeek\n\n"
        "A minimal, Typora-style Markdown viewer & editor for Linux.\n\n"
        "## Get started\n\n"
        "- Press **Ctrl+O** or drag & drop a `.md` file into this window\n"
        "- Press **Ctrl+E** to edit right in the rendered view (WYSIWYG)\n"
        "- While editing, **Ctrl+S** saves; **Ctrl+A**, **Ctrl+C/V/X/Z** and other\n"
        "  shortcuts work in any keyboard layout\n"
        "- Or from the terminal: `markpeek readme.md`\n\n"
        "## Features\n\n"
        "- Clean, Typora-like design\n"
        "- CommonMark + GitHub tables, task lists, strikethrough\n"
        "- UTF-8 / UTF-16 files\n"
        "- A single binary, no installation\n\n"
        "> Tip: relative images next to the opened `.md` file are resolved automatically.";
    std::string body;
    MdToHtml(welcome, body);
    std::string html = BuildHtml(body, "Welcome", "");
    webkit_web_view_load_html(WEBKIT_WEB_VIEW(g_webview), html.c_str(), "about:markpeek");
    g_currentPath.clear();
    g_dirty = false;
    UpdateTitle();
    SetStatus("Ready - open a .md file (Ctrl+O) or drop it here");
}

// ---------------------------------------------------------------------------
// Drag & drop of .md files onto the window

static void OnDragDataReceived(GtkWidget*, GdkDragContext*, gint, gint,
                               GtkSelectionData* data, guint, gpointer) {
    gchar** uris = gtk_selection_data_get_uris(data);
    if (!uris) return;
    for (gchar** u = uris; *u; ++u) {
        char* file = g_filename_from_uri(*u, NULL, NULL);
        if (!file) continue;
        std::string p = file;
        g_free(file);
        if (HasMdExt(p)) {
            if (g_editing) LeaveEditMode();
            OpenFile(p);
            break;
        }
    }
    g_strfreev(uris);
}

// ---------------------------------------------------------------------------
// Navigation policy: the page itself (about:) is internal; everything else
// (links, file links) opens in the user's default browser/app.

static void LaunchExternal(const std::string& uri) {
    GError* err = NULL;
    if (!g_app_info_launch_default_for_uri(uri.c_str(), NULL, &err) && err)
        g_error_free(err);
}

static gboolean OnDecidePolicy(WebKitWebView*, WebKitPolicyDecision* decision,
                               WebKitPolicyDecisionType type, gpointer) {
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return FALSE;

    WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(
        WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    WebKitURIRequest* req = webkit_navigation_action_get_request(action);
    std::string uri = req ? webkit_uri_request_get_uri(req) : "";

    if (webkit_navigation_action_get_mouse_button(action) == 2 ||
        (webkit_navigation_action_get_modifiers(action) & GDK_CONTROL_MASK)) {
        LaunchExternal(uri);
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }
    if (uri.rfind("about:", 0) == 0 || uri.rfind("data:", 0) == 0) {
        webkit_policy_decision_use(decision);
        return TRUE;
    }
    if (!uri.empty()) {
        LaunchExternal(uri);
    }
    webkit_policy_decision_ignore(decision);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Dirty-state poll (same model as the Windows WM_TIMER dirty timer)

static gboolean DirtyPoll(gpointer) {
    if (g_editing && g_webview)
        RunJs("__mpIsDirty();", "check-dirty", "");
    return G_SOURCE_CONTINUE;
}

// Smoke mode driver: MP_SMOKE_FILE (or the argv file) is already loaded by the
// caller; wait for render, run the check JS, write the result, quit the app.
static void StartSmoke(GtkApplication* app) {
    if (g_currentPath.empty()) {
        const char* f = g_getenv("MP_SMOKE_FILE");
        if (f && FileExists(f)) OpenFile(f);
    }
    g_timeout_add(500, SmokeStart, NULL);
    gtk_main();  // waits until the smoke script finishes, then quits
    g_application_quit(G_APPLICATION(app));
}

// ---------------------------------------------------------------------------
// Application

static void Activate(GtkApplication* app, gpointer) {
    if (g_window) {  // second activate (e.g. remote instance) — just present
        gtk_window_present(GTK_WINDOW(g_window));
        return;
    }
    g_window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(g_window), 960, 720);

    // GtkHeaderBar close button renders a themed icon; on platforms where
    // gdk-pixbuf cannot decode icons (WSL1, minimal containers) GTK aborts.
    // Probe once and fall back to the plain title bar when icons are broken.
    GtkWidget* header = NULL;
    if (IconThemeHas("window-close-symbolic")) {
        header = gtk_header_bar_new();
        gtk_header_bar_set_title(GTK_HEADER_BAR(header), APP_NAME);
        gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    }

    if (header) {
        g_btn_edit = gtk_button_new();
        UpdateEditButton();
        gtk_widget_set_tooltip_text(g_btn_edit, "Edit / Preview (Ctrl+E)");
        g_signal_connect(g_btn_edit, "clicked", G_CALLBACK(ActionEditToggle), NULL);
        gtk_header_bar_pack_start(GTK_HEADER_BAR(header), g_btn_edit);

        GtkWidget* btn_open = MakeIconButton("document-open-symbolic", "_Open");
        gtk_widget_set_tooltip_text(btn_open, "Open (Ctrl+O)");
        g_signal_connect(btn_open, "clicked", G_CALLBACK(ActionOpen), NULL);
        gtk_header_bar_pack_start(GTK_HEADER_BAR(header), btn_open);

        GtkWidget* btn_save = MakeIconButton("document-save-symbolic", "_Save");
        gtk_widget_set_tooltip_text(btn_save, "Save (Ctrl+S)");
        g_signal_connect(btn_save, "clicked", G_CALLBACK(ActionSave), NULL);
        gtk_header_bar_pack_end(GTK_HEADER_BAR(header), btn_save);

        GtkWidget* btn_about = MakeIconButton("help-about-symbolic", "_About");
        gtk_widget_set_tooltip_text(btn_about, "About");
        g_signal_connect(btn_about, "clicked", G_CALLBACK(ActionAbout), NULL);
        gtk_header_bar_pack_end(GTK_HEADER_BAR(header), btn_about);

        gtk_window_set_titlebar(GTK_WINDOW(g_window), header);
    } else {
        // Broken-icon environment: plain title bar, app menu via keyboard only.
        gtk_window_set_title(GTK_WINDOW(g_window), APP_NAME);
        SetStatus("Icons unavailable in this environment; use Ctrl+O / Ctrl+E / Ctrl+S");
    }

    g_webview = webkit_web_view_new();
    WebKitSettings* settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(g_webview));
    webkit_settings_set_javascript_can_access_clipboard(settings, TRUE);
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
    webkit_settings_set_enable_tabs_to_links(settings, FALSE);
    g_signal_connect(g_webview, "decide-policy", G_CALLBACK(OnDecidePolicy), NULL);
    g_signal_connect(g_webview, "load-failed", G_CALLBACK(OnWebViewLoadFailed), NULL);

    g_status = gtk_statusbar_new();

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), g_webview, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), g_status, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(g_window), vbox);

    gtk_drag_dest_set(g_window, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
    GtkTargetEntry targets[] = { { (gchar*)"text/uri-list", 0, 0 } };
    gtk_drag_dest_set_target_list(g_window, gtk_target_list_new(targets, 1));
    g_signal_connect(g_window, "drag-data-received", G_CALLBACK(OnDragDataReceived), NULL);

    // Accels are keyboard-layout independent (same as the Windows accelerator
    // table): Ctrl+O / Ctrl+E / Ctrl+S / F5.
    GtkAccelGroup* accel = gtk_accel_group_new();
    gtk_accel_group_connect(accel, GDK_KEY_o, GDK_CONTROL_MASK, (GtkAccelFlags)0,
                            g_cclosure_new(G_CALLBACK(ActionOpen), NULL, NULL));
    gtk_accel_group_connect(accel, GDK_KEY_e, GDK_CONTROL_MASK, (GtkAccelFlags)0,
                            g_cclosure_new(G_CALLBACK(ActionEditToggle), NULL, NULL));
    gtk_accel_group_connect(accel, GDK_KEY_s, GDK_CONTROL_MASK, (GtkAccelFlags)0,
                            g_cclosure_new(G_CALLBACK(ActionSave), NULL, NULL));
    gtk_accel_group_connect(accel, GDK_KEY_F5, (GdkModifierType)0, (GtkAccelFlags)0,
                            g_cclosure_new(G_CALLBACK(ActionReload), NULL, NULL));
    gtk_window_add_accel_group(GTK_WINDOW(g_window), accel);

    gtk_widget_show_all(g_window);
    ShowWelcome();

    g_timeout_add(500, DirtyPoll, NULL);
}

// "open" arrives when files are passed on the command line (G_APPLICATION_
// HANDLES_OPEN); it is emitted instead of "activate", so create the UI first.
static gboolean OpenPendingIdle(gpointer) {
    for (const std::string& path : g_pendingOpenFiles) {
        if (g_editing) LeaveEditMode();
        OpenFile(path);
    }
    g_pendingOpenFiles.clear();
    return G_SOURCE_REMOVE;
}

static void OnAppOpen(GtkApplication* app, GFile** files, gint n, const gchar*, gpointer) {
    Activate(app, NULL);
    for (int i = 0; i < n; i++) {
        char* p = g_file_get_path(files[i]);
        if (!p) continue;
        g_pendingOpenFiles.push_back(p);
        g_free(p);
    }
    if (!g_pendingOpenFiles.empty())
        g_idle_add(OpenPendingIdle, NULL);
    if (!g_smokeJsFile.empty()) StartSmoke(app);
}

// WebKit process tuning for the AppImage: the bundled WebKitGTK may pick a
// GPU/DMABUF renderer or bubblewrap sandbox that fails on some real systems
// (the view then stays blank). CI-proven defaults are applied ONLY when
// running as an AppImage (APPIMAGE env var is set) and only when the user has
// not overridden them. Regular (non-AppImage) runs are untouched.
static void ApplyAppImageWebKitEnv() {
    if (!g_getenv("APPIMAGE")) return;
    struct { const char* name; const char* value; } envs[] = {
        { "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1" },
        { "WEBKIT_DISABLE_DMABUF_RENDERER", "1" },
        { "LIBGL_ALWAYS_SOFTWARE", "1" },
    };
    for (auto& e : envs) {
        if (!g_getenv(e.name)) g_setenv(e.name, e.value, TRUE);
    }
}

static void OnWebViewLoadFailed(WebKitWebView*, WebKitLoadEvent, const gchar* failing,
                                const gchar* error, gpointer) {
    std::string msg = std::string("Page load failed: ") + (failing ? failing : "") +
                      " - " + (error ? error : "unknown");
    g_warning("markpeek: %s", msg.c_str());
    SetStatus(msg);
}

int main(int argc, char** argv) {
    ApplyAppImageWebKitEnv();
    const char* smokeJs = g_getenv("MP_SMOKE_JS");
    const char* smokeOut = g_getenv("MP_SMOKE_OUT");
    if (smokeJs && smokeOut) {
        g_smokeJsFile = smokeJs;
        g_smokeOutFile = smokeOut;
    }

    GtkApplication* app = gtk_application_new("io.github.larin_ilya.MarkPeek",
                                              (GApplicationFlags)(G_APPLICATION_NON_UNIQUE |
                                              G_APPLICATION_HANDLES_OPEN));
    g_signal_connect(app, "activate", G_CALLBACK(Activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(OnAppOpen), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
