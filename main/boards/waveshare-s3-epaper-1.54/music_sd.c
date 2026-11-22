#include "music_sd.h"

#include <inttypes.h>
#include <esp_log.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <diskio_sdmmc.h>

static sdmmc_card_t *card;

esp_err_t music_sd_mount(void)
{
    if (card) {
        return ESP_OK;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_39;
    slot.cmd = GPIO_NUM_41;
    slot.d0 = GPIO_NUM_40;
    const esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files = 8,
    };
    esp_err_t result = esp_vfs_fat_sdmmc_mount(MUSIC_SD_ROOT, &host, &slot, &config, &card);
    if (result == ESP_ERR_INVALID_CRC || result == ESP_ERR_TIMEOUT) {
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        result = esp_vfs_fat_sdmmc_mount(MUSIC_SD_ROOT, &host, &slot, &config, &card);
    }
    if (result == ESP_OK) {
        uint64_t bytes = (uint64_t)(uint32_t)card->csd.capacity * card->csd.sector_size;
        ESP_LOGI("music_sd", "Mounted card (%" PRIu32 " MiB)", (uint32_t)(bytes >> 20));
    }
    return result;
}

int music_sd_drive(void) { return card ? ff_diskio_get_pdrv_card(card) : -1; }

esp_err_t music_sd_unmount(void)
{
    if (!card) {
        return ESP_OK;
    }
    esp_err_t result = esp_vfs_fat_sdcard_unmount(MUSIC_SD_ROOT, card);
    // ESP-IDF releases the card before unregistering the VFS path. An error
    // from the latter must not leave a stale card handle for the next mount.
    card = NULL;
    if (result != ESP_OK) ESP_LOGE("music_sd", "Unmount failed: %s", esp_err_to_name(result));
    return result;
}
