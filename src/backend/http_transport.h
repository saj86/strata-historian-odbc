// The server transport. Implementations: WinHTTP (Windows) and scripted fakes (tests).
#pragma once
#include <map>
#include <memory>

#include "../core/common.h"

namespace strata {

enum class AuthMode { Anonymous, Basic, Negotiate };

struct HttpRequest {
    std::string url;
    long long timeoutMs = 30000;
};
struct HttpResponse {
    int status = 0;
    std::string body;
    std::string retryAfter;  // raw Retry-After header, empty when absent
};

// Network failures throw Error: HYT00 timeout, HY008 canceled, 08S01 link failure, 08001 TLS/connect.
class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual HttpResponse get(const HttpRequest& req, CancelToken& cancel) = 0;
};

struct TransportConfig {
    std::string baseUrl;
    AuthMode auth = AuthMode::Anonymous;
    std::string user, password;
    int maxResponseBytes = 64 * 1024 * 1024;
};

// Validates the URL policy: https required, http allowed only for loopback test servers.
void validateBaseUrl(const std::string& url);

#ifdef _WIN32
std::unique_ptr<HttpTransport> makeWinHttpTransport(const TransportConfig& cfg);
#else
std::unique_ptr<HttpTransport> makeLoopbackTransport();  // test builds only: http://127.0.0.1
#endif

}  // namespace strata
