// Connection-string and DSN settings. Credentials come only from the connection string the
// client supplies at connect time; UID/PWD stored in a DSN are ignored on purpose.
#pragma once
#include <functional>
#include <map>

#include "../backend/http_transport.h"
#include "../core/planner.h"

namespace strata {

struct Config {
    std::string dsn;
    std::string backend;     // fixture | piwebapi | native. Required: there is no silent default.
    std::string baseUrl, dataServer;
    AuthMode auth = AuthMode::Anonymous;
    std::string user, password;
    Limits limits;
    int requestTimeoutMs = 30000;
    int maxRetries = 3;
    int maxConcurrency = 4;
    std::string traceFile;
};

using KeyValues = std::map<std::string, std::string>;  // keys upper-cased

// Handles ODBC {braced} values and ;; inside braces. Throws 08001 on malformed input.
KeyValues parseConnectionString(std::string_view s);

// DSN values fill gaps; values given explicitly in the connection string win.
// dsnLookup(dsnName, keyUpper) returns "" when unset.
Config resolveConfig(const KeyValues& conn, const std::function<std::string(const std::string&, const std::string&)>& dsnLookup);

std::unique_ptr<Backend> makeBackend(const Config& cfg);

// Completed connection string for SQLDriverConnect output; secrets are removed.
std::string redactedConnectionString(const KeyValues& conn);

}  // namespace strata
