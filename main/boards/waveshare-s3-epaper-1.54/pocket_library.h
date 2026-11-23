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

Result Playlist(const std::string& operation, const std::string& name, std::string track, bool file_selected = false);
bool AddProfile(cJSON* profile);
std::optional<Result> Handle(const cJSON* request);
}
