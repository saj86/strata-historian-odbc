// UTF-8 <-> UTF-16 helpers. SQLWCHAR is 16 bits on Windows and with default unixODBC builds.
#pragma once
#include <string>
#include <string_view>

#ifdef _WIN32
#define STRATA_EXPORT
#else
#define STRATA_EXPORT __attribute__((visibility("default")))
#endif

namespace strata {

inline std::u16string utf8ToUtf16(std::string_view s) {
    std::u16string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
        else { o.push_back(0xFFFD); i++; continue; }
        if (i + static_cast<size_t>(n) > s.size()) { o.push_back(0xFFFD); break; }
        for (int k = 1; k < n; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
        i += static_cast<size_t>(n);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            o.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            o.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            o.push_back(static_cast<char16_t>(cp));
        }
    }
    return o;
}

// len < 0 means NUL-terminated (SQL_NTS is -3; any negative is treated alike).
inline std::string utf16ToUtf8(const char16_t* w, long len) {
    std::string o;
    if (!w) return o;
    if (len < 0) { len = 0; while (w[len]) len++; }
    for (long i = 0; i < len; i++) {
        unsigned cp = w[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < len && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | cp >> 6); o += static_cast<char>(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += static_cast<char>(0xE0 | cp >> 12); o += static_cast<char>(0x80 | (cp >> 6 & 63)); o += static_cast<char>(0x80 | (cp & 63)); }
        else { o += static_cast<char>(0xF0 | cp >> 18); o += static_cast<char>(0x80 | (cp >> 12 & 63)); o += static_cast<char>(0x80 | (cp >> 6 & 63)); o += static_cast<char>(0x80 | (cp & 63)); }
    }
    return o;
}

}  // namespace strata
