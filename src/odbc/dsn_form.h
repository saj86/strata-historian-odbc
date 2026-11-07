// The data source settings window's logic, kept free of Windows UI code so it can be tested anywhere.
#pragma once
#include <functional>
#include <utility>

#include "config.h"

namespace strata {

struct DsnForm {
    std::string name, description;
    std::string backend = "piwebapi";  // piwebapi | fixture
    std::string baseUrl, dataServer;
    std::string auth = "Negotiate";    // Negotiate | Basic | Anonymous
    int maxWindowDays = 31;
    int pageSize = 1000;
    bool navigatorPreview = false;
    std::string traceFile;
};

// Keys written to the DSN, in order. An empty value means the key is removed. Never UID or PWD.
std::vector<std::pair<std::string, std::string>> dsnAttributes(const DsnForm& f);

// Empty when the form can be saved, otherwise a message for the person filling it in.
std::string validateDsnForm(const DsnForm& f);

DsnForm dsnFormFromLookup(const std::string& name, const std::function<std::string(const std::string& key)>& get);

// Settings for the Test button: the form plus credentials that are used once and never saved.
KeyValues dsnTestConnection(const DsnForm& f, const std::string& user, const std::string& password);

}  // namespace strata
