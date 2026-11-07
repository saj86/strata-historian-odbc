// Windows setup entry points. The ODBC Data Sources tool calls ConfigDSNW to add, configure or remove
// a data source. With a window handle it shows the settings window; without one (scripts, installers)
// it works silently. Either way, UID and PWD are never stored, so credentials can't reach the registry.
#ifdef _WIN32
#include <windows.h>
#include <odbcinst.h>
#include <sql.h>

#include <string>
#include <vector>

#include "../backend/backend.h"
#include "dsn_form.h"
#include "resource.h"
#include "wstr.h"

using namespace strata;

namespace {
const wchar_t* kIni = L"ODBC.INI";

std::wstring W(const std::string& s) {
    std::u16string u = utf8ToUtf16(s);
    return std::wstring(u.begin(), u.end());
}
std::string N(const std::wstring& w) { return utf16ToUtf8(reinterpret_cast<const char16_t*>(w.c_str()), static_cast<long>(w.size())); }

HINSTANCE moduleInstance() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&moduleInstance), &m);
    return m;
}

std::string readValue(const std::wstring& dsn, const std::string& key) {
    wchar_t buf[2048];
    int n = SQLGetPrivateProfileStringW(dsn.c_str(), W(key).c_str(), L"", buf, 2048, kIni);
    return n > 0 ? N(std::wstring(buf, static_cast<size_t>(n))) : std::string{};
}

std::string itemText(HWND dlg, int id) {
    HWND item = GetDlgItem(dlg, id);
    int len = GetWindowTextLengthW(item);
    std::wstring w(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(item, w.data(), len + 1);
    w.resize(static_cast<size_t>(len));
    std::string s = N(w);
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
}
void setText(HWND dlg, int id, const std::string& s) { SetDlgItemTextW(dlg, id, W(s).c_str()); }

struct Window {
    DsnForm form;
};

const wchar_t* kBackends[] = {L"PI Web API", L"Test data (fixture, not PI)"};
const char* kBackendValues[] = {"piwebapi", "fixture"};
const wchar_t* kAuths[] = {L"Windows sign-in (Negotiate)", L"User name and password (Basic)", L"No sign-in (Anonymous)"};
const char* kAuthValues[] = {"Negotiate", "Basic", "Anonymous"};

void updateEnabled(HWND dlg) {
    BOOL live = SendDlgItemMessageW(dlg, IDC_BACKEND, CB_GETCURSEL, 0, 0) == 0;
    for (int id : {IDC_BASEURL, IDC_SERVER, IDC_AUTH, IDC_USER, IDC_PWD}) EnableWindow(GetDlgItem(dlg, id), live);
}

DsnForm readForm(HWND dlg) {
    DsnForm f;
    f.name = itemText(dlg, IDC_NAME);
    f.description = itemText(dlg, IDC_DESC);
    LRESULT b = SendDlgItemMessageW(dlg, IDC_BACKEND, CB_GETCURSEL, 0, 0);
    f.backend = kBackendValues[b == 1 ? 1 : 0];
    f.baseUrl = itemText(dlg, IDC_BASEURL);
    f.dataServer = itemText(dlg, IDC_SERVER);
    LRESULT a = SendDlgItemMessageW(dlg, IDC_AUTH, CB_GETCURSEL, 0, 0);
    f.auth = kAuthValues[a >= 0 && a <= 2 ? a : 0];
    BOOL ok = FALSE;
    UINT days = GetDlgItemInt(dlg, IDC_MAXDAYS, &ok, FALSE);
    f.maxWindowDays = ok ? static_cast<int>(days) : 0;
    UINT page = GetDlgItemInt(dlg, IDC_PAGESIZE, &ok, FALSE);
    f.pageSize = ok ? static_cast<int>(page) : 0;
    f.navigatorPreview = IsDlgButtonChecked(dlg, IDC_PREVIEW) == BST_CHECKED;
    f.traceFile = itemText(dlg, IDC_TRACE);
    return f;
}

void testConnection(HWND dlg) {
    DsnForm f = readForm(dlg);
    if (f.name.empty()) f.name = "Test";
    std::string problem = validateDsnForm(f);
    if (!problem.empty()) {
        MessageBoxW(dlg, W(problem).c_str(), L"Test connection", MB_OK | MB_ICONWARNING);
        return;
    }
    HCURSOR old = SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514)));  // IDC_WAIT
    std::string message;
    UINT icon = MB_ICONERROR;
    try {
        Config cfg = resolveConfig(dsnTestConnection(f, itemText(dlg, IDC_USER), itemText(dlg, IDC_PWD)), nullptr);
        auto backend = makeBackend(cfg);
        CancelToken token;
        token.setTimeoutSeconds(20);
        backend->testConnection(token);
        message = "Connected.\n\nData from: " + backend->name() + "\nData server: " + backend->server();
        if (backend->name() == "fixture") message += "\n\nThis is synthetic test data, not PI.";
        icon = MB_ICONINFORMATION;
    } catch (const Error& e) {
        message = std::string("Could not connect.\n\n") + e.what() + "\n\nSQLSTATE " + e.sqlstate;
    } catch (const std::exception& e) {
        message = std::string("Could not connect.\n\n") + e.what();
    }
    SetCursor(old);
    SetDlgItemTextW(dlg, IDC_PWD, L"");  // the password leaves the window as soon as it has been used
    MessageBoxW(dlg, W(message).c_str(), L"Test connection", MB_OK | icon);
}

INT_PTR CALLBACK dialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            auto* w = reinterpret_cast<Window*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            const DsnForm& f = w->form;
            SendDlgItemMessageW(dlg, IDC_NAME, EM_LIMITTEXT, 32, 0);
            setText(dlg, IDC_NAME, f.name);
            setText(dlg, IDC_DESC, f.description);
            for (auto* s : kBackends) SendDlgItemMessageW(dlg, IDC_BACKEND, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
            SendDlgItemMessageW(dlg, IDC_BACKEND, CB_SETCURSEL, iequals(f.backend, "fixture") ? 1 : 0, 0);
            setText(dlg, IDC_BASEURL, f.baseUrl);
            setText(dlg, IDC_SERVER, f.dataServer);
            for (auto* s : kAuths) SendDlgItemMessageW(dlg, IDC_AUTH, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
            int auth = iequals(f.auth, "Basic") ? 1 : iequals(f.auth, "Anonymous") ? 2 : 0;
            SendDlgItemMessageW(dlg, IDC_AUTH, CB_SETCURSEL, auth, 0);
            SetDlgItemInt(dlg, IDC_MAXDAYS, static_cast<UINT>(f.maxWindowDays), FALSE);
            SetDlgItemInt(dlg, IDC_PAGESIZE, static_cast<UINT>(f.pageSize), FALSE);
            CheckDlgButton(dlg, IDC_PREVIEW, f.navigatorPreview ? BST_CHECKED : BST_UNCHECKED);
            setText(dlg, IDC_TRACE, f.traceFile);
            updateEnabled(dlg);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_BACKEND:
                    if (HIWORD(wp) == CBN_SELCHANGE) updateEnabled(dlg);
                    return TRUE;
                case IDC_TEST:
                    testConnection(dlg);
                    return TRUE;
                case IDOK: {
                    DsnForm f = readForm(dlg);
                    std::string problem = validateDsnForm(f);
                    if (problem.empty() && !SQLValidDSNW(W(f.name).c_str())) problem = "That data source name isn't allowed.";
                    if (!problem.empty()) {
                        MessageBoxW(dlg, W(problem).c_str(), L"Data source", MB_OK | MB_ICONWARNING);
                        return TRUE;
                    }
                    reinterpret_cast<Window*>(GetWindowLongPtrW(dlg, DWLP_USER))->form = f;
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

BOOL write(const std::wstring& dsn, LPCWSTR driver, bool createSection, const std::vector<std::pair<std::string, std::string>>& values) {
    if (createSection && !SQLWriteDSNToIniW(dsn.c_str(), driver)) return FALSE;
    for (auto& [k, v] : values) {
        // An empty value removes the key, so switching to test data clears the server settings.
        if (!SQLWritePrivateProfileStringW(dsn.c_str(), W(k).c_str(), v.empty() ? nullptr : W(v).c_str(), kIni)) return FALSE;
    }
    return TRUE;
}
}  // namespace

extern "C" BOOL WINAPI ConfigDSNW(HWND hwnd, WORD request, LPCWSTR driver, LPCWSTR attributes) {
    std::wstring dsn;
    std::vector<std::pair<std::wstring, std::wstring>> kv;
    for (const wchar_t* p = attributes; p && *p; p += wcslen(p) + 1) {
        std::wstring item(p);
        auto eq = item.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = item.substr(0, eq), v = item.substr(eq + 1);
        if (_wcsicmp(k.c_str(), L"DSN") == 0) { dsn = v; continue; }
        if (_wcsicmp(k.c_str(), L"UID") == 0 || _wcsicmp(k.c_str(), L"PWD") == 0) {
            SQLPostInstallerErrorW(ODBC_ERROR_INVALID_KEYWORD_VALUE, L"UID and PWD must not be stored in a DSN.");
            return FALSE;
        }
        kv.emplace_back(k, v);
    }
    const bool remove = request == ODBC_REMOVE_DSN || request == ODBC_REMOVE_SYS_DSN;
    const bool add = request == ODBC_ADD_DSN || request == ODBC_ADD_SYS_DSN;
    const bool configure = request == ODBC_CONFIG_DSN || request == ODBC_CONFIG_SYS_DSN;
    if (!remove && !add && !configure) {
        SQLPostInstallerErrorW(ODBC_ERROR_INVALID_REQUEST_TYPE, L"Unsupported ConfigDSN request.");
        return FALSE;
    }
    if (remove) {
        if (dsn.empty()) { SQLPostInstallerErrorW(ODBC_ERROR_INVALID_NAME, L"DSN= is required."); return FALSE; }
        return SQLRemoveDSNFromIniW(dsn.c_str());
    }

    if (!hwnd) {  // silent: scripts and installers
        if (dsn.empty()) { SQLPostInstallerErrorW(ODBC_ERROR_INVALID_NAME, L"DSN= is required."); return FALSE; }
        if (!SQLValidDSNW(dsn.c_str())) return FALSE;
        if (add && !SQLWriteDSNToIniW(dsn.c_str(), driver)) return FALSE;
        for (auto& [k, v] : kv)
            if (!SQLWritePrivateProfileStringW(dsn.c_str(), k.c_str(), v.c_str(), kIni)) return FALSE;
        return TRUE;
    }

    Window window;
    if (configure && !dsn.empty()) window.form = dsnFormFromLookup(N(dsn), [&](const std::string& key) { return readValue(dsn, key); });
    else window.form.name = N(dsn);
    if (DialogBoxParamW(moduleInstance(), MAKEINTRESOURCEW(IDD_DSN), hwnd, dialogProc, reinterpret_cast<LPARAM>(&window)) != IDOK) return FALSE;

    std::wstring name = W(window.form.name);
    const bool renamed = configure && !dsn.empty() && _wcsicmp(name.c_str(), dsn.c_str()) != 0;
    if (renamed && !SQLRemoveDSNFromIniW(dsn.c_str())) return FALSE;
    return write(name, driver, add || renamed || dsn.empty(), dsnAttributes(window.form));
}

extern "C" BOOL WINAPI ConfigDriverW(HWND, WORD request, LPCWSTR, LPCWSTR, LPWSTR msg, WORD msgMax, WORD* msgOut) {
    if (msgOut) *msgOut = 0;
    if (msg && msgMax) *msg = 0;
    return request == ODBC_INSTALL_DRIVER || request == ODBC_REMOVE_DRIVER;
}
#endif  // _WIN32
