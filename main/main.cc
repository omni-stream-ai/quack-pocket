#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "system_info.h"

#if CONFIG_BOARD_TYPE_WAVESHARE_S3_ePaper_1_54
#include "boards/waveshare-s3-epaper-1.54/config.h"
#endif
#define TAG "main"

extern "C" void app_main(void)
{
#if CONFIG_BOARD_TYPE_WAVESHARE_S3_ePaper_1_54
    // Assert the battery latch before the network and flash startup work.
    gpio_set_level(VBAT_PWR_PIN, 1);
    gpio_set_direction(VBAT_PWR_PIN, GPIO_MODE_OUTPUT);
#endif
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
}
