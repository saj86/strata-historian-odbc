// Small strict JSON reader for PI Web API responses (no third-party dependency).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace strata::json {

struct Node {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Node> a;
    std::map<std::string, Node> o;
    bool isNull() const { return kind == Null; }
    const Node* find(const std::string& key) const {
        if (kind != Object) return nullptr;
        auto it = o.find(key);
        return it == o.end() ? nullptr : &it->second;
    }
};

// Throws std::runtime_error on malformed input.
Node parse(std::string_view text);

}  // namespace strata::json
