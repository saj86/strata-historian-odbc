#include "dsn_form.h"

#include <cstdlib>

namespace strata {
namespace {
bool fixture(const DsnForm& f) { return iequals(f.backend, "fixture"); }

KeyValues asConnection(const DsnForm& f) {
    KeyValues kv;
    for (auto& [k, v] : dsnAttributes(f))
        if (!v.empty() && k != "Description") kv[toUpper(k)] = v;
    return kv;
}
}  // namespace

std::vector<std::pair<std::string, std::string>> dsnAttributes(const DsnForm& f) {
    const bool live = !fixture(f);
    return {
        {"Description", f.description},
        {"Backend", fixture(f) ? "fixture" : "piwebapi"},
        {"BaseUrl", live ? f.baseUrl : ""},
        {"DataServer", live ? f.dataServer : ""},
        {"Auth", live ? f.auth : ""},
        {"MaxWindowDays", std::to_string(f.maxWindowDays)},
        {"PageSize", std::to_string(f.pageSize)},
        {"NavigatorPreview", f.navigatorPreview ? "1" : "0"},
        {"TraceFile", f.traceFile},
    };
}

std::string validateDsnForm(const DsnForm& f) {
    if (f.name.empty()) return "Enter a data source name.";
    if (f.name.size() > 32) return "The data source name can be at most 32 characters.";
    if (f.name.find_first_of("[]{}(),;?*=!@\\") != std::string::npos) return "The data source name can't contain any of these characters: [ ] { } ( ) , ; ? * = ! @ \\";
    if (!fixture(f)) {
        if (f.baseUrl.empty()) return "Enter the PI Web API URL, for example https://piweb.example.local/piwebapi.";
        if (f.dataServer.empty()) return "Enter the PI data server name.";
    }
    // Reuse the driver's own rules, so the window accepts exactly what a connection accepts.
    KeyValues kv = asConnection(f);
    if (iequals(f.auth, "Basic") && !fixture(f)) {
        kv["UID"] = "placeholder";  // credentials come at connect time; only the rest is checked here
        kv["PWD"] = "placeholder";
    }
    try {
        Config c = resolveConfig(kv, nullptr);
        if (!fixture(f)) validateBaseUrl(c.baseUrl);
    } catch (const Error& e) {
        return e.what();
    }
    return {};
}

DsnForm dsnFormFromLookup(const std::string& name, const std::function<std::string(const std::string&)>& get) {
    DsnForm f;
    f.name = name;
    f.description = get("Description");
    if (iequals(get("Backend"), "fixture")) f.backend = "fixture";
    f.baseUrl = get("BaseUrl");
    f.dataServer = get("DataServer");
    std::string auth = get("Auth");
    if (iequals(auth, "Basic")) f.auth = "Basic";
    else if (iequals(auth, "Anonymous")) f.auth = "Anonymous";
    auto number = [&](const char* key, int fallback) {
        std::string v = get(key);
        char* end = nullptr;
        long n = std::strtol(v.c_str(), &end, 10);
        return v.empty() || *end != 0 ? fallback : static_cast<int>(n);
    };
    f.maxWindowDays = number("MaxWindowDays", f.maxWindowDays);
    f.pageSize = number("PageSize", f.pageSize);
    std::string preview = get("NavigatorPreview");
    f.navigatorPreview = preview == "1" || iequals(preview, "yes") || iequals(preview, "true");
    f.traceFile = get("TraceFile");
    return f;
}

KeyValues dsnTestConnection(const DsnForm& f, const std::string& user, const std::string& password) {
    KeyValues kv = asConnection(f);
    if (!user.empty()) kv["UID"] = user;
    if (!password.empty()) kv["PWD"] = password;
    kv["REQUESTTIMEOUTMS"] = "15000";
    kv["MAXRETRIES"] = "0";
    return kv;
}

}  // namespace strata
