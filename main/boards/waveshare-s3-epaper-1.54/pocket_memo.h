#pragma once

#include <functional>
#include <string>

bool PocketMemoUnread();
std::string RecordPocketMemo(const std::function<bool()>& stop);
std::string PlayPocketMemo(const std::function<bool()>& stop);
