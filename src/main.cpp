// MdViewer - compact Markdown viewer: Win32 + WebView2 + md4c
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <wininet.h>
#include <dwmapi.h>
#include <wrl.h>
#include <cwctype>
#include <cstdio>
#include <string>
#include <fstream>
#include <sstream>
#include "WebView2.h"
#include "md4c-html.h"

using namespace Microsoft::WRL;

static const wchar_t* kClassName = L"MdViewerWnd";
static const wchar_t* kTitle     = L"MdViewer";
static const int      ID_OPEN    = 1001;
static const int      ID_RELOAD  = 1002;

struct App {
    HWND                            hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2>           webview;
    ComPtr<ICoreWebView2Environment9> env9;
    std::wstring                    currentFile;
    std::wstring                    previewUrl;
    bool                            ready = false;
    // Theme: 0 — system default, 1 — light, 2 — dark.
    // Stored in the ini ([window] theme=system|light|dark).
    int                             theme = 0;
};
static App g_app;
static bool AppDark();
static void ApplyTheme();

// ---------- string helpers ----------

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static bool IsValidUtf8(const std::string& s) {
    const unsigned char* p = (const unsigned char*)s.data();
    size_t n = s.size();
    for (size_t i = 0; i < n;) {
        unsigned char c = p[i];
        if (c < 0x80) { ++i; continue; }
        int len = 0;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else return false;
        if (i + (size_t)len > n) return false;
        for (int k = 1; k < len; ++k)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        i += (size_t)len;
    }
    return true;
}

static std::string ToUtf8(const std::string& raw) {
    size_t off = 0;
    if (raw.size() >= 3 &&
        (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
        off = 3;
    std::string s = raw.substr(off);
    if (IsValidUtf8(s)) return s;
    // Not UTF-8: assume the system ANSI code page (e.g. CP1251).
    int wlen = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    if (wlen <= 0) return s;
    std::wstring w((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), w.data(), wlen);
    return WideToUtf8(w);
}

static std::string EscapeHtml(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;";  break;
            case '>': r += "&gt;";  break;
            case '"': r += "&quot;"; break;
            default:  r += c;
        }
    }
    return r;
}

static std::wstring FileUrl(const std::wstring& path) {
    DWORD n = INTERNET_MAX_URL_LENGTH;
    std::wstring url(n, L'\0');
    if (SUCCEEDED(UrlCreateFromPathW(path.c_str(), url.data(), &n, 0))) {
        url.resize(wcslen(url.c_str()));
        return url;
    }
    return path;
}

static std::wstring ParentDir(const std::wstring& file) {
    size_t p = file.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? std::wstring() : file.substr(0, p + 1);
}

static std::wstring FileName(const std::wstring& file) {
    size_t p = file.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? file : file.substr(p + 1);
}

// ---------- page template ----------

static const char* kCss = R"CSS(
:root { color-scheme: light dark; }
html { -webkit-text-size-adjust: 100%; }
body {
  margin: 0; background: #ffffff; color: #24292f;
  font-family: "Segoe UI Variable Text", "Segoe UI", system-ui, -apple-system, sans-serif;
  font-size: 16px; line-height: 1.65;
  -webkit-font-smoothing: antialiased;
}
article { max-width: 760px; margin: 0 auto; padding: 44px 28px 120px; }
h1, h2, h3, h4, h5, h6 { margin: 1.6em 0 0.6em; line-height: 1.25; font-weight: 600; }
h1 { font-size: 2em; margin-top: 0; padding-bottom: .3em; border-bottom: 1px solid #d8dee4; }
h2 { font-size: 1.5em; padding-bottom: .3em; border-bottom: 1px solid #d8dee4; }
h3 { font-size: 1.25em; } h4 { font-size: 1em; } h5 { font-size: .875em; } h6 { font-size: .85em; color: #57606a; }
p, ul, ol, table, pre, blockquote { margin: 0 0 1em; }
ul, ol { padding-left: 1.6em; }
li + li { margin-top: .25em; }
a { color: #0969da; text-decoration: none; }
a:hover { text-decoration: underline; }
code, kbd, samp {
  font-family: "Cascadia Mono", "Consolas", "Courier New", monospace;
  font-size: .875em;
  background: rgba(175, 184, 193, .22);
  padding: .18em .4em; border-radius: 6px;
}
pre { background: #f6f8fa; padding: 16px; border-radius: 8px; overflow: auto; line-height: 1.45; }
pre code { background: none; padding: 0; font-size: .86em; }
blockquote { margin-left: 0; padding: 0 1em; color: #57606a; border-left: 4px solid #d0d7de; }
table { border-collapse: collapse; display: block; overflow: auto; max-width: 100%; }
th, td { border: 1px solid #d0d7de; padding: 6px 13px; }
th { font-weight: 600; background: #f6f8fa; }
tr:nth-child(2n) td { background: #f6f8fa; }
img { max-width: 100%; height: auto; }
hr { border: 0; border-top: 1px solid #d0d7de; margin: 28px 0; }
::selection { background: #b6d7ff; }
::-webkit-scrollbar { width: 11px; height: 11px; }
::-webkit-scrollbar-thumb { background: #c9d1d9; border-radius: 6px; border: 3px solid #ffffff; }
::-webkit-scrollbar-thumb:hover { background: #adbac7; }

@media (prefers-color-scheme: dark) {
  body { background: #0d1117; color: #e6edf3; }
  h1, h2 { border-bottom-color: #30363d; }
  h6 { color: #9198a1; }
  a { color: #4493f8; }
  code, kbd, samp { background: rgba(110, 118, 129, .40); }
  pre { background: #161b22; }
  blockquote { color: #9198a1; border-left-color: #3d444d; }
  th, td { border-color: #3d444d; }
  th { background: #161b22; }
  tr:nth-child(2n) td { background: #161b22; }
  hr { border-top-color: #30363d; }
  ::selection { background: #264f78; }
  ::-webkit-scrollbar-thumb { background: #3d444d; border-color: #0d1117; }
  ::-webkit-scrollbar-thumb:hover { background: #4d5863; }
}
@media print {
  article { max-width: none; padding: 0; }
  body { background: #fff; color: #000; }
}
)CSS";

static const char* kWelcome = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<style>
html,body{height:100%}
body{margin:0;display:flex;align-items:center;justify-content:center;
 background:#fff;color:#57606a;
 font-family:"Segoe UI Variable Text","Segoe UI",system-ui,sans-serif;}
.box{text-align:center;line-height:1.7}
h1{margin:0 0 .2em;font-size:1.4em;font-weight:600;color:#24292f}
kbd{font-family:Consolas,monospace;background:#f6f8fa;border:1px solid #d0d7de;
 border-bottom-width:2px;border-radius:6px;padding:.1em .4em;font-size:.9em;color:#24292f}
@media (prefers-color-scheme:dark){
 body{background:#0d1117;color:#9198a1}
 h1{color:#e6edf3}
 kbd{background:#161b22;border-color:#3d444d;color:#e6edf3}
}
</style></head><body><div class="box">
<h1>MdViewer</h1>
<div>Drag a <b>.md</b> file into the window &nbsp;·&nbsp; <kbd>Ctrl</kbd>+<kbd>O</kbd> to open</div>
</div></body></html>)HTML";

static std::string BuildHtml(const std::string& mdFragment,
                             const std::wstring& baseDirUrl,
                             const std::string& title) {
    std::string base = EscapeHtml(WideToUtf8(baseDirUrl));
    std::string t    = EscapeHtml(title);
    std::string h;
    h.reserve(mdFragment.size() + 4096);
    h += "<!doctype html><html><head><meta charset=\"utf-8\">";
    h += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
    h += "<base href=\"" + base + "\">";
    h += "<title>" + t + "</title>";
    h += "<style>";
    h += kCss;
    h += "</style></head><body><article class=\"markdown-body\">";
    h += mdFragment;
    h += "</article></body></html>";
    return h;
}

// ---------- markdown -> html ----------

static void HtmlOut(const MD_CHAR* text, MD_SIZE size, void* userdata) {
    static_cast<std::string*>(userdata)->append(text, size);
}

static bool ReadAll(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool WriteAll(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == data.size();
}

static std::wstring TempPreviewPath() {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"MdViewer";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\preview.html";
}

static void ShowWelcome() {
    std::wstring path = TempPreviewPath();
    if (!WriteAll(path, kWelcome)) return;
    g_app.previewUrl = FileUrl(path);
    if (g_app.webview) g_app.webview->Navigate(g_app.previewUrl.c_str());
    SetWindowTextW(g_app.hwnd, kTitle);
}

static bool LoadFile(const std::wstring& path) {
    std::string raw;
    if (!ReadAll(path, raw)) {
        MessageBoxW(g_app.hwnd, (L"Could not open the file:\n" + path).c_str(),
                    kTitle, MB_ICONERROR | MB_OK);
        return false;
    }

    std::string md = ToUtf8(raw);
    std::string fragment;
    md_html(md.data(), (MD_SIZE)md.size(), HtmlOut, &fragment,
            MD_DIALECT_GITHUB, MD_HTML_FLAG_SKIP_UTF8_BOM);

    std::wstring dir = ParentDir(path);
    if (dir.empty()) {
        wchar_t cwd[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        dir = std::wstring(cwd) + L"\\";
    }

    std::wstring title = FileName(path);
    std::string html = BuildHtml(fragment, FileUrl(dir), WideToUtf8(title));

    std::wstring out = TempPreviewPath();
    if (!WriteAll(out, html)) {
        MessageBoxW(g_app.hwnd, L"Could not write the preview temp file.",
                    kTitle, MB_ICONERROR | MB_OK);
        return false;
    }

    g_app.currentFile = path;
    g_app.previewUrl  = FileUrl(out);

    std::wstring caption = title + L" — " + kTitle;
    SetWindowTextW(g_app.hwnd, caption.c_str());

    if (g_app.webview) {
        // Cache-buster so a reload of the same temp path shows fresh content.
        wchar_t suffix[32];
        swprintf_s(suffix, L"?v=%llu", (unsigned long long)GetTickCount64());
        std::wstring url = g_app.previewUrl + suffix;
        g_app.webview->Navigate(url.c_str());
    }
    return true;
}

static void OpenDialog() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_app.hwnd;
    ofn.lpstrFilter = L"Markdown\0*.md;*.markdown;*.mdown;*.mkd;*.mdwn\0All files\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) LoadFile(file);
}

// ---------- WebView2 ----------

static std::wstring UserDataDir() {
    wchar_t* p = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) {
        base = p;
        CoTaskMemFree(p);
    }
    if (base.empty()) {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        base = tmp;
    }
    std::wstring dir = base + L"\\MdViewer\\WebView2";
    // Create the whole chain.
    std::wstring acc;
    for (size_t i = 0; i < dir.size(); ++i) {
        acc += dir[i];
        if (dir[i] == L'\\') CreateDirectoryW(acc.c_str(), nullptr);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static void ResizeWebView() {
    if (!g_app.controller) return;
    RECT rc;
    GetClientRect(g_app.hwnd, &rc);
    g_app.controller->put_Bounds(rc);
}

static bool IsPreviewUrl(const std::wstring& u) {
    return !g_app.previewUrl.empty() && u.rfind(g_app.previewUrl, 0) == 0;
}

static void HandleExternal(const std::wstring& url) {
    // Local .md link -> open inside the viewer; everything else -> default app/browser.
    std::wstring lower = url;
    for (auto& c : lower) c = (wchar_t)towlower(c);
    if (lower.rfind(L"file:", 0) == 0) {
        wchar_t path[MAX_PATH] = {};
        DWORD n = MAX_PATH;
        if (SUCCEEDED(PathCreateFromUrlW(url.c_str(), path, &n, 0))) {
            std::wstring p = path;
            std::wstring lp = p;
            for (auto& c : lp) c = (wchar_t)towlower(c);
            if (lp.size() > 3 &&
                (lp.rfind(L".md") == lp.size() - 3 || lp.rfind(L".markdown") == lp.size() - 9)) {
                LoadFile(p);
                return;
            }
        }
    }
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void SetupWebView() {
    ComPtr<ICoreWebView2_13> wv13;
    if (SUCCEEDED(g_app.webview.As(&wv13)) && wv13) {
        ComPtr<ICoreWebView2Profile> profile;
        if (SUCCEEDED(wv13->get_Profile(&profile)) && profile) {
            profile->put_PreferredColorScheme(AppDark()
                ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK
                : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT);
        }
    }

    ComPtr<ICoreWebView2Settings> s;
    if (SUCCEEDED(g_app.webview->get_Settings(&s)) && s) {
        s->put_AreDefaultContextMenusEnabled(TRUE);
        s->put_AreDevToolsEnabled(TRUE);
        s->put_IsStatusBarEnabled(FALSE);
        s->put_IsZoomControlEnabled(TRUE);
    }

    EventRegistrationToken tok = {};

    g_app.webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    HandleExternal(uri);
                    CoTaskMemFree(uri);
                }
                args->put_Handled(TRUE);
                return S_OK;
            }).Get(), &tok);

    g_app.webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR uri = nullptr;
                std::wstring u;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    u = uri;
                    CoTaskMemFree(uri);
                }
                if (u.empty() || IsPreviewUrl(u) || u == L"about:blank")
                    return S_OK;
                args->put_Cancel(TRUE);
                HandleExternal(u);
                return S_OK;
            }).Get(), &tok);

    // Custom context menu: WebView2 first builds the standard menu
    // (Back / Reload / Save as / Print / Inspect), and
    // ContextMenuRequested lets us fix it up:
    //   - remove "Inspect" (the built-in item) and put in its place,
    //     at the bottom of the menu, our "Open in Notepad" item with a separator above it
    //     (only when a file is open);
    //   - right after it, with no separator, the "Theme" submenu: Light /
    //     Dark / System (the current one is checkmarked; the choice
    //     applies immediately via ApplyTheme() and is written to the ini);
    //   - first, "Open" (the same as Ctrl+O: the open-file dialog),
    //     but only on plain page or selected text — never over a link
    //     or an image, where it would mislead (visible on the welcome
    //     screen too, where there is no file).
    // Needs ICoreWebView2_11 and ICoreWebView2Environment9 (in the SDK);
    // on an old runtime the QI simply fails and the menu stays stock.
    // To add another item: one more CreateContextMenuItem +
    // InsertValueAtIndex; the item's action goes in its CustomItemSelected.
    ComPtr<ICoreWebView2_11> wv11;
    if (SUCCEEDED(g_app.webview.As(&wv11)) && wv11 && g_app.env9) {
        wv11->add_ContextMenuRequested(
            Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
                    ComPtr<ICoreWebView2ContextMenuItemCollection> items;
                    if (FAILED(args->get_MenuItems(&items)) || !items) return S_OK;

                    // Remove "Inspect": built-in items carry the English
                    // label in lowerCamelCase as Name; for Inspect it is "inspect"
                    // (also cross-checked against the Ctrl+Shift+I shortcut).
                    // Remember its index — our item goes there.
                    UINT32 count = 0;
                    UINT32 at = 0;  // where to insert our item
                    bool found = false;
                    if (SUCCEEDED(items->get_Count(&count))) {
                        for (UINT32 i = 0; i < count; ++i) {
                            ComPtr<ICoreWebView2ContextMenuItem> cur;
                            if (FAILED(items->GetValueAtIndex(i, &cur)) || !cur) continue;
                            LPWSTR name = nullptr;
                            LPWSTR keys = nullptr;
                            bool isInspect = false;
                            if (SUCCEEDED(cur->get_Name(&name)) && name) {
                                if (_wcsicmp(name, L"inspect") == 0 ||
                                    _wcsicmp(name, L"inspectElement") == 0)
                                    isInspect = true;
                                CoTaskMemFree(name);
                            }
                            if (!isInspect && SUCCEEDED(cur->get_ShortcutKeyDescription(&keys)) && keys) {
                                if (_wcsicmp(keys, L"Ctrl+Shift+I") == 0)
                                    isInspect = true;
                                CoTaskMemFree(keys);
                            }
                            if (isInspect) {
                                if (SUCCEEDED(items->RemoveValueAtIndex(i))) {
                                    at = i;
                                    found = true;
                                }
                                break;
                            }
                        }
                    }
                    if (!found) {
                        // "Inspect" not found (a different menu) — our item
                        // just goes at the end.
                        if (FAILED(items->get_Count(&at))) return S_OK;
                    }

                    // At the bottom: "Open in Notepad" — only when a file is open.
                    // (Inserted first, while the indices have not yet been shifted
                    // by inserting "Open" at the top.)
                    UINT32 after = at;  // where the "Theme" submenu goes
                    if (!g_app.currentFile.empty()) {
                        ComPtr<ICoreWebView2ContextMenuItem> item;
                        if (SUCCEEDED(g_app.env9->CreateContextMenuItem(
                                L"Open in Notepad", nullptr,
                                COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &item)) && item) {
                            EventRegistrationToken tok2 = {};
                            item->add_CustomItemSelected(
                                Callback<ICoreWebView2CustomItemSelectedEventHandler>(
                                    [](ICoreWebView2ContextMenuItem*, IUnknown*) -> HRESULT {
                                        if (!g_app.currentFile.empty()) {
                                            std::wstring params = L"\"" + g_app.currentFile + L"\"";
                                            ShellExecuteW(nullptr, L"open", L"notepad.exe",
                                                        params.c_str(), nullptr, SW_SHOWNORMAL);
                                        }
                                        return S_OK;
                                    }).Get(), &tok2);

                            // The separator goes exactly above our item: first
                            // the separator at position at, then the item right after it.
                            ComPtr<ICoreWebView2ContextMenuItem> sep;
                            if (SUCCEEDED(g_app.env9->CreateContextMenuItem(
                                    L"", nullptr,
                                    COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR, &sep)) && sep) {
                                items->InsertValueAtIndex(at, sep.Get());
                                items->InsertValueAtIndex(at + 1, item.Get());
                                after = at + 2;  // right after "Open in Notepad"
                            } else {
                                items->InsertValueAtIndex(at, item.Get());
                                after = at + 1;
                            }
                        }
                    }

                    // "Theme" is a submenu right after "Open in Notepad",
                    // with no separator between them; on the welcome screen
                    // (no file) it takes the place of the removed "Inspect".
                    // The current theme is checkmarked (IsChecked).
                    ComPtr<ICoreWebView2ContextMenuItem> themeItem;
                    if (SUCCEEDED(g_app.env9->CreateContextMenuItem(
                            L"Theme", nullptr,
                            COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SUBMENU, &themeItem)) && themeItem) {
                        ComPtr<ICoreWebView2ContextMenuItemCollection> kids;
                        if (SUCCEEDED(themeItem->get_Children(&kids)) && kids) {
                            struct ThemeOpt { const wchar_t* label; int value; };
                            static const ThemeOpt opts[] = {
                                { L"Light", 1 },
                                { L"Dark", 2 },
                                { L"System", 0 },
                            };
                            // Note: the checkmark (IsChecked) only renders on
                            // CHECK_BOX / RADIO. On COMMAND, IsChecked is silently
                            // ignored — that is why there were no checkmarks without CHECK_BOX.
                            for (int i = 0; i < 3; ++i) {
                                ComPtr<ICoreWebView2ContextMenuItem> child;
                                if (FAILED(g_app.env9->CreateContextMenuItem(
                                        opts[i].label, nullptr,
                                        COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_CHECK_BOX, &child)) || !child)
                                    continue;
                                child->put_IsChecked(g_app.theme == opts[i].value ? TRUE : FALSE);
                                int val = opts[i].value;
                                EventRegistrationToken tokT = {};
                                child->add_CustomItemSelected(
                                    Callback<ICoreWebView2CustomItemSelectedEventHandler>(
                                        [val](ICoreWebView2ContextMenuItem*, IUnknown*) -> HRESULT {
                                            g_app.theme = val;
                                            ApplyTheme();
                                            return S_OK;
                                        }).Get(), &tokT);
                                kids->InsertValueAtIndex((UINT32)i, child.Get());
                            }
                        }
                        items->InsertValueAtIndex(after, themeItem.Get());
                    }

                    // At the top: "Open" — but only where it cannot be
                    // confused with opening the clicked thing: plain page
                    // or selected text, and never over a link or an image
                    // (the standard menu already offers actions for those,
                    // and "Open" means the open-file dialog). Always shown
                    // on the welcome screen (kind PAGE, no link).
                    // The same action as Ctrl+O (ID_OPEN → open dialog).
                    bool showOpen = true;
                    ComPtr<ICoreWebView2ContextMenuTarget> target;
                    if (SUCCEEDED(args->get_ContextMenuTarget(&target)) && target) {
                        COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND kind =
                            COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND_PAGE;
                        BOOL hasLink = FALSE;
                        if (SUCCEEDED(target->get_Kind(&kind))) {
                            if (kind != COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND_PAGE &&
                                kind != COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND_SELECTED_TEXT)
                                showOpen = false;
                        }
                        if (showOpen && SUCCEEDED(target->get_HasLinkUri(&hasLink)) && hasLink)
                            showOpen = false;
                    }
                    ComPtr<ICoreWebView2ContextMenuItem> openItem;
                    if (showOpen && SUCCEEDED(g_app.env9->CreateContextMenuItem(
                            L"Open", nullptr,
                            COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &openItem)) && openItem) {
                        EventRegistrationToken tok3 = {};
                        openItem->add_CustomItemSelected(
                            Callback<ICoreWebView2CustomItemSelectedEventHandler>(
                                [](ICoreWebView2ContextMenuItem*, IUnknown*) -> HRESULT {
                                    PostMessageW(g_app.hwnd, WM_COMMAND, ID_OPEN, 0);
                                    return S_OK;
                                }).Get(), &tok3);
                        items->InsertValueAtIndex(0, openItem.Get());
                    }
                    return S_OK;
                }).Get(), &tok);
    }

}

static void InitWebView() {
    std::wstring udf = UserDataDir();
    auto handler = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr) || !env) {
                MessageBoxW(g_app.hwnd,
                    L"Could not start WebView2.\n"
                    L"Install Microsoft Edge WebView2 Runtime.",
                    kTitle, MB_ICONERROR | MB_OK);
                return S_OK;
            }
            // Environment9 is needed for CreateContextMenuItem (custom menu item).
            // If the runtime is old and the QI fails, env9 stays null
            // and the menu simply stays stock.
            env->QueryInterface(IID_PPV_ARGS(&g_app.env9));
            env->CreateCoreWebView2Controller(g_app.hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [](HRESULT hr2, ICoreWebView2Controller* c) -> HRESULT {
                        if (FAILED(hr2) || !c) {
                            MessageBoxW(g_app.hwnd, L"Could not create the WebView2 controller.",
                                        kTitle, MB_ICONERROR | MB_OK);
                            return S_OK;
                        }
                        g_app.controller = c;
                        ComPtr<ICoreWebView2Controller2> c2;
                        if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) && c2) {
                            COREWEBVIEW2_COLOR bg = AppDark()
                                ? COREWEBVIEW2_COLOR{255, 13, 17, 23}
                                : COREWEBVIEW2_COLOR{255, 255, 255, 255};
                            c2->put_DefaultBackgroundColor(bg);
                        }
                        c->get_CoreWebView2(&g_app.webview);
                        SetupWebView();
                        ResizeWebView();
                        g_app.ready = true;
                        if (!g_app.currentFile.empty()) LoadFile(g_app.currentFile);
                        else ShowWelcome();
                        return S_OK;
                    }).Get());
            return S_OK;
        });

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, udf.c_str(), nullptr, handler.Get());
    if (FAILED(hr)) {
        MessageBoxW(g_app.hwnd,
            L"WebView2 Runtime not found.\n"
            L"Download it from https://developer.microsoft.com/microsoft-edge/webview2/",
            kTitle, MB_ICONERROR | MB_OK);
    }
}


// ---------- theme / placement ----------

static bool SystemUsesDark() {
    DWORD v = 1, sz = sizeof(v);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, nullptr, (BYTE*)&v, &sz);
        RegCloseKey(k);
    }
    return v == 0;
}

static bool AppDark() {
    // The menu choice wins over everything: light/dark always,
    // and "system" falls back to the old way: env MDVIEWER_DARK=1, else the system setting.
    if (g_app.theme == 2) return true;
    if (g_app.theme == 1) return false;
    wchar_t b[8] = {};
    if (GetEnvironmentVariableW(L"MDVIEWER_DARK", b, 8) > 0 && b[0] == L'1')
        return true;
    return SystemUsesDark();
}

static std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    size_t i = s.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? std::wstring(L".") : s.substr(0, i);
}

static std::wstring IniPath() {
    return ExeDir() + L"\\MdViewer.ini";
}

static bool LoadPlacement(int& x, int& y, int& w, int& h, bool& maximized) {
    std::wstring ini = IniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    int ix = (int)GetPrivateProfileIntW(L"window", L"x", 200, ini.c_str());
    int iy = (int)GetPrivateProfileIntW(L"window", L"y", 200, ini.c_str());
    int iw = (int)GetPrivateProfileIntW(L"window", L"w", 1000, ini.c_str());
    int ih = (int)GetPrivateProfileIntW(L"window", L"h", 780, ini.c_str());
    if (iw < 480 || ih < 360) return false;

    RECT r = { ix, iy, ix + iw, iy + ih };
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return false;  // saved monitor is gone

    x = ix; y = iy; w = iw; h = ih;
    maximized = GetPrivateProfileIntW(L"window", L"maximized", 0, ini.c_str()) != 0;
    return true;
}

static void SavePlacement(HWND hwnd) {
    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) return;
    // rcNormalPosition is the position in the normal (not maximized) state:
    // that is what must be saved, not the current rect, which for a maximized
    // window covers the whole screen. The maximized state is a separate flag.
    RECT r = wp.rcNormalPosition;

    std::wstring ini = IniPath();
    wchar_t buf[32];
    wsprintfW(buf, L"%d", (int)r.left);   WritePrivateProfileStringW(L"window", L"x", buf, ini.c_str());
    wsprintfW(buf, L"%d", (int)r.top);    WritePrivateProfileStringW(L"window", L"y", buf, ini.c_str());
    wsprintfW(buf, L"%d", (int)(r.right - r.left));  WritePrivateProfileStringW(L"window", L"w", buf, ini.c_str());
    wsprintfW(buf, L"%d", (int)(r.bottom - r.top));  WritePrivateProfileStringW(L"window", L"h", buf, ini.c_str());
    wsprintfW(buf, L"%d", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    WritePrivateProfileStringW(L"window", L"maximized", buf, ini.c_str());
}

static void LoadTheme() {
    wchar_t buf[16] = {};
    GetPrivateProfileStringW(L"window", L"theme", L"system",
                             buf, 16, IniPath().c_str());
    if (_wcsicmp(buf, L"dark") == 0) g_app.theme = 2;
    else if (_wcsicmp(buf, L"light") == 0) g_app.theme = 1;
    else g_app.theme = 0;
}

static void SaveTheme() {
    const wchar_t* s = g_app.theme == 2 ? L"dark"
                     : g_app.theme == 1 ? L"light" : L"system";
    WritePrivateProfileStringW(L"window", L"theme", s, IniPath().c_str());
}

// Apply the current theme on the fly (from the menu): the same logic as at
// startup — the page's prefers-color-scheme, the background behind the page,
// and the dark title bar — plus re-rendering the current file and writing the ini.
static void ApplyTheme() {
    ComPtr<ICoreWebView2_13> wv13;
    if (SUCCEEDED(g_app.webview.As(&wv13)) && wv13) {
        ComPtr<ICoreWebView2Profile> profile;
        if (SUCCEEDED(wv13->get_Profile(&profile)) && profile) {
            profile->put_PreferredColorScheme(AppDark()
                ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK
                : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT);
        }
    }
    if (g_app.controller) {
        ComPtr<ICoreWebView2Controller2> c2;
        if (SUCCEEDED(g_app.controller->QueryInterface(IID_PPV_ARGS(&c2))) && c2) {
            COREWEBVIEW2_COLOR bg = AppDark()
                ? COREWEBVIEW2_COLOR{255, 13, 17, 23}
                : COREWEBVIEW2_COLOR{255, 255, 255, 255};
            c2->put_DefaultBackgroundColor(bg);
        }
    }
    BOOL dark = AppDark() ? TRUE : FALSE;
    DwmSetWindowAttribute(g_app.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    DwmSetWindowAttribute(g_app.hwnd, 19 /* DWMWA_USE_IMMERSIVE_DARK_MODE (older builds) */, &dark, sizeof(dark));
    if (!g_app.currentFile.empty()) LoadFile(g_app.currentFile);
    else ShowWelcome();
    SaveTheme();
}

// ---------- window ----------



// ---------- keyboard ----------
// Ctrl+O / F5 are caught by a low-level hook, not AcceleratorKeyPressed:
// WebView2 swallows Ctrl+O itself, and
// ICoreWebView2Controller::add_AcceleratorKeyPressed never fires for
// Ctrl+O, F5 or Ctrl+F (re-checked live 2026-11-25 with the hook and the
// accelerator table disabled: the event log stayed empty). The hook is
// installed on the thread; the window must be the foreground window.
// No logging to a file here.
static HHOOK g_kbHook = nullptr;

static LRESULT CALLBACK KbHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) &&
        GetForegroundWindow() == g_app.hwnd) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lp;
        if (k->vkCode == 'O' && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
            PostMessageW(g_app.hwnd, WM_COMMAND, ID_OPEN, 0);
            return 1;
        }
        if (k->vkCode == VK_F5) {
            PostMessageW(g_app.hwnd, WM_COMMAND, ID_RELOAD, 0);
            return 1;
        }
    }
    return CallNextHookEx(g_kbHook, code, wp, lp);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_app.hwnd = hwnd;
        DragAcceptFiles(hwnd, TRUE);
        InitWebView();
        return 0;

    case WM_SIZE:
        ResizeWebView();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 480;
        mmi->ptMinTrackSize.y = 360;
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == ID_OPEN) {
            OpenDialog();
        } else if (LOWORD(wp) == ID_RELOAD) {
            if (!g_app.currentFile.empty()) LoadFile(g_app.currentFile);
            else ShowWelcome();
        }
        return 0;

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count > 0) {
            wchar_t path[MAX_PATH] = {};
            if (DragQueryFileW(drop, 0, path, MAX_PATH))
                LoadFile(path);
        }
        DragFinish(drop);
        return 0;
    }

    case WM_CLOSE:
        SavePlacement(hwnd);
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_DESTROY:
        SavePlacement(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // The theme is needed first of all: the window class background already depends on AppDark().
    LoadTheme();

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(AppDark() ? RGB(13, 17, 23) : RGB(255, 255, 255));
    wc.hIcon         = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm       = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    std::wstring initial;
    if (cmdLine && *cmdLine) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            if (argc > 1) initial = argv[1];
            LocalFree(argv);
        }
    }

    g_app.currentFile = initial;

    int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT, ww = 1000, wh = 780;
    bool startMaximized = false;
    LoadPlacement(wx, wy, ww, wh, startMaximized);

    HWND hwnd = CreateWindowExW(
        0, kClassName, kTitle,
        WS_OVERLAPPEDWINDOW,
        wx, wy, ww, wh,
        nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    BOOL dark = AppDark() ? TRUE : FALSE;
    // 20 on Windows 11, 19 on Windows 10; one of them silently fails.
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 19 /* DWMWA_USE_IMMERSIVE_DARK_MODE (older builds) */, &dark, sizeof(dark));

    ShowWindow(hwnd, (startMaximized || show == SW_SHOWMAXIMIZED) ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd);

    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbHook, GetModuleHandleW(nullptr), 0);

    ACCEL acc[2] = {
        { FVIRTKEY | FCONTROL, 'O', ID_OPEN },
        { FVIRTKEY, VK_F5, ID_RELOAD }
    };
    HACCEL hAccel = CreateAcceleratorTableW(acc, 2);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(hwnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (hAccel) DestroyAcceleratorTable(hAccel);
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);

    if (g_app.controller) g_app.controller->Close();
    CoUninitialize();
    return (int)msg.wParam;
}
