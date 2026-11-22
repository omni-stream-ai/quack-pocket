#pragma once

#include <mutex>
#include <string>
#include <vector>
#include <nvs.h>

namespace pocket_list {
struct Item { std::string text; bool checked; };
using List = std::vector<Item>;
inline std::mutex lock;

// One blob keeps the item labels and their checkmarks together across resets.
inline esp_err_t Read(List& list) {
    nvs_handle_t handle;
    list.clear();
    auto err = nvs_open_from_partition("omni_ui", "pocket_list", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    size_t length = 0;
    err = nvs_get_blob(handle, "items", nullptr, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); return ESP_OK; }
    if (length > 3000) err = ESP_ERR_INVALID_SIZE;
    std::string data(length <= 3000 ? length : 0, '\0');
    if (err == ESP_OK && length) err = nvs_get_blob(handle, "items", data.data(), &length);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    for (size_t pos = 0; pos < length;) {
        auto end = data.find('\n', pos);
        if (end == std::string::npos || end <= pos + 1) return ESP_ERR_INVALID_STATE;
        list.push_back({data.substr(pos + 1, end-pos-1), data[pos] == '1'});
        pos = end + 1;
    }
    return ESP_OK;
}
inline esp_err_t Write(const List& list) {
    std::string data;
    for (auto& item : list) data += (item.checked ? "1" : "0") + item.text + "\n";
    nvs_handle_t handle;
    auto err = nvs_open_from_partition("omni_ui", "pocket_list", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if (data.empty()) {
        err = nvs_erase_key(handle, "items");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else err = nvs_set_blob(handle, "items", data.data(), data.size());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
}
