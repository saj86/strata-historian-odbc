// Plain-HTTP transport for non-Windows test builds. It only talks to a loopback mock server, so the
// driver, backend and mock can be tested together on a development machine. Windows uses WinHTTP.
#include "http_transport.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>

namespace strata {
namespace {

class LoopbackTransport final : public HttpTransport {
public:
    HttpResponse get(const HttpRequest& req, CancelToken& cancel) override {
        const std::string prefix = "http://127.0.0.1:";
        if (req.url.rfind(prefix, 0) != 0) throw Error("08001", "The test transport only reaches http://127.0.0.1.");
        size_t slash = req.url.find('/', prefix.size());
        int port = std::atoi(req.url.substr(prefix.size(), slash - prefix.size()).c_str());
        std::string path = slash == std::string::npos ? "/" : req.url.substr(slash);

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) throw Error("08001", "Cannot create a socket.");
        struct Closer { int fd; ~Closer() { close(fd); } } closer{fd};
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) throw Error("08S01", "Cannot reach the configured BaseUrl from this host.");

        std::string request = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
        if (send(fd, request.data(), request.size(), 0) < 0) throw Error("08S01", "Network failure sending the request.");

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(req.timeoutMs);
        std::string raw;
        char buf[65536];
        for (;;) {
            cancel.check();
            if (std::chrono::steady_clock::now() > deadline) throw Error("HYT00", "Upstream request timed out.");
            pollfd pfd{fd, POLLIN, 0};
            int ready = poll(&pfd, 1, 50);
            if (ready < 0) throw Error("08S01", "Network failure reading the response.");
            if (ready == 0) continue;
            ssize_t got = recv(fd, buf, sizeof buf, 0);
            if (got < 0) throw Error("08S01", "Network failure reading the response.");
            if (got == 0) break;
            raw.append(buf, static_cast<size_t>(got));
        }
        size_t split = raw.find("\r\n\r\n");
        if (split == std::string::npos || raw.size() < 12) throw Error("08S01", "Malformed HTTP response.");
        HttpResponse resp;
        resp.status = std::atoi(raw.c_str() + 9);
        resp.body = raw.substr(split + 4);
        std::string headers = toUpper(raw.substr(0, split));
        size_t at = headers.find("\r\nRETRY-AFTER:");
        if (at != std::string::npos) {
            size_t start = raw.find_first_not_of(' ', at + 14);
            resp.retryAfter = raw.substr(start, raw.find("\r\n", start) - start);
        }
        return resp;
    }
};
}  // namespace

std::unique_ptr<HttpTransport> makeLoopbackTransport() { return std::make_unique<LoopbackTransport>(); }

}  // namespace strata
#endif  // !_WIN32
