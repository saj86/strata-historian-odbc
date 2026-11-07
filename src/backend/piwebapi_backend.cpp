#include "piwebapi_backend.h"

#include <cmath>
#include <condition_variable>
#include <cstdio>

#include "../core/json.h"

namespace strata {
namespace {

std::string urlEncode(std::string_view s) {
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else { char b[8]; std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

// Process-wide request gate keyed by BaseUrl so concurrent refresh queries cannot overload the source.
class Gate {
public:
    static Gate& forUrl(const std::string& url, int capacity) {
        static std::mutex m;
        static std::map<std::string, std::unique_ptr<Gate>> gates;
        std::lock_guard<std::mutex> g(m);
        auto& p = gates[url];
        if (!p) p = std::make_unique<Gate>(capacity);
        return *p;
    }
    explicit Gate(int cap) : free_(cap < 1 ? 1 : cap) {}
    void acquire(CancelToken& c) {
        std::unique_lock<std::mutex> l(m_);
        while (free_ == 0) {
            c.check();
            cv_.wait_for(l, std::chrono::milliseconds(20));
        }
        free_--;
    }
    void release() {
        { std::lock_guard<std::mutex> l(m_); free_++; }
        cv_.notify_one();
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    int free_;
};

// IMF-fixdate only ("Sun, 06 Nov 1994 08:49:37 GMT"), the form RFC 9110 requires senders to use.
bool parseHttpDate(const std::string& s, Ticks& out) {
    char weekday[4], month[4], zone[4];
    int d, y, h, mi, sec;
    if (std::sscanf(s.c_str(), "%3[A-Za-z], %d %3s %d %d:%d:%d %3s", weekday, &d, month, &y, &h, &mi, &sec, zone) != 8) return false;
    if (std::string_view(zone) != "GMT") return false;
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int m = 0; m < 12; m++) {
        if (std::string_view(month) != months[m]) continue;
        char iso[32];
        std::snprintf(iso, sizeof iso, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, m + 1, d, h, mi, sec);
        return parseUtc(iso, out);  // validates the fields
    }
    return false;
}

// Seconds to wait, from either Retry-After form. Empty when absent or unreadable: the caller backs off.
std::optional<long long> retryAfterSeconds(const std::string& value) {
    if (value.empty()) return std::nullopt;
    char* end = nullptr;
    long long secs = std::strtoll(value.c_str(), &end, 10);
    if (end && *end == 0) return secs >= 0 ? std::optional<long long>(secs) : std::nullopt;
    Ticks when;
    if (!parseHttpDate(value, when)) return std::nullopt;
    long long delta = (when.v - nowUtc().v) / kTicksPerSecond;
    return delta > 0 ? delta : 0;  // a date already past means retry now
}

[[noreturn]] void schema(const std::string& what) {
    // A response we cannot interpret is an error, never a guessed value.
    throw Error("HY000", "Unexpected PI Web API response: " + what + ". No value was inferred. Check the server version against docs/compatibility-matrix.md.");
}
const json::Node& need(const json::Node& n, const char* key) {
    const json::Node* f = n.find(key);
    if (!f) schema(std::string("missing field '") + key + "'");
    return *f;
}
bool needBool(const json::Node& n, const char* key) {
    const json::Node& f = need(n, key);
    if (f.kind != json::Node::Bool) schema(std::string("field '") + key + "' is not boolean");
    return f.b;
}
std::string needStr(const json::Node& n, const char* key) {
    const json::Node& f = need(n, key);
    if (f.kind != json::Node::String) schema(std::string("field '") + key + "' is not text");
    return f.s;
}
std::optional<std::string> optStr(const json::Node& n, const char* key) {
    const json::Node* f = n.find(key);
    if (!f || f->kind != json::Node::String || f->s.empty()) return std::nullopt;
    return f->s;
}
void checkEmbeddedErrors(const json::Node& n) {
    if (const json::Node* e = n.find("Errors"); e && e->kind == json::Node::Array && !e->a.empty()) {
        std::string first = e->a[0].kind == json::Node::String ? e->a[0].s : "see server log";
        throw Error("HY000", "PI Web API reported an error inside the response: " + first + ". The result is incomplete.");
    }
}

RecordedEvent parseEvent(const json::Node& item) {
    checkEmbeddedErrors(item);
    RecordedEvent e;
    e.rawTime = needStr(item, "Timestamp");
    if (!parseUtc(e.rawTime, e.time)) schema("unparseable Timestamp");
    e.good = needBool(item, "Good");
    e.questionable = needBool(item, "Questionable");
    e.substituted = needBool(item, "Substituted");
    e.annotated = needBool(item, "Annotated");
    if (auto u = optStr(item, "UnitsAbbreviation")) e.units = u;
    const json::Node& v = need(item, "Value");
    switch (v.kind) {
        case json::Node::Number: e.kind = ValueKind::Numeric; e.numeric = v.n; break;
        case json::Node::String: e.kind = ValueKind::Text; e.text = v.s; break;
        case json::Node::Bool: e.kind = ValueKind::Numeric; e.numeric = v.b ? 1 : 0; break;
        case json::Node::Object: {
            // Digital or system state: {"Name":...,"Value":n,"IsSystem":bool}
            e.stateText = needStr(v, "Name");
            const json::Node& code = need(v, "Value");
            if (code.kind != json::Node::Number) schema("digital state Value is not numeric");
            e.digitalCode = static_cast<int32_t>(code.n);
            e.kind = needBool(v, "IsSystem") ? ValueKind::System : ValueKind::Digital;
            break;
        }
        default: schema("event Value is null or an array");
    }
    return e;
}

PointInfo parsePoint(const json::Node& n, const std::string& server) {
    PointInfo p;
    p.sourceServer = server;
    p.pointId = needStr(n, "WebId");
    p.tagName = needStr(n, "Name");
    p.descriptor = optStr(n, "Descriptor");
    p.pointClass = optStr(n, "PointClass");
    p.pointType = optStr(n, "PointType");
    p.engUnits = optStr(n, "EngineeringUnits");
    p.digitalSet = optStr(n, "DigitalSetName");
    if (const json::Node* s = n.find("Step"); s && s->kind == json::Node::Bool) p.step = s->b;
    return p;
}

const char* kPointFields = "WebId;Name;Descriptor;PointClass;PointType;EngineeringUnits;DigitalSetName;Step";

class PiWebApiBackend final : public Backend {
public:
    PiWebApiBackend(PiWebApiConfig cfg, std::unique_ptr<HttpTransport> t) : cfg_(std::move(cfg)), http_(std::move(t)), gate_(Gate::forUrl(cfg_.baseUrl, cfg_.maxConcurrency)) {
        while (!cfg_.baseUrl.empty() && cfg_.baseUrl.back() == '/') cfg_.baseUrl.pop_back();
    }
    std::string name() const override { return "piwebapi"; }
    std::string server() const override { return cfg_.dataServer; }
    Ticks previewEnd() const override { return nowUtc(); }

    void testConnection(CancelToken& c) override { resolveServer(c); }

    PointPage listPoints(const PointQuery& q, CancelToken& c) override {
        resolveServer(c);
        int start = q.continuation.empty() ? 0 : std::stoi(q.continuation);
        std::string url = cfg_.baseUrl + "/dataservers/" + urlEncode(serverWebId_) + "/points?maxCount=" + std::to_string(q.pageSize) + "&startIndex=" + std::to_string(start) + "&webIdType=IDOnly";
        if (q.tagName) url += "&nameFilter=" + urlEncode(*q.tagName);
        url += "&selectedFields=" + urlEncode(prefixed(kPointFields));
        json::Node doc = getJson(url, "listPoints", c);
        checkEmbeddedErrors(doc);
        const json::Node& items = need(doc, "Items");
        if (items.kind != json::Node::Array) schema("Items is not an array");
        PointPage page;
        for (auto& n : items.a) page.points.push_back(parsePoint(n, cfg_.dataServer));
        if (static_cast<int>(items.a.size()) == q.pageSize) page.continuation = std::to_string(start + q.pageSize);
        return page;
    }

    PointInfo getPoint(const std::string& id, CancelToken& c) override {
        resolveServer(c);
        std::string url = cfg_.baseUrl + "/points/" + urlEncode(id) + "?webIdType=IDOnly&selectedFields=" + urlEncode(kPointFields);
        json::Node doc = getJson(url, "getPoint", c);
        checkEmbeddedErrors(doc);
        PointInfo p = parsePoint(doc, cfg_.dataServer);
        return p;
    }

    RecordedPage readRecorded(const RecordedRequest& r, CancelToken& c) override {
        std::string startRaw = formatUtc(r.start);
        int skip = 0;
        if (!r.continuation.empty()) {
            auto bar = r.continuation.rfind('|');
            if (bar == std::string::npos) throw Error("HY000", "Corrupt continuation state.");
            startRaw = r.continuation.substr(0, bar);
            skip = std::stoi(r.continuation.substr(bar + 1));
        }
        // Inside: no events are synthesized at the window edges. The planner re-applies exact bounds.
        std::string url = cfg_.baseUrl + "/streams/" + urlEncode(r.pointId) + "/recorded?startTime=" + urlEncode(startRaw) + "&endTime=" + urlEncode(formatUtc(r.end)) + "&boundaryType=Inside&maxCount=" + std::to_string(r.pageSize) +
                          "&selectedFields=" + urlEncode("Items.Timestamp;Items.Value;Items.UnitsAbbreviation;Items.Good;Items.Questionable;Items.Substituted;Items.Annotated;Items.Errors;Errors");
        json::Node doc = getJson(url, "readRecorded", c, r.correlationId);
        checkEmbeddedErrors(doc);
        const json::Node& items = need(doc, "Items");
        if (items.kind != json::Node::Array) schema("Items is not an array");

        RecordedPage page;
        const int n = static_cast<int>(items.a.size());
        if (n < skip) throw Error("08S01", "History changed between pages: fewer events at the continuation timestamp than were already delivered. The result is incomplete; retry the window.");
        Ticks last{};
        std::string lastRaw;
        int runAtLast = 0;
        for (int i = 0; i < n; i++) {
            RecordedEvent e = parseEvent(items.a[static_cast<size_t>(i)]);
            if (i > 0 && e.time < last) schema("events are not in ascending time order");
            if (e.rawTime == lastRaw) runAtLast++;
            else { lastRaw = e.rawTime; runAtLast = 1; }
            last = e.time;
            if (i >= skip) page.events.push_back(std::move(e));
        }
        if (n == r.pageSize) {
            if (runAtLast == n) {
                // The whole page shares one timestamp: this page size cannot make progress.
                throw Error("HY000", "At least PageSize=" + std::to_string(r.pageSize) + " events share timestamp " + lastRaw + "; raise PageSize. The result is incomplete.");
            }
            page.continuation = encodeRecordedContinuation(lastRaw, runAtLast);
        }
        return page;
    }

private:
    static std::string prefixed(const char* fields) {
        std::string o, f(fields);
        size_t p = 0;
        while (p <= f.size()) {
            size_t e = f.find(';', p);
            if (e == std::string::npos) e = f.size();
            if (!o.empty()) o += ';';
            o += "Items." + f.substr(p, e - p);
            p = e + 1;
        }
        return o;
    }
    void resolveServer(CancelToken& c) {
        if (!serverWebId_.empty()) return;
        std::string url = cfg_.baseUrl + "/dataservers?name=" + urlEncode(cfg_.dataServer) + "&webIdType=IDOnly&selectedFields=WebId;Name";
        json::Node doc = getJson(url, "resolveServer", c);
        checkEmbeddedErrors(doc);
        serverWebId_ = needStr(doc, "WebId");
    }
    json::Node getJson(const std::string& url, const char* op, CancelToken& c, const std::string& corr = {}) {
        HttpResponse resp = withRetries(url, op, c, corr);
        if (resp.status >= 200 && resp.status < 300) {
            try {
                return json::parse(resp.body);
            } catch (const std::exception&) {
                throw Error("08S01", "PI Web API returned a malformed or truncated response. The result is incomplete.");
            }
        }
        throwForStatus(resp, op);
    }
    [[noreturn]] void throwForStatus(const HttpResponse& r, const char* op) {
        std::string prefix = std::string("PI Web API ") + op + " returned HTTP " + std::to_string(r.status) + ": ";
        switch (r.status) {
            case 401: throw Error("28000", prefix + "authentication failed for this identity.", 401);
            case 403: throw Error("42000", prefix + "access denied for this identity.", 403);
            case 404: throw Error("42S02", prefix + "not found or not visible to this identity.", 404);
            case 409: throw Error("HYC00", prefix + "the server does not support this operation.", 409);
            default: throw Error("08S01", prefix + "upstream failure after retries. The result is incomplete.", r.status);
        }
    }
    static bool retryable(int status) { return status == 429 || status == 502 || status == 503 || status == 504; }
    HttpResponse withRetries(const std::string& url, const char* op, CancelToken& c, const std::string& corr) {
        long long backoffMs = 500;
        for (int attempt = 1;; attempt++) {
            c.check();
            gate_.acquire(c);
            HttpResponse resp;
            auto t0 = std::chrono::steady_clock::now();
            try {
                HttpRequest rq{url, cfg_.requestTimeoutMs};
                resp = http_->get(rq, c);
            } catch (const Error& e) {
                gate_.release();
                bool transient = e.sqlstate == "08S01" && attempt <= cfg_.maxRetries;
                log(corr, op, attempt, 0, 0, t0, e.sqlstate);
                if (!transient) throw;
                c.sleepMs(backoffMs);
                backoffMs *= 2;
                continue;
            }
            gate_.release();
            log(corr, op, attempt, resp.status, resp.body.size(), t0, "");
            if (!retryable(resp.status) || attempt > cfg_.maxRetries) return resp;
            long long waitMs = backoffMs;
            if (auto secs = retryAfterSeconds(resp.retryAfter)) {
                if (*secs > cfg_.maxRetryAfterSeconds) return resp;  // do not wait longer than policy allows
                waitMs = *secs * 1000;
            }
            c.sleepMs(waitMs);
            backoffMs *= 2;
        }
    }
    void log(const std::string& corr, const char* op, int attempt, int status, size_t bytes, std::chrono::steady_clock::time_point t0, const std::string& err) {
        if (!cfg_.trace.enabled()) return;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        cfg_.trace.write(corr, "http", "\"op\":" + Trace::quote(op) + ",\"attempt\":" + std::to_string(attempt) + ",\"status\":" + std::to_string(status) + ",\"bytes\":" + std::to_string(bytes) + ",\"ms\":" + std::to_string(ms) + ",\"error\":" + Trace::quote(err));
    }
    PiWebApiConfig cfg_;
    std::unique_ptr<HttpTransport> http_;
    Gate& gate_;
    std::string serverWebId_;
};
}  // namespace

std::string encodeRecordedContinuation(const std::string& raw, int delivered) { return raw + "|" + std::to_string(delivered); }

std::unique_ptr<Backend> makePiWebApiBackend(PiWebApiConfig cfg, std::unique_ptr<HttpTransport> t) { return std::make_unique<PiWebApiBackend>(std::move(cfg), std::move(t)); }

void validateBaseUrl(const std::string& url) {
    std::string u = url;
    auto low = toUpper(u);
    if (low.rfind("HTTPS://", 0) == 0) return;
    if (low.rfind("HTTP://127.0.0.1", 0) == 0 || low.rfind("HTTP://LOCALHOST", 0) == 0) return;  // loopback mock only
    throw Error("08001", "BaseUrl must use https:// (plain http is accepted only for a loopback test server).");
}

}  // namespace strata
