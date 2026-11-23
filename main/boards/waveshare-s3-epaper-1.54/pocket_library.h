#pragma once

#include <cJSON.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pocket_library {
struct Result {
    bool ok;
    std::string body;
    bool json = false;
};

Result Playlist(const std::string& operation, const std::string& name, std::string track, bool file_selected = false);
std::optional<std::string> StoryCatalog(const std::string* book, std::vector<std::string>& entries);
std::optional<std::string> MusicCatalog(uint32_t offset, uint32_t& total, std::vector<std::string>& entries);
inline constexpr uint32_t MusicPageSize = 64;
bool AddProfile(cJSON* profile);
std::optional<Result> Handle(const cJSON* request);
}
