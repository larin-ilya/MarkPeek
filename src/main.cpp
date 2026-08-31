// MarkPeek — a minimal Typora-style Markdown viewer for Windows.
// Win32 + embedded IE (MSHTML) + md4c renderer. MIT License.
// Works on Windows 7 SP1 and Windows 10. Portable, 32-bit, no dependencies.

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <ole2.h>
#include <exdisp.h>
#include <oleidl.h>
#include <ocidl.h>
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
#define APP_VERSION L"1.1.0"

static HWND g_hwnd = NULL;
static HWND g_hwndBrowser = NULL;
static HWND g_hwndStatus = NULL;
static IWebBrowser2* g_wb = NULL;
static IOleInPlaceActiveObject* g_pioipa = NULL;
static std::wstring g_currentPath;
static std::wstring g_tempHtmlPath;
static HWND g_hwndEdit = NULL;
static HFONT g_hEditFont = NULL;
static bool g_editing = false;
static bool g_dirty = false;

enum { IDM_OPEN = 1001, IDM_RELOAD, IDM_ASSOC, IDM_UNASSOC, IDM_EXIT, IDM_ABOUT,
       IDM_EDIT, IDM_SAVE };
enum { IDC_STATUS = 2001, IDC_EDIT = 2002 };

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
)CSS";

static std::string BuildHtml(const std::string& bodyHtml, const std::wstring& titleW) {
    std::string title = EscapeHtml(WideToUtf8(titleW.c_str()));
    std::string html;
    html.reserve(bodyHtml.size() + 4096);
    html += "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n";
    html += "<meta http-equiv=\"X-UA-Compatible\" content=\"IE=edge\">\n";
    html += "<title>" + title + "</title>\n<style>\n";
    html += kCss;
    html += "\n</style>\n</head>\n<body>\n<div id=\"markpeek-content\">\n";
    html += bodyHtml;
    html += "\n</div>\n</body>\n</html>\n";
    return html;
}

// ---------------------------------------------------------------------------
// WebBrowser control

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

static void SetStatus(const std::wstring& text) {
    if (g_hwndStatus)
        SendMessageW(g_hwndStatus, SB_SETTEXT, 0, (LPARAM)text.c_str());
}

static bool PromptSaveIfDirty();
static void LeaveEditMode(bool save, bool rerender);

static void OpenFile(const std::wstring& path) {
    if (g_editing) {
        if (!PromptSaveIfDirty()) return;
        LeaveEditMode(false, false);
    }
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
    std::string html = BuildHtml(body, name);
    ShowHtml(html);
    g_currentPath = path;

    SetWindowTextW(g_hwnd, (name + L" - " + APP_NAME).c_str());

    int lines = 0;
    for (char c : md) if (c == '\n') lines++;
    wchar_t st[2600];
    swprintf(st, 2600, L"%s   |   %d lines", path.c_str(), lines);
    SetStatus(st);
}

static void ShowWelcome() {
    const wchar_t* welcome =
        L"# Welcome to MarkPeek\n\n"
        L"A minimal, Typora-style Markdown viewer for Windows.\n\n"
        L"## Get started\n\n"
        L"- Press **Ctrl+O** or drag & drop a `.md` file into this window\n"
        L"- Press **F5** to reload, **Ctrl+E** to edit, **Ctrl+S** to save\n"
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
    std::string html = BuildHtml(body, L"Welcome");
    ShowHtml(html);
    SetWindowTextW(g_hwnd, (std::wstring(APP_NAME) + L" - drop or open a Markdown file").c_str());
    SetStatus(L"Ready - open a .md file (Ctrl+O) or drop it here");
}

// ---------------------------------------------------------------------------
// Edit mode (built-in editor)

static void UpdateTitle() {
    if (g_currentPath.empty()) return;
    std::wstring t = BaseName(g_currentPath);
    if (g_dirty) t += L" *";
    t += L" - " APP_NAME;
    SetWindowTextW(g_hwnd, t.c_str());
}

static bool SaveCurrentFile() {
    if (!g_hwndEdit || g_currentPath.empty()) return false;
    int len = GetWindowTextLengthW(g_hwndEdit);
    std::wstring text(len, L'\0');
    GetWindowTextW(g_hwndEdit, &text[0], len + 1);
    std::string utf8 = WideToUtf8(text.c_str(), len);
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
    if (g_editing || !g_hwndEdit) return;
    if (g_currentPath.empty()) {
        SendMessageW(g_hwnd, WM_COMMAND, IDM_OPEN, 0);
        if (g_currentPath.empty()) return;
    }
    std::string md = ReadFileUtf8(g_currentPath);
    SetWindowTextW(g_hwndEdit, Utf8ToWide(md).c_str());
    if (g_hwndBrowser) ShowWindow(g_hwndBrowser, SW_HIDE);
    if (g_wb) g_wb->put_Visible(FALSE);
    ShowWindow(g_hwndEdit, SW_SHOW);
    SetFocus(g_hwndEdit);
    SendMessageW(g_hwndEdit, EM_SETSEL, 0, 0);
    g_editing = true;
    g_dirty = false;
    UpdateTitle();
    SetStatus(L"Editing - Ctrl+S to save, Ctrl+E to preview");
}

static void LeaveEditMode(bool save, bool rerender) {
    if (!g_editing) return;
    if (save && g_dirty) SaveCurrentFile();
    ShowWindow(g_hwndEdit, SW_HIDE);
    if (g_hwndBrowser) ShowWindow(g_hwndBrowser, SW_SHOW);
    if (g_wb) g_wb->put_Visible(TRUE);
    g_editing = false;
    UpdateTitle();
    if (rerender && !g_currentPath.empty())
        OpenFile(g_currentPath);
    else if (g_hwndBrowser)
        SetFocus(g_hwndBrowser);
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
// Window

// ---------------------------------------------------------------------------
// WebBrowser control host (IOleClientSite / IOleInPlaceSite / IOleInPlaceFrame
// / IOleControlSite / IDocHostUIHandler) — classic EXEBrowser pattern.

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
        g_hwndEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
            WS_VSCROLL | WS_HSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hwnd, (HMENU)IDC_EDIT, GetModuleHandleW(NULL), NULL);
        g_hEditFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        if (g_hwndEdit && g_hEditFont)
            SendMessageW(g_hwndEdit, WM_SETFONT, (WPARAM)g_hEditFont, TRUE);
        return 0;

    case WM_SIZE:
        if (g_hwndBrowser) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (g_hwndStatus) {
                // Standard MSDN pattern: the status bar must receive WM_SIZE
                // itself to resize to the parent's full width and re-stick to
                // the bottom. Without this it keeps its creation-time
                // position, which ends up in the middle of the screen over
                // the rendered text when the window is maximized/fullscreen.
                SendMessageW(g_hwndStatus, WM_SIZE, 0, 0);
                RECT sr;
                GetWindowRect(g_hwndStatus, &sr);
                rc.bottom -= (sr.bottom - sr.top);
            }
            SetWindowPos(g_hwndBrowser, NULL, 0, 0, rc.right, rc.bottom, SWP_NOZORDER);
            if (g_hwndEdit)
                SetWindowPos(g_hwndEdit, NULL, 0, 0, rc.right, rc.bottom, SWP_NOZORDER);
        }
        return 0;

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        wchar_t buf[MAX_PATH];
        if (DragQueryFileW(hDrop, 0, buf, MAX_PATH) > 0)
            OpenFile(buf);
        DragFinish(hDrop);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_EDIT && HIWORD(wParam) == EN_UPDATE) {
            if (g_editing && !g_dirty) {
                g_dirty = true;
                UpdateTitle();
            }
            return 0;
        }
        switch (LOWORD(wParam)) {
        case IDM_OPEN: {
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
            if (GetOpenFileNameW(&ofn))
                OpenFile(file);
            return 0;
        }
        case IDM_RELOAD:
            if (!g_currentPath.empty())
                OpenFile(g_currentPath);
            return 0;
        case IDM_EDIT:
            if (g_editing) {
                if (PromptSaveIfDirty())
                    LeaveEditMode(false, true);
            } else {
                EnterEditMode();
            }
            return 0;
        case IDM_SAVE:
            if (g_editing)
                SaveCurrentFile();
            else
                EnterEditMode();
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
                L"A minimal Typora-style Markdown viewer.\n\n"
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
        if (g_hEditFont) {
            DeleteObject(g_hEditFont);
            g_hEditFont = NULL;
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