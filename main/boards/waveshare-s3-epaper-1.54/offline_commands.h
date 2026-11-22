#pragma once

#include <functional>
#include <string>

struct OfflineCommand {
    int id = -1;
    std::string song;
};

std::string ReadOfflineWords();
bool SaveOfflineWords(const std::string& words, std::string& error);
OfflineCommand ListenOffline(const std::function<bool()>& cancelled);
void InstallOfflineModel(const std::function<bool()>& cancelled);
