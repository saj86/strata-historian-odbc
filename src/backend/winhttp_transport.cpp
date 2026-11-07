// WinHTTP transport. OS-provided; adds no redistributable dependency. TLS certificate
// validation is left at the secure default and the driver has no option to disable it.
#include "http_transport.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

#include <atomic>

namespace strata {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};
std::string b64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16 | static_cast<unsigned char>(in[i + 1]) << 8 | static_cast<unsigned char>(in[i + 2]);
        o += t[v >> 18]; o += t[v >> 12 & 63]; o += t[v >> 6 & 63]; o += t[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        o += t[v >> 18]; o += t[v >> 12 & 63]; o += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16 | static_cast<unsigned char>(in[i + 1]) << 8;
        o += t[v >> 18]; o += t[v >> 12 & 63]; o += t[v >> 6 & 63]; o += '=';
    }
    return o;
}

class WinHttpTransport final : public HttpTransport {
public:
    explicit WinHttpTransport(TransportConfig cfg) : cfg_(std::move(cfg)) {
        session_.h = WinHttpOpen(L"StrataHistorianOdbc/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_.h) fail("WinHttpOpen");
        DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(session_.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls)) {
            tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // older Windows without TLS 1.3
            WinHttpSetOption(session_.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls);
        }
    }
    HttpResponse get(const HttpRequest& req, CancelToken& cancel) override {
        cancel.check();
        std::wstring url = widen(req.url);
        URL_COMPONENTS u{};
        u.dwStructSize = sizeof u;
        wchar_t host[256], path[8192], extra[8192];
        u.lpszHostName = host; u.dwHostNameLength = 256;
        u.lpszUrlPath = path; u.dwUrlPathLength = 8192;
        u.lpszExtraInfo = extra; u.dwExtraInfoLength = 8192;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) throw Error("HY000", "Invalid request URL.");
        std::wstring target = std::wstring(path, u.dwUrlPathLength) + std::wstring(extra, u.dwExtraInfoLength);

        Handle con;
        con.h = WinHttpConnect(session_.h, std::wstring(host, u.dwHostNameLength).c_str(), u.nPort, 0);
        if (!con.h) fail("WinHttpConnect");
        DWORD flags = u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET rq = WinHttpOpenRequest(con.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!rq) fail("WinHttpOpenRequest");

        // SQLCancel closes the request handle from another thread; the blocked call then
        // fails with ERROR_WINHTTP_OPERATION_CANCELLED. Exactly one side closes it.
        std::atomic<HINTERNET> shared{rq};
        cancel.setOnCancel([&shared] {
            HINTERNET h = shared.exchange(nullptr);
            if (h) WinHttpCloseHandle(h);
        });
        struct Guard {
            std::atomic<HINTERNET>& s;
            CancelToken& c;
            ~Guard() {
                c.clearOnCancel();
                HINTERNET h = s.exchange(nullptr);
                if (h) WinHttpCloseHandle(h);
            }
        } guard{shared, cancel};

        long long remaining = cancel.remainingMs();
        int t = static_cast<int>(remaining >= 0 && remaining < req.timeoutMs ? remaining : req.timeoutMs);
        WinHttpSetTimeouts(rq, t, t, t, t);

        std::wstring headers = L"Accept: application/json\r\n";
        if (cfg_.auth == AuthMode::Basic) {
            headers += L"Authorization: Basic " + widen(b64(cfg_.user + ":" + cfg_.password)) + L"\r\n";
        } else if (cfg_.auth == AuthMode::Negotiate) {
            DWORD policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_LOW;  // send the current thread identity to this host only
            WinHttpSetOption(rq, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof policy);
        }

        BOOL ok = WinHttpSendRequest(rq, headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (ok) ok = WinHttpReceiveResponse(rq, nullptr);
        if (ok && cfg_.auth == AuthMode::Negotiate) {
            DWORD st = queryStatus(rq);
            if (st == 401) {
                DWORD supported, first, target2;
                if (WinHttpQueryAuthSchemes(rq, &supported, &first, &target2)) {
                    DWORD scheme = (supported & WINHTTP_AUTH_SCHEME_NEGOTIATE) ? WINHTTP_AUTH_SCHEME_NEGOTIATE : (supported & WINHTTP_AUTH_SCHEME_NTLM) ? WINHTTP_AUTH_SCHEME_NTLM : 0;
                    if (scheme && WinHttpSetCredentials(rq, target2, scheme, nullptr, nullptr, nullptr)) {
                        ok = WinHttpSendRequest(rq, headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
                        if (ok) ok = WinHttpReceiveResponse(rq, nullptr);
                    }
                }
            }
        }
        if (!ok) failNetwork(cancel);

        HttpResponse resp;
        resp.status = static_cast<int>(queryStatus(rq));
        wchar_t ra[128];
        DWORD raLen = sizeof ra;
        if (WinHttpQueryHeaders(rq, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, ra, &raLen, WINHTTP_NO_HEADER_INDEX)) {
            for (DWORD i = 0; i < raLen / sizeof(wchar_t); i++) resp.retryAfter += static_cast<char>(ra[i]);
        }
        char buf[65536];
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(rq, buf, sizeof buf, &got)) failNetwork(cancel);
            if (got == 0) break;
            resp.body.append(buf, got);
            if (static_cast<int>(resp.body.size()) > cfg_.maxResponseBytes)
                throw Error("HY000", "Upstream response exceeded the response size limit; lower PageSize.");
        }
        return resp;
    }

private:
    static DWORD queryStatus(HINTERNET rq) {
        DWORD st = 0, len = sizeof st;
        WinHttpQueryHeaders(rq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &len, WINHTTP_NO_HEADER_INDEX);
        return st;
    }
    [[noreturn]] static void fail(const char* what) {
        throw Error("08001", std::string(what) + " failed, Windows error " + std::to_string(GetLastError()) + ".", static_cast<int>(GetLastError()));
    }
    [[noreturn]] static void failNetwork(CancelToken& cancel) {
        DWORD e = GetLastError();
        cancel.check();
        switch (e) {
            case ERROR_WINHTTP_TIMEOUT: throw Error("HYT00", "Upstream request timed out.", static_cast<int>(e));
            case ERROR_WINHTTP_OPERATION_CANCELLED: throw Error("HY008", "Operation canceled.", static_cast<int>(e));
            case ERROR_WINHTTP_SECURE_FAILURE:
            case ERROR_WINHTTP_SECURE_INVALID_CA:
            case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
            case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
            case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
                throw Error("08001", "TLS certificate validation failed for the configured BaseUrl. Fix the certificate chain on this host; validation cannot be disabled.", static_cast<int>(e));
            case ERROR_WINHTTP_CANNOT_CONNECT:
            case ERROR_WINHTTP_NAME_NOT_RESOLVED:
                throw Error("08S01", "Cannot reach the configured BaseUrl from this host.", static_cast<int>(e));
            default:
                throw Error("08S01", "Network failure talking to the configured BaseUrl, Windows error " + std::to_string(e) + ". Any partial result is incomplete.", static_cast<int>(e));
        }
    }
    TransportConfig cfg_;
    Handle session_;
};
}  // namespace

std::unique_ptr<HttpTransport> makeWinHttpTransport(const TransportConfig& cfg) { return std::make_unique<WinHttpTransport>(cfg); }

}  // namespace strata
#endif  // _WIN32
