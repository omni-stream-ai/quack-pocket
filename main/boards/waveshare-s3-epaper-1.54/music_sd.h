#pragma once

#include <esp_err.h>

#define MUSIC_SD_ROOT "/sdcard"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount, use and unmount the card from the same storage task. */
esp_err_t music_sd_mount(void);
int music_sd_drive(void);
esp_err_t music_sd_unmount(void);

#ifdef __cplusplus
}
#endif
