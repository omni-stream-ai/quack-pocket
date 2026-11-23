#pragma once

#include <nvs.h>
#include <nvs_flash.h>

namespace pocket_reset {

inline esp_err_t Mark(const char* partition) {
    nvs_handle_t db;
    auto err = nvs_open_from_partition(partition, "factory_reset", NVS_READWRITE, &db);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(db, "pending", 1);
    if (err == ESP_OK) err = nvs_commit(db);
    nvs_close(db);
    return err;
}

inline esp_err_t Pending(const char* partition, bool& pending) {
    pending = false;
    nvs_handle_t db;
    auto err = nvs_open_from_partition(partition, "factory_reset", NVS_READONLY, &db);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t value = 0;
    err = nvs_get_u8(db, "pending", &value);
    nvs_close(db);
    pending = err == ESP_OK && value == 1;
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

// Called before any device services start. Keep a committed marker in the
// opposite partition until the partition being erased is empty.
inline esp_err_t FinishAtBoot() {
    bool requested;
    auto err = Pending("nvs", requested);
    if (err != ESP_OK) return err;
    if (requested) {
        err = nvs_flash_erase_partition("omni_ui");
        if (err != ESP_OK) return err;
    }
    err = nvs_flash_init_partition("omni_ui");
    if (err != ESP_OK) return err;
    if (requested) {
        err = Mark("omni_ui");
        if (err != ESP_OK) return err;
    } else {
        err = Pending("omni_ui", requested);
        if (err != ESP_OK || !requested) return err;
    }
    err = nvs_flash_erase_partition("nvs");
    if (err != ESP_OK) return err;
    err = nvs_flash_init();
    if (err != ESP_OK) return err;
    nvs_handle_t db;
    err = nvs_open_from_partition("omni_ui", "factory_reset", NVS_READWRITE, &db);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(db, "pending");
    if (err == ESP_OK) err = nvs_commit(db);
    nvs_close(db);
    return err;
}

} // namespace pocket_reset
