#include "music_sd.h"

#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <esp_attr.h>
#include <esp_memory_utils.h>
#include <stdbool.h>
#include <esp_log.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <diskio_sdmmc.h>

static sdmmc_card_t *card;

// FATFS buffers can live in PSRAM. Reserve one DMA sector so a card access
// during Wi-Fi or WakeNet initialization never needs a temporary heap block.
static DMA_ATTR uint8_t dma_sector[512];
static pthread_mutex_t sector_lock = PTHREAD_MUTEX_INITIALIZER;

esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t*, void*, size_t, size_t);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t*, const void*, size_t, size_t);

static bool needs_staging(sdmmc_card_t *device, const void *data, size_t count)
{
    return count && device->csd.sector_size == sizeof(dma_sector) &&
        (!esp_ptr_dma_capable(data) ||
         !device->host.check_buffer_alignment(device->host.slot, data, count * sizeof(dma_sector)));
}

esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *device, void *dst, size_t first, size_t count)
{
    if (!needs_staging(device, dst, count))
        return __real_sdmmc_read_sectors(device, dst, first, count);

    esp_err_t result = ESP_OK;
    pthread_mutex_lock(&sector_lock);
    for (size_t i = 0; i < count; ++i) {
        result = __real_sdmmc_read_sectors(device, dma_sector, first + i, 1);
        if (result != ESP_OK) break;
        memcpy((uint8_t *)dst + i * sizeof(dma_sector), dma_sector, sizeof(dma_sector));
    }
    pthread_mutex_unlock(&sector_lock);
    return result;
}

esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t *device, const void *src, size_t first, size_t count)
{
    if (!needs_staging(device, src, count))
        return __real_sdmmc_write_sectors(device, src, first, count);

    esp_err_t result = ESP_OK;
    pthread_mutex_lock(&sector_lock);
    for (size_t i = 0; i < count; ++i) {
        memcpy(dma_sector, (const uint8_t *)src + i * sizeof(dma_sector), sizeof(dma_sector));
        result = __real_sdmmc_write_sectors(device, dma_sector, first + i, 1);
        if (result != ESP_OK) break;
    }
    pthread_mutex_unlock(&sector_lock);
    return result;
}

static esp_err_t mount_locked(void)
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



static esp_err_t unmount_locked(void)
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

// The player keeps its mount across commands. A phone request can also hold
// the same mount while it reads a directory, even when playback stops.
static pthread_mutex_t mount_lock = PTHREAD_MUTEX_INITIALIZER;
static bool player_mounted;
static unsigned visitors;

static bool stopping;
static pthread_cond_t released = PTHREAD_COND_INITIALIZER;

int music_sd_drive(void)
{
    pthread_mutex_lock(&mount_lock);
    int drive = card ? ff_diskio_get_pdrv_card(card) : -1;
    pthread_mutex_unlock(&mount_lock);
    return drive;
}

void music_sd_shutdown(void)
{
    pthread_mutex_lock(&mount_lock);
    stopping = true;
    while (visitors) pthread_cond_wait(&released, &mount_lock);
    unmount_locked();
    pthread_mutex_unlock(&mount_lock);
}

esp_err_t music_sd_mount(void)
{
    pthread_mutex_lock(&mount_lock);
    esp_err_t result = mount_locked();
    if (result == ESP_OK) player_mounted = true;
    pthread_mutex_unlock(&mount_lock);
    return result;
}

esp_err_t music_sd_unmount(void)
{
    pthread_mutex_lock(&mount_lock);
    player_mounted = false;
    esp_err_t result = visitors ? ESP_OK : unmount_locked();
    pthread_mutex_unlock(&mount_lock);
    return result;
}

esp_err_t music_sd_acquire(void)
{
    pthread_mutex_lock(&mount_lock);
    esp_err_t result = stopping ? ESP_ERR_INVALID_STATE : mount_locked();
    if (result == ESP_OK) ++visitors;
    pthread_mutex_unlock(&mount_lock);
    return result;
}

void music_sd_release(void)
{
    pthread_mutex_lock(&mount_lock);
    if (visitors && --visitors == 0) {
        if (!player_mounted) unmount_locked();
        pthread_cond_broadcast(&released);
    }
    pthread_mutex_unlock(&mount_lock);
}
