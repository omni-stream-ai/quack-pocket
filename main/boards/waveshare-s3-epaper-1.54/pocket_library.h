#pragma once

#include <cJSON.h>
#include <optional>
#include <string>

namespace pocket_library {
struct Result {
    bool ok;
    std::string body;
    bool json = false;
};

bool AddProfile(cJSON* profile);
std::optional<Result> Handle(const cJSON* request);
}
