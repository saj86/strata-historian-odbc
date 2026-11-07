#include "common.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <thread>

namespace strata {
namespace {
// Howard Hinnant's civil-date algorithms (public domain).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
void civilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(static_cast<int64_t>(yoe) + era * 400 + (m <= 2));
}
bool digits(std::string_view s, size_t pos, size_t n, int& out) {
    if (pos + n > s.size()) return false;
    int v = 0;
    for (size_t i = 0; i < n; i++) {
        if (!std::isdigit(static_cast<unsigned char>(s[pos + i]))) return false;
        v = v * 10 + (s[pos + i] - '0');
    }
    out = v;
    return true;
}
int daysInMonth(int y, int m) {
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : dim[m - 1];
}
}  // namespace

Ticks fromCivil(int y, int mo, int d, int h, int mi, int s, int64_t t) {
    return Ticks{(daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + h * 3600 + mi * 60 + s) * kTicksPerSecond + t};
}

CivilTime toCivil(Ticks t) {
    int64_t secs = t.v / kTicksPerSecond;
    int64_t frac = t.v % kTicksPerSecond;
    if (frac < 0) { frac += kTicksPerSecond; secs--; }
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    if (rem < 0) { rem += 86400; days--; }
    CivilTime c{};
    civilFromDays(days, c.year, c.month, c.day);
    c.hour = static_cast<int>(rem / 3600);
    c.minute = static_cast<int>(rem % 3600 / 60);
    c.second = static_cast<int>(rem % 60);
    c.ticks100ns = static_cast<int>(frac);
    return c;
}

bool parseUtc(std::string_view s, Ticks& out) {
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (!digits(s, 0, 4, y) || s.size() < 10 || s[4] != '-' || !digits(s, 5, 2, mo) || s[7] != '-' || !digits(s, 8, 2, d)) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > daysInMonth(y, mo)) return false;
    size_t p = 10;
    int64_t frac = 0, offsetSec = 0;
    if (p < s.size() && (s[p] == 'T' || s[p] == ' ')) {
        if (!digits(s, p + 1, 2, h) || s.size() <= p + 3 || s[p + 3] != ':' || !digits(s, p + 4, 2, mi)) return false;
        p += 6;
        if (p < s.size() && s[p] == ':') {
            if (!digits(s, p + 1, 2, sec)) return false;
            p += 3;
        }
        if (h > 23 || mi > 59 || sec > 59) return false;
        if (p < s.size() && s[p] == '.') {
            p++;
            int count = 0;
            while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
                if (count < 7) { frac = frac * 10 + (s[p] - '0'); count++; }
                p++;  // digits past 100 ns are truncated here; EventUtcRaw keeps them
            }
            if (count == 0) return false;
            for (; count < 7; count++) frac *= 10;
        }
        if (p < s.size() && (s[p] == 'Z' || s[p] == 'z')) {
            p++;
        } else if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
            int oh, om;
            if (!digits(s, p + 1, 2, oh) || s.size() <= p + 3 || s[p + 3] != ':' || !digits(s, p + 4, 2, om)) return false;
            offsetSec = (oh * 3600 + om * 60) * (s[p] == '-' ? -1 : 1);
            p += 6;
        }
    }
    if (p != s.size()) return false;
    out = fromCivil(y, mo, d, h, mi, sec, frac);
    out.v -= offsetSec * kTicksPerSecond;
    return true;
}

std::string formatUtc(Ticks t) {
    CivilTime c = toCivil(t);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%07dZ", c.year, c.month, c.day, c.hour, c.minute, c.second, c.ticks100ns);
    return buf;
}

Ticks nowUtc() {
    using namespace std::chrono;
    return Ticks{duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() * 10};
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

std::string toUpper(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return r;
}

void CancelToken::sleepMs(long long ms) const {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        check();
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<long long>(left > 0 ? left : 0, 20)));
    }
    check();
}

std::string Trace::quote(std::string_view s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); r += b; }
                else r += static_cast<char>(c);
        }
    }
    return r + "\"";
}

void Trace::write(const std::string& corr, const std::string& event, const std::string& fields) const {
    if (path_.empty()) return;
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    std::ofstream f(path_, std::ios::app | std::ios::binary);
    if (!f) return;  // tracing must never fail a query
    f << "{\"ts\":" << quote(formatUtc(nowUtc())) << ",\"corr\":" << quote(corr) << ",\"event\":" << quote(event);
    if (!fields.empty()) f << "," << fields;
    f << "}\n";
}

}  // namespace strata
