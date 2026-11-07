// Windows setup entry points. ODBC Administrator and SQLConfigDataSource call ConfigDSNW.
// There is no dialog by design: gateways are unattended and UID/PWD are refused so that
// credentials cannot end up in the registry. Use scripts/Provision-StrataDsn.ps1.
#ifdef _WIN32
#include <windows.h>
#include <odbcinst.h>
#include <sql.h>

#include <string>
#include <vector>

namespace {
const wchar_t* kIni = L"ODBC.INI";
}

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
    if (hwnd) {
        MessageBoxW(hwnd, L"This driver has no configuration dialog.\nProvision DSNs with scripts\\Provision-StrataDsn.ps1.", L"Strata Historian ODBC Driver", MB_OK | MB_ICONINFORMATION);
        if (dsn.empty()) return FALSE;
    }
    if (dsn.empty()) {
        SQLPostInstallerErrorW(ODBC_ERROR_INVALID_NAME, L"DSN= is required.");
        return FALSE;
    }
    switch (request) {
        case ODBC_ADD_DSN: case ODBC_CONFIG_DSN: case ODBC_ADD_SYS_DSN: case ODBC_CONFIG_SYS_DSN:
            if (!SQLValidDSNW(dsn.c_str())) return FALSE;
            if (request == ODBC_ADD_DSN || request == ODBC_ADD_SYS_DSN) {
                if (!SQLWriteDSNToIniW(dsn.c_str(), driver)) return FALSE;
            }
            for (auto& [k, v] : kv)
                if (!SQLWritePrivateProfileStringW(dsn.c_str(), k.c_str(), v.c_str(), kIni)) return FALSE;
            return TRUE;
        case ODBC_REMOVE_DSN: case ODBC_REMOVE_SYS_DSN:
            return SQLRemoveDSNFromIniW(dsn.c_str());
        default:
            SQLPostInstallerErrorW(ODBC_ERROR_INVALID_REQUEST_TYPE, L"Unsupported ConfigDSN request.");
            return FALSE;
    }
}

extern "C" BOOL WINAPI ConfigDriverW(HWND, WORD request, LPCWSTR, LPCWSTR, LPWSTR msg, WORD msgMax, WORD* msgOut) {
    if (msgOut) *msgOut = 0;
    if (msg && msgMax) *msg = 0;
    return request == ODBC_INSTALL_DRIVER || request == ODBC_REMOVE_DRIVER;
}
#endif  // _WIN32
