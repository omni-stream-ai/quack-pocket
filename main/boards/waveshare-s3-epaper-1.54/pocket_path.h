#pragma once

#include <string>
#include <string_view>

// Store UTF-8 paths relative to the card. The empty path denotes its root.
inline bool ValidPocketPath(const std::string& path) {
    if (path.empty()) return true;
    if (path.size() >= 1024 || path.back() == '/') return false;
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        if (end == start || end - start > 255 || path[end - 1] == '.' || path[end - 1] == ' ') return false;
        for (size_t i = start; i < end; ++i) {
            unsigned char c = path[i];
            if (c < 32 || c == 127 || std::string_view("\\:*?\"<>|").find(c) != std::string_view::npos) return false;
        }
        start = end + 1;
    }
    return true;
}
