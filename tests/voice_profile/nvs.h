#pragma once
#include <stddef.h>
using esp_err_t = int;
using nvs_handle_t = int;
enum { ESP_OK, ESP_FAIL, ESP_ERR_NVS_NOT_FOUND, ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_RESPONSE };
enum { NVS_READONLY, NVS_READWRITE };
int nvs_open(const char*, int, nvs_handle_t*);
int nvs_get_str(nvs_handle_t, const char*, char*, size_t*);
int nvs_set_str(nvs_handle_t, const char*, const char*);
int nvs_erase_key(nvs_handle_t, const char*);
int nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
