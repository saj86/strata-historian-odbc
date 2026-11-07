#include "config.h"

#include "../backend/piwebapi_backend.h"

namespace strata {
namespace {
int parseInt(const std::string& key, const std::string& v, int minV, int maxV) {
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || *end != 0 || n < minV || n > maxV)
        throw Error("08001", "Setting " + key + "=" + v + " is invalid; expected an integer from " + std::to_string(minV) + " to " + std::to_string(maxV) + ".");
    return static_cast<int>(n);
}
bool parseBool(const std::string& key, const std::string& v) {
    if (v == "1" || iequals(v, "yes") || iequals(v, "true")) return true;
    if (v == "0" || iequals(v, "no") || iequals(v, "false")) return false;
    throw Error("08001", "Setting " + key + "=" + v + " is invalid; expected 0 or 1.");
}
}  // namespace

KeyValues parseConnectionString(std::string_view s) {
    KeyValues out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ';' || s[i] == ' ')) i++;
        if (i >= s.size()) break;
        size_t eq = s.find('=', i);
        if (eq == std::string_view::npos) throw Error("08001", "Malformed connection string.");
        std::string key = toUpper(s.substr(i, eq - i));
        while (!key.empty() && key.back() == ' ') key.pop_back();
        i = eq + 1;
        std::string value;
        if (i < s.size() && s[i] == '{') {
            i++;
            for (;;) {
                if (i >= s.size()) throw Error("08001", "Unterminated {value} in connection string.");
                if (s[i] == '}') {
                    if (i + 1 < s.size() && s[i + 1] == '}') { value += '}'; i += 2; continue; }
                    i++;
                    break;
                }
                value += s[i++];
            }
        } else {
            size_t e = s.find(';', i);
            if (e == std::string_view::npos) e = s.size();
            value = std::string(s.substr(i, e - i));
            i = e;
        }
        // ODBC: the first occurrence of a keyword wins.
        out.emplace(std::move(key), std::move(value));
    }
    return out;
}

Config resolveConfig(const KeyValues& conn, const std::function<std::string(const std::string&, const std::string&)>& lookup) {
    Config c;
    auto cs = [&](const char* k) -> std::string { auto it = conn.find(k); return it == conn.end() ? std::string{} : it->second; };
    c.dsn = cs("DSN");
    auto get = [&](const char* k) -> std::string {
        std::string v = cs(k);
        if (v.empty() && !c.dsn.empty() && lookup) v = lookup(c.dsn, k);
        return v;
    };
    c.backend = get("BACKEND");
    c.baseUrl = get("BASEURL");
    c.dataServer = get("DATASERVER");
    c.traceFile = get("TRACEFILE");
    // Credentials are connection-string only, never read back from the DSN.
    c.user = cs("UID");
    c.password = cs("PWD");

    std::string auth = get("AUTH");
    if (auth.empty()) auth = !c.user.empty() ? "Basic" : "Anonymous";
    if (iequals(auth, "Basic")) c.auth = AuthMode::Basic;
    else if (iequals(auth, "Negotiate") || iequals(auth, "Windows")) c.auth = AuthMode::Negotiate;
    else if (iequals(auth, "Anonymous")) c.auth = AuthMode::Anonymous;
    else throw Error("08001", "Auth=" + auth + " is invalid; expected Anonymous, Basic or Negotiate.");

    auto setInt = [&](const char* k, int& target, int minV, int maxV) {
        std::string v = get(k);
        if (!v.empty()) target = parseInt(k, v, minV, maxV);
    };
    setInt("MAXWINDOWDAYS", c.limits.maxWindowDays, 1, 3660);
    setInt("PAGESIZE", c.limits.pageSize, 1, 150000);
    setInt("MAXPOINTSPERQUERY", c.limits.maxPointsPerQuery, 1, 1000);
    setInt("PREVIEWWINDOWHOURS", c.limits.previewWindowHours, 1, 744);
    setInt("PREVIEWROWS", c.limits.previewRows, 1, 10000);
    setInt("REQUESTTIMEOUTMS", c.requestTimeoutMs, 1000, 3600000);
    setInt("MAXRETRIES", c.maxRetries, 0, 10);
    setInt("MAXCONCURRENCY", c.maxConcurrency, 1, 64);
    int maxRows = 0;
    setInt("MAXROWS", maxRows, 1, 2000000000);
    if (maxRows) c.limits.maxRows = maxRows;
    std::string preview = get("NAVIGATORPREVIEW");
    if (!preview.empty()) c.limits.navigatorPreview = parseBool("NavigatorPreview", preview);

    if (c.auth == AuthMode::Basic && (c.user.empty() || c.password.empty()))
        throw Error("28000", "Auth=Basic needs UID and PWD supplied by the client at connect time. Credentials stored in a DSN are ignored.");
    return c;
}

std::unique_ptr<Backend> makeBackend(const Config& c) {
    if (c.backend.empty()) throw Error("08001", "No Backend= setting. Set Backend=fixture or Backend=piwebapi in the DSN; the driver never falls back to fixture data.");
    if (iequals(c.backend, "fixture")) return makeFixtureBackend();
    if (iequals(c.backend, "native"))
        throw Error("08001", "unsupported-backend: no native protocol transaction has verified evidence (docs/evidence-log.md). The driver will not simulate a live read.");
    if (iequals(c.backend, "piwebapi")) {
        if (c.baseUrl.empty() || c.dataServer.empty()) throw Error("08001", "Backend=piwebapi needs BaseUrl and DataServer.");
        validateBaseUrl(c.baseUrl);
#ifdef _WIN32
        TransportConfig tc;
        tc.baseUrl = c.baseUrl;
        tc.auth = c.auth;
        tc.user = c.user;
        tc.password = c.password;
        PiWebApiConfig pc;
        pc.baseUrl = c.baseUrl;
        pc.dataServer = c.dataServer;
        pc.requestTimeoutMs = c.requestTimeoutMs;
        pc.maxRetries = c.maxRetries;
        pc.maxConcurrency = c.maxConcurrency;
        pc.trace = Trace(c.traceFile);
        return makePiWebApiBackend(std::move(pc), makeWinHttpTransport(tc));
#else
        throw Error("08001", "unsupported-backend: the PI Web API transport is implemented with WinHTTP and exists only in the Windows build.");
#endif
    }
    throw Error("08001", "Backend=" + c.backend + " is not recognized; expected fixture or piwebapi.");
}

std::string redactedConnectionString(const KeyValues& conn) {
    std::string o;
    for (auto& [k, v] : conn) {
        if (k == "PWD" || k == "UID") continue;
        o += k + "=";
        o += v.find_first_of(";{} ") != std::string::npos ? "{" + v + "}" : v;
        o += ";";
    }
    return o;
}

}  // namespace strata
