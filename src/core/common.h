// Shared value model. Everything above the ODBC layer is UTF-8 and platform neutral.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace strata {

// An error that maps directly onto an ODBC diagnostic record.
struct Error : std::runtime_error {
    std::string sqlstate;
    int native;
    Error(std::string state, const std::string& message, int nativeCode = 0)
        : std::runtime_error(message), sqlstate(std::move(state)), native(nativeCode) {}
};

// 100 ns ticks since 1970-01-01T00:00:00Z. The source's own text is kept separately
// (EventUtcRaw) so no precision is lost when a source is finer than this.
struct Ticks {
    int64_t v = 0;
    auto operator<=>(const Ticks&) const = default;
};

enum class ColType { Text, Int16, Int32, Double, Bit, Timestamp };  // Int16: catalog result sets only

using Value = std::variant<std::monostate, std::string, int32_t, double, bool, Ticks>;
using Row = std::vector<Value>;

inline bool isNull(const Value& v) { return std::holds_alternative<std::monostate>(v); }

struct ColumnDef {
    std::string name;
    ColType type;
    int size;       // characters for text, display size otherwise
    bool nullable;
    std::string description;
};

// Time helpers (see src/core/timeutil.cpp). Text without an offset is read as UTC.
bool parseUtc(std::string_view text, Ticks& out);
std::string formatUtc(Ticks t);  // 2026-01-15T08:00:10.0000153Z
struct CivilTime { int year, month, day, hour, minute, second, ticks100ns; };
CivilTime toCivil(Ticks t);
Ticks fromCivil(int y, int mo, int d, int h, int mi, int s, int64_t ticks100ns);
Ticks nowUtc();

inline constexpr int64_t kTicksPerSecond = 10'000'000;
inline constexpr int64_t kTicksPerHour = 3600 * kTicksPerSecond;
inline constexpr int64_t kTicksPerDay = 24 * kTicksPerHour;

bool iequals(std::string_view a, std::string_view b);
std::string toUpper(std::string_view s);

// Cooperative cancellation plus a deadline. A transport registers onCancel so that
// SQLCancel can abort blocking network I/O from another thread.
class CancelToken {
public:
    void reset() {
        std::lock_guard<std::mutex> g(m_);
        cancelled_ = false;
        onCancel_ = nullptr;
        hasDeadline_ = false;
    }
    void setTimeoutSeconds(unsigned long seconds) {
        std::lock_guard<std::mutex> g(m_);
        hasDeadline_ = seconds > 0;
        if (hasDeadline_) deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    }
    void cancel() {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> g(m_);
            cancelled_ = true;
            cb = onCancel_;
        }
        if (cb) cb();
    }
    void setOnCancel(std::function<void()> cb) {
        std::lock_guard<std::mutex> g(m_);
        onCancel_ = std::move(cb);
    }
    void clearOnCancel() { setOnCancel(nullptr); }
    bool cancelled() const { return cancelled_.load(); }
    // Remaining milliseconds, or -1 when no deadline is set.
    long long remainingMs() const {
        std::lock_guard<std::mutex> g(m_);
        if (!hasDeadline_) return -1;
        auto d = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - std::chrono::steady_clock::now()).count();
        return d < 0 ? 0 : d;
    }
    void check() const {
        if (cancelled()) throw Error("HY008", "Operation canceled.");
        if (remainingMs() == 0) throw Error("HYT00", "Query timeout expired.");
    }
    // Cancel-aware sleep used for page pacing and retry backoff.
    void sleepMs(long long ms) const;

private:
    mutable std::mutex m_;
    std::atomic<bool> cancelled_{false};
    std::function<void()> onCancel_;
    bool hasDeadline_ = false;
    std::chrono::steady_clock::time_point deadline_{};
};

// Append-only JSON-lines trace. Callers never pass credentials; see docs/security.md.
class Trace {
public:
    explicit Trace(std::string path = {}) : path_(std::move(path)) {}
    bool enabled() const { return !path_.empty(); }
    // fields are already-encoded "key":value fragments.
    void write(const std::string& correlationId, const std::string& event, const std::string& fields) const;
    static std::string quote(std::string_view s);

private:
    std::string path_;
};

}  // namespace strata
