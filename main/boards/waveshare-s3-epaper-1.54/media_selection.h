#pragma once

#include <functional>
#include <string>
#include <vector>

#include "music/music_index.h"

namespace media_selection {

inline constexpr size_t visible_count = 3;

struct Candidate {
    std::string path;
    bool book;
};

struct Matches {
    music_title_result_t status;
    std::vector<Candidate> candidates;
};

Matches Find(music_index_t& index, uint32_t first, uint32_t end, bool story, const std::string& title,
             const std::vector<std::string>& keywords, const std::function<bool()>& keep_searching);
std::string ChoiceKeyword(std::string choice);
size_t ChoiceNumber(const std::string& choice);
std::string Prompt(const std::vector<Candidate>& candidates, bool story, bool retry);

} // namespace media_selection
