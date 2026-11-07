#include "json.h"

#include <cstdlib>
#include <stdexcept>

namespace strata::json {
namespace {
class P {
public:
    explicit P(std::string_view t) : t_(t) {}
    Node doc() {
        Node n = value(0);
        ws();
        if (i_ != t_.size()) fail("trailing data");
        return n;
    }

private:
    [[noreturn]] void fail(const char* m) { throw std::runtime_error(std::string("JSON: ") + m + " at offset " + std::to_string(i_)); }
    void ws() { while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\n' || t_[i_] == '\r' || t_[i_] == '\t')) i_++; }
    char peek() { if (i_ >= t_.size()) fail("unexpected end"); return t_[i_]; }
    bool lit(std::string_view w) {
        if (t_.substr(i_, w.size()) == w) { i_ += w.size(); return true; }
        return false;
    }
    Node value(int depth) {
        if (depth > 64) fail("nesting too deep");
        ws();
        Node n;
        char c = peek();
        if (c == '{') {
            i_++;
            n.kind = Node::Object;
            ws();
            if (peek() == '}') { i_++; return n; }
            for (;;) {
                ws();
                if (peek() != '"') fail("expected key");
                std::string k = str();
                ws();
                if (peek() != ':') fail("expected ':'");
                i_++;
                n.o[k] = value(depth + 1);
                ws();
                if (peek() == ',') { i_++; continue; }
                if (peek() == '}') { i_++; return n; }
                fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            i_++;
            n.kind = Node::Array;
            ws();
            if (peek() == ']') { i_++; return n; }
            for (;;) {
                n.a.push_back(value(depth + 1));
                ws();
                if (peek() == ',') { i_++; continue; }
                if (peek() == ']') { i_++; return n; }
                fail("expected ',' or ']'");
            }
        }
        if (c == '"') { n.kind = Node::String; n.s = str(); return n; }
        if (lit("true")) { n.kind = Node::Bool; n.b = true; return n; }
        if (lit("false")) { n.kind = Node::Bool; return n; }
        if (lit("null")) return n;
        size_t st = i_;
        if (peek() == '-') i_++;
        while (i_ < t_.size() && ((t_[i_] >= '0' && t_[i_] <= '9') || t_[i_] == '.' || t_[i_] == 'e' || t_[i_] == 'E' || t_[i_] == '+' || t_[i_] == '-')) i_++;
        if (st == i_) fail("unexpected character");
        std::string num(t_.substr(st, i_ - st));
        char* end = nullptr;
        n.kind = Node::Number;
        n.n = std::strtod(num.c_str(), &end);
        if (end != num.c_str() + num.size()) fail("bad number");
        return n;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | cp >> 6); o += static_cast<char>(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += static_cast<char>(0xE0 | cp >> 12); o += static_cast<char>(0x80 | (cp >> 6 & 63)); o += static_cast<char>(0x80 | (cp & 63)); }
        else { o += static_cast<char>(0xF0 | cp >> 18); o += static_cast<char>(0x80 | (cp >> 12 & 63)); o += static_cast<char>(0x80 | (cp >> 6 & 63)); o += static_cast<char>(0x80 | (cp & 63)); }
    }
    unsigned hex4() {
        if (i_ + 4 > t_.size()) fail("bad \\u escape");
        unsigned v = 0;
        for (int k = 0; k < 4; k++) {
            char c = t_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else fail("bad \\u escape");
        }
        return v;
    }
    std::string str() {
        i_++;
        std::string o;
        for (;;) {
            char c = peek();
            i_++;
            if (c == '"') return o;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') { o += c; continue; }
            char e = peek();
            i_++;
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        if (!lit("\\u")) fail("lone surrogate");
                        unsigned lo = hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) fail("bad surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) fail("lone surrogate");
                    utf8(o, cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
    }
    std::string_view t_;
    size_t i_ = 0;
};
}  // namespace

Node parse(std::string_view text) { return P(text).doc(); }

}  // namespace strata::json
