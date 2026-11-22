#pragma once

#include <functional>
#include <string>

// Writes a sorted card-local index in .omni-music/catalog-v2. Single writer per card.
bool BuildMusicIndex(const std::string& root, const std::function<bool()>& keep_scanning, int fat_drive = -1);
