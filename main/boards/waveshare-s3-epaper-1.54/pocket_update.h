#pragma once

#include <array>
#include <functional>
#include <string>

namespace pocket_update {
struct Release {
    std::string version;
    std::string image_url;
    int bytes = 0;
    std::array<uint8_t, 32> digest{};
};
std::string Source();
bool ValidSource(const std::string& url);
// Empty error means a verified manifest. An empty release means already current.
std::string Check(Release& release);
std::string Install(const Release& release, const std::function<void(int)>& progress);
}
