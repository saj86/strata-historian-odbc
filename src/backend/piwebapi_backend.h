// PI Web API backend. Documented interface; see docs/backend-decision.md for the evidence
// status of each field. Gate B (real server, trusted reader comparison) has NOT been run.
#pragma once
#include "backend.h"
#include "http_transport.h"

namespace strata {

struct PiWebApiConfig {
    std::string baseUrl;     // e.g. https://piweb.example.local/piwebapi
    std::string dataServer;  // exact data server name; required
    int requestTimeoutMs = 30000;
    int maxRetries = 3;
    int maxRetryAfterSeconds = 60;
    int maxConcurrency = 4;  // per process, across connections, per BaseUrl
    Trace trace;
};

std::unique_ptr<Backend> makePiWebApiBackend(PiWebApiConfig cfg, std::unique_ptr<HttpTransport> transport);

// Exposed for tests. A page is "full" when it has maxCount events; the next request restarts
// at the last timestamp and skips the events already delivered at that timestamp.
std::string encodeRecordedContinuation(const std::string& rawTime, int alreadyDelivered);

}  // namespace strata
