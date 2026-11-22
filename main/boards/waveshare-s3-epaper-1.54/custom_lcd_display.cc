#include "custom_lcd_display.h"

#include "lcd_display.h"

#include <vector>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include "assets/lang_config.h"
#include <cstring>
#include "settings.h"
#include <nvs_flash.h>

#include "esp_lcd_panel_io.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "config.h"

#include "board.h"
#include "application.h"
#include "voice/voice_profile.h"


#define TAG "CustomLcdDisplay"


#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))
#define BUFF_SIZE (EXAMPLE_LCD_WIDTH * EXAMPLE_LCD_HEIGHT * BYTES_PER_PIXEL)

const uint8_t WF_Full_1IN54[159] =
{											
    0x80,	0x48,	0x40,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x40,	0x48,	0x80,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x80,	0x48,	0x40,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x40,	0x48,	0x80,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0xA,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x8,	0x1,	0x0,	0x8,	0x1,	0x0,	0x2,					
    0xA,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x22,	0x22,	0x22,	0x22,	0x22,	0x22,	0x0,	0x0,	0x0,			
    0x22,	0x17,	0x41,	0x0,	0x32,	0x20
};

uint8_t WF_PARTIAL_1IN54_0[159] =
{
    0x0,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x80,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x40,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0xF,0x0,0x0,0x0,0x0,0x0,0x0,
    0x1,0x1,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x22,0x22,0x22,0x22,0x22,0x22,0x0,0x0,0x0,
    0x02,0x17,0x41,0xB0,0x32,0x28,
};

void CustomLcdDisplay::lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    assert(disp != NULL);
    CustomLcdDisplay *driver = (CustomLcdDisplay *)lv_display_get_user_data(disp);
    auto *pixels = reinterpret_cast<const uint16_t*>(color_p);
    bool changed = false;
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            uint16_t rgb = *pixels++;
            unsigned luminance = 299 * ((rgb >> 11) * 255 / 31) +
                                 587 * (((rgb >> 5) & 63) * 255 / 63) +
                                 114 * ((rgb & 31) * 255 / 31);
            changed |= driver->EPD_DrawColorPixel(x, y, luminance < 128000 ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE);
        }
    }
    if (changed && ++driver->partial_updates_ >= 30) {
        driver->EPD_Init();
        driver->EPD_DisplayPartBaseImage();
        driver->EPD_Init_Partial();
        driver->partial_updates_ = 0;
    } else if (changed) {
        driver->EPD_DisplayPart();
    }
    lv_disp_flush_ready(disp);
}

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy,custom_lcd_spi_t _lcd_spi_data)
    : LcdDisplay(panel_io, panel, width, height),
    lcd_spi_data(_lcd_spi_data),
    Width(width),
    Height(height) {

    ESP_LOGI(TAG, "Initialize SPI");
	spi_port_init();
	spi_gpio_init();

	ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    buffer = (uint8_t *)heap_caps_malloc(lcd_spi_data.buffer_len, MALLOC_CAP_SPIRAM);
	assert(buffer);
    display_ = lv_display_create(width, height); /* 以水平和垂直分辨率（像素）进行基本初始化 */
    lv_display_set_flush_cb(display_, lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
    
    uint8_t *buffer_1 = NULL;
	buffer_1 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM);
	assert(buffer_1);
    lv_display_set_buffers(display_, buffer_1, NULL, BUFF_SIZE, LV_DISPLAY_RENDER_MODE_FULL);

	ESP_LOGI(TAG, "EPD init");
	EPD_Init();
    EPD_Clear();
    EPD_Display();
    EPD_DisplayPartBaseImage();
    EPD_Init_Partial();            //局部刷新初始化

    lvgl_port_unlock();
	if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    ESP_LOGI(TAG, "ui start");

    SetupUI();
    if (nvs_flash_init_partition("omni_ui") == ESP_OK && Lock(3000)) {
        std::memset(saver_pixels_, 255, 4);
        saver_pixels_[7] = 255;
        saver_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
        saver_image_.header.cf = LV_COLOR_FORMAT_I1;
        saver_image_.header.w = saver_image_.header.h = 200;
        saver_image_.header.stride = 25;
        saver_image_.data_size = sizeof(saver_pixels_);
        saver_image_.data = saver_pixels_;
        saver_available_ = ReadSaver(saver_pixels_ + 8);
        last_activity_ = lv_tick_get();
        lv_timer_create([](lv_timer_t* t) {
            static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(t))->UpdateSaver();
        }, 1000, this);
        Unlock();
    }
}

CustomLcdDisplay::~CustomLcdDisplay() {

}

void CustomLcdDisplay::spi_gpio_init() {
    int rst = lcd_spi_data.rst;
    int cs = lcd_spi_data.cs;
    int dc = lcd_spi_data.dc;
    int busy = lcd_spi_data.busy;

    gpio_config_t gpio_conf = {};
	gpio_conf.intr_type = GPIO_INTR_DISABLE;
	gpio_conf.mode = GPIO_MODE_OUTPUT;
	gpio_conf.pin_bit_mask = (0x1ULL<<rst) | (0x1ULL<<dc) | (0x1ULL<<cs);
	gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
	gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

	gpio_conf.mode = GPIO_MODE_INPUT;
	gpio_conf.pin_bit_mask = (0x1ULL<<busy);
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    set_rst_1();
}

void CustomLcdDisplay::spi_port_init() {
    int mosi = lcd_spi_data.mosi;
    int scl = lcd_spi_data.scl;
    int spi_host = lcd_spi_data.spi_host;
    esp_err_t ret;
  	spi_bus_config_t buscfg = {};
  	  	buscfg.miso_io_num = -1;
  	  	buscfg.mosi_io_num = mosi;
  	  	buscfg.sclk_io_num = scl;
  	  	buscfg.quadwp_io_num = -1;
  	  	buscfg.quadhd_io_num = -1;
  	  	buscfg.max_transfer_sz = Width * Height;

  	spi_device_interface_config_t devcfg = {};
  	  	devcfg.spics_io_num = -1;
  	  	devcfg.clock_speed_hz = 40 * 1000 * 1000;  //Clock out at 10 MHz
  	  	devcfg.mode = 0;                           //SPI mode 0
  	  	devcfg.queue_size = 7;                     //We want to be able to queue 7 transactions at a time

  	ret = spi_bus_initialize((spi_host_device_t)spi_host, &buscfg, SPI_DMA_CH_AUTO);
  	ESP_ERROR_CHECK(ret);
  	ret = spi_bus_add_device((spi_host_device_t)spi_host, &devcfg, &spi);
  	ESP_ERROR_CHECK(ret);
}

void CustomLcdDisplay::read_busy() {
    int busy = lcd_spi_data.busy;
    while(gpio_get_level((gpio_num_t)busy) == 1) {
        vTaskDelay(1);   // LOW: idle, HIGH: busy
    }
}

void CustomLcdDisplay::SPI_SendByte(uint8_t data) {
    esp_err_t ret;
  	spi_transaction_t t; 
  	memset(&t, 0, sizeof(t));
  	t.length = 8;      
  	t.tx_buffer = &data;
  	ret = spi_device_polling_transmit(spi, &t); //Transmit!
  	assert(ret == ESP_OK);                      //Should have had no issues.
}

void CustomLcdDisplay::EPD_SendData(uint8_t data) {
    set_dc_1();
  	set_cs_0();
  	SPI_SendByte(data);
  	set_cs_1();
}

void CustomLcdDisplay::EPD_SendCommand(uint8_t command) {
    set_dc_0();
  	set_cs_0();
  	SPI_SendByte(command);
  	set_cs_1();
}

void CustomLcdDisplay::writeBytes(uint8_t *buffer,int len) {
    set_dc_1();
  	set_cs_0();
  	esp_err_t ret;
  	spi_transaction_t t; 
  	memset(&t, 0, sizeof(t));
  	t.length = 8 * len;      
  	t.tx_buffer = buffer;
  	ret = spi_device_polling_transmit(spi, &t); //Transmit!
  	assert(ret == ESP_OK);
  	set_cs_1();
}

void CustomLcdDisplay::writeBytes(const uint8_t *buffer, int len) {
    set_dc_1();
  	set_cs_0();
  	esp_err_t ret;
  	spi_transaction_t t; 
  	memset(&t, 0, sizeof(t));
  	t.length = 8 * len;      
  	t.tx_buffer = buffer;
  	ret = spi_device_polling_transmit(spi, &t); //Transmit!
  	assert(ret == ESP_OK);
  	set_cs_1();
}

void CustomLcdDisplay::EPD_SetWindows(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend) {
    EPD_SendCommand(0x44);  // SET_RAM_X_ADDRESS_START_END_POSITION
    EPD_SendData((Xstart>>3) & 0xFF);
    EPD_SendData((Xend>>3) & 0xFF);
	
    EPD_SendCommand(0x45);  // SET_RAM_Y_ADDRESS_START_END_POSITION
    EPD_SendData(Ystart & 0xFF);
    EPD_SendData((Ystart >> 8) & 0xFF);
    EPD_SendData(Yend & 0xFF);
    EPD_SendData((Yend >> 8) & 0xFF);
}

void CustomLcdDisplay::EPD_SetCursor(uint16_t Xstart, uint16_t Ystart) {
    EPD_SendCommand(0x4E); // SET_RAM_X_ADDRESS_COUNTER
    EPD_SendData(Xstart & 0xFF);

    EPD_SendCommand(0x4F); // SET_RAM_Y_ADDRESS_COUNTER
    EPD_SendData(Ystart & 0xFF);
    EPD_SendData((Ystart >> 8) & 0xFF);
}

void CustomLcdDisplay::EPD_SetLut(const uint8_t *lut) {
	EPD_SendCommand(0x32);
    writeBytes(lut,153);
	read_busy();
	
    EPD_SendCommand(0x3f);
    EPD_SendData(lut[153]);
	
    EPD_SendCommand(0x03);
    EPD_SendData(lut[154]);
	
    EPD_SendCommand(0x04);
    EPD_SendData(lut[155]);
	EPD_SendData(lut[156]);
	EPD_SendData(lut[157]);

	EPD_SendCommand(0x2c);
    EPD_SendData(lut[158]);
}

void CustomLcdDisplay::EPD_TurnOnDisplay() {
    EPD_SendCommand(0x22);
    EPD_SendData(0xc7);
	EPD_SendCommand(0x20);
    read_busy();
}

void CustomLcdDisplay::EPD_TurnOnDisplayPart() {
    EPD_SendCommand(0x22);
    EPD_SendData(0xcf);
    EPD_SendCommand(0x20);
    read_busy();
}

void CustomLcdDisplay::EPD_Init() {
    set_rst_1();
  	vTaskDelay(pdMS_TO_TICKS(50));
  	set_rst_0();
  	vTaskDelay(pdMS_TO_TICKS(20));
  	set_rst_1();
  	vTaskDelay(pdMS_TO_TICKS(50));

    read_busy();
    EPD_SendCommand(0x12);  //SWRESET
    read_busy();

    EPD_SendCommand(0x01); //Driver output control
    EPD_SendData(0xC7);
    EPD_SendData(0x00);
    EPD_SendData(0x01);

    EPD_SendCommand(0x11); //data entry mode
    EPD_SendData(0x01);

	EPD_SetWindows(0, Width-1, Height-1, 0);

    EPD_SendCommand(0x3C); //BorderWavefrom
    EPD_SendData(0x01);

    EPD_SendCommand(0x18);
    EPD_SendData(0x80);

    EPD_SendCommand(0x22); //Load Temperature and waveform setting.
    EPD_SendData(0XB1);
    EPD_SendCommand(0x20);

    EPD_SetCursor(0, Height-1);
	read_busy();
	
	EPD_SetLut(WF_Full_1IN54);
}

void CustomLcdDisplay::EPD_Clear() {
    int buffer_len = lcd_spi_data.buffer_len;
    memset(buffer,0xff,buffer_len);
}

void CustomLcdDisplay::EPD_Display() {
    int buffer_len = lcd_spi_data.buffer_len;
    EPD_SendCommand(0x24);
    assert(buffer);
    writeBytes(buffer,buffer_len);
    EPD_TurnOnDisplay();
}

void CustomLcdDisplay::EPD_DisplayPartBaseImage() {
    int buffer_len = lcd_spi_data.buffer_len;
    EPD_SendCommand(0x24);
    assert(buffer);
    writeBytes(buffer,buffer_len);
    EPD_SendCommand(0x26);
    writeBytes(buffer,buffer_len);
    EPD_TurnOnDisplay();
}

void CustomLcdDisplay::EPD_Init_Partial() {
    set_rst_1();
  	vTaskDelay(pdMS_TO_TICKS(50));
  	set_rst_0();
  	vTaskDelay(pdMS_TO_TICKS(20));
  	set_rst_1();
  	vTaskDelay(pdMS_TO_TICKS(50));

	read_busy();
	
	EPD_SetLut(WF_PARTIAL_1IN54_0);

    EPD_SendCommand(0x37); 
    EPD_SendData(0x00);  
    EPD_SendData(0x00);  
    EPD_SendData(0x00);  
    EPD_SendData(0x00); 
    EPD_SendData(0x00);  	
    EPD_SendData(0x40);  
    EPD_SendData(0x00);  
    EPD_SendData(0x00);   
    EPD_SendData(0x00);  
    EPD_SendData(0x00);
	
    EPD_SendCommand(0x3C); //BorderWavefrom
    EPD_SendData(0x80);
	
	EPD_SendCommand(0x22); 
	EPD_SendData(0xc0); 
	EPD_SendCommand(0x20); 
	read_busy();
}

void CustomLcdDisplay::EPD_DisplayPart() {
    EPD_SendCommand(0x24);
    assert(buffer);
    writeBytes(buffer, lcd_spi_data.buffer_len);
    EPD_TurnOnDisplayPart();
}

bool CustomLcdDisplay::EPD_DrawColorPixel(uint16_t x, uint16_t y,uint8_t color) {
    if (x >= Width || y >= Height) {
        ESP_LOGE("EPD", "Out of bounds pixel: (%d,%d)", x, y);
        return false;
    }

    uint16_t index = y * 25 + (x >> 3);
    uint8_t bit = 7 - (x & 0x07);
    uint8_t before = buffer[index];
    if(color == DRIVER_COLOR_WHITE) {
        buffer[index] |= (0x01 << bit);
    } else {
        buffer[index] &= ~(0x01 << bit);
    }
    return buffer[index] != before;
}

void CustomLcdDisplay::ShowSetupCode(const char* ssid, const char* password, unsigned pairing_code) {
    if (!Lock(3000)) return;
    ResetSaver();
    if (!setup_overlay_) {
        setup_overlay_ = lv_obj_create(lv_display_get_screen_active(display_));
        lv_obj_remove_style_all(setup_overlay_);
        lv_obj_add_flag(setup_overlay_, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(setup_overlay_, 200, 200);
        lv_obj_center(setup_overlay_);
        lv_obj_set_style_bg_color(setup_overlay_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(setup_overlay_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(setup_overlay_, LV_OBJ_FLAG_SCROLLABLE);

        // Both fields are generated locally using only letters, digits and '-'.
        std::string payload = std::string("WIFI:T:WPA;S:") + ssid + ";P:" + password + ";;";
        auto* qr = lv_qrcode_create(setup_overlay_);
        lv_qrcode_set_size(qr, 116);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 16);
        if (lv_qrcode_update(qr, payload.data(), payload.size()) != LV_RESULT_OK)
            ESP_LOGW(TAG, "Wi-Fi QR encoding failed");

        auto* label = lv_label_create(setup_overlay_);
        lv_obj_set_style_text_font(label, lv_obj_get_style_text_font(lv_display_get_screen_active(display_), 0), 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(label, 0, 0);
        lv_label_set_text_fmt(label, "%s  %s\nhttp://192.168.4.1\n蓝牙配对 %06u", ssid, password, pairing_code);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 148);
    }
    Unlock();
}

void CustomLcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!Lock(3000)) return;
    ResetSaver();
    if (setup_overlay_) {
        lv_obj_delete(setup_overlay_);
        setup_overlay_ = nullptr;
    }
    Unlock();
    if (content && !*content && !strcmp(role, "system") &&
        Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        const char* source = VoiceProfile::Load().Custom() ? "自建语音网关" : "小智语音服务";
        std::string home = std::string(source) + "\n说“小鸭小鸭”开始对话\nPWR 本地音乐\n长按 BOOT 主菜单";
        LcdDisplay::SetChatMessage(role, home.c_str());
    } else if (content && !strcmp(role, "system") &&
               Application::GetInstance().GetDeviceState() == kDeviceStateActivating) {
        std::string message = std::string(content) + "\n长按 BOOT 打开主菜单";
        LcdDisplay::SetChatMessage(role, message.c_str());
    } else {
        LcdDisplay::SetChatMessage(role, content);
    }
}

void CustomLcdDisplay::ShowMenu(const std::string& text) {
    if (!Lock(3000)) return;
    RenderMenuLocked(text);
    Unlock();
}

void CustomLcdDisplay::RenderMenuLocked(const std::string& text) {
    ResetSaver();
    if (!menu_label_) menu_label_ = CreatePageLabel();
    lv_obj_move_foreground(menu_label_);
    lv_obj_clean(menu_label_);
    lv_label_set_text(menu_label_, text.c_str());
}

lv_obj_t* CustomLcdDisplay::CreatePageLabel() {
    auto* label = lv_label_create(lv_display_get_screen_active(display_));
    lv_obj_add_flag(label, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(label, 200, 200);
    lv_obj_center(label);
    lv_obj_set_style_pad_all(label, 5, 0);
    lv_obj_set_style_text_line_space(label, 2, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_bg_color(label, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    return label;
}

void CustomLcdDisplay::ShowVoiceHint(const char* text, int duration_ms) {
    if (!Lock(3000)) return;
    if (voice_hint_timer_) lv_timer_delete(voice_hint_timer_);
    voice_hint_timer_ = nullptr;
    if (*text) {
        ResetSaver();
        if (!voice_hint_) {
            voice_hint_ = lv_label_create(lv_display_get_layer_top(display_));
            lv_obj_set_width(voice_hint_, 196);
            lv_obj_set_style_pad_all(voice_hint_, 4, 0);
            lv_obj_set_style_text_font(voice_hint_, lv_obj_get_style_text_font(lv_display_get_screen_active(display_), 0), 0);
            lv_obj_set_style_bg_opa(voice_hint_, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(voice_hint_, lv_color_white(), 0);
            lv_obj_set_style_text_color(voice_hint_, lv_color_black(), 0);
            lv_obj_set_style_border_width(voice_hint_, 1, 0);
        }
        lv_label_set_text(voice_hint_, text);
        lv_obj_align(voice_hint_, LV_ALIGN_BOTTOM_MID, 0, -2);
        voice_hint_timer_ = lv_timer_create([](lv_timer_t* timer) {
            auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
            lv_obj_delete(self->voice_hint_);
            self->voice_hint_ = nullptr;
            self->voice_hint_timer_ = nullptr;
        }, duration_ms, this);
        lv_timer_set_repeat_count(voice_hint_timer_, 1);
    } else if (voice_hint_) {
        lv_obj_delete(voice_hint_);
        voice_hint_ = nullptr;
    }
    Unlock();
}

void CustomLcdDisplay::ShowHome(bool visible) {
    if (!Lock(3000)) return;
    ResetSaver();
    if (visible) {
        if (!home_page_) home_page_ = CreatePageLabel();
        lv_label_set_text(home_page_, "首页\n说“小鸭小鸭”可直接对话\n短按 BOOT 进入语音通话\n短按 PWR 播放或暂停音乐\n长按 BOOT 打开主菜单");
        if (network_page_) lv_obj_move_foreground(network_page_);
        if (menu_label_) lv_obj_move_foreground(menu_label_);
    } else if (home_page_) {
        lv_obj_delete(home_page_);
        home_page_ = nullptr;
    }
    Unlock();
}

void CustomLcdDisplay::ShowNetworkPage(const char* text) {
    if (!Lock(3000)) return;
    ResetSaver();
    if (text) {
        if (!network_page_) network_page_ = CreatePageLabel();
        lv_label_set_text(network_page_, text);
        if (menu_label_) lv_obj_move_foreground(menu_label_);
    } else if (network_page_) {
        lv_obj_delete(network_page_);
        network_page_ = nullptr;
    }
    Unlock();
}

void CustomLcdDisplay::CloseMenu() {
    if (!Lock(3000)) return;
    ResetSaver();
    if (menu_label_) lv_obj_delete(menu_label_);
    menu_label_ = nullptr;
    Unlock();
}

void CustomLcdDisplay::ResetSaver() {
    last_activity_ = lv_tick_get();
    if (saver_layer_) {
        lv_obj_delete(saver_layer_);
        saver_layer_ = nullptr;
    }
}

bool CustomLcdDisplay::WakeScreen() {
    if (!Lock(3000)) return false;
    bool visible = saver_layer_ != nullptr;
    ResetSaver();
    Unlock();
    return visible;
}

void CustomLcdDisplay::SetStatus(const char* status) {
    // The standby clock changes every minute, including while the saver is on.
    if (status && !(strlen(status) >= 5 && status[2] == ':' && isdigit((unsigned char)status[0]))) {
        if (Lock(3000)) {
            ResetSaver();
            saver_allowed_ = !strcmp(status, Lang::Strings::STANDBY) || !strcmp(status, "离线首页");
            Unlock();
        }
    }
    LcdDisplay::SetStatus(status);
}

void CustomLcdDisplay::UpdateSaver() {
    if (!saver_available_ || (!saver_allowed_ && !home_page_) || menu_label_ || network_page_ || setup_overlay_ ||
        saver_layer_ || lv_tick_elaps(last_activity_) < 60000) return;
    saver_layer_ = lv_image_create(lv_display_get_screen_active(display_));
    lv_obj_add_flag(saver_layer_, LV_OBJ_FLAG_FLOATING);
    lv_image_set_src(saver_layer_, &saver_image_);
    lv_obj_center(saver_layer_);
}

bool CustomLcdDisplay::ReadSaver(uint8_t* pixels) {
    nvs_handle_t nvs;
    if (nvs_open_from_partition("omni_ui", "display", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t length = saver_bytes;
    auto err = nvs_get_blob(nvs, "saver", pixels, &length);
    nvs_close(nvs);
    return err == ESP_OK && length == saver_bytes;
}

bool CustomLcdDisplay::SaveSaver(const uint8_t* pixels) {
    nvs_handle_t nvs;
    if (nvs_open_from_partition("omni_ui", "display", NVS_READWRITE, &nvs) != ESP_OK) return false;
    auto err = pixels ? nvs_set_blob(nvs, "saver", pixels, saver_bytes) : nvs_erase_key(nvs, "saver");
    if (err == ESP_ERR_NVS_NOT_FOUND && !pixels) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK || !Lock(3000)) return false;
    ResetSaver();
    lv_image_cache_drop(&saver_image_);
    if (pixels) memcpy(saver_pixels_ + 8, pixels, saver_bytes);
    saver_available_ = pixels != nullptr;
    Unlock();
    return true;
}

void CustomLcdDisplay::ShowGuide(int page) {
    static const char* const messages[] = {
        "欢迎使用\n\n卡内音乐可以离线播放。\n首页按 BOOT 进入语音通话。\n\n以后可在主菜单重看帮助。",
        "BOOT 键\n单击对话，长按打开菜单。\n菜单中单击确认。",
        "PWR 键\n单击播放或暂停，双击下一首。\n菜单中单击换项，长按关机。",
        "联网与设置\n\n长按 BOOT 打开主菜单，\n选“手机配网与模型”。\n手机扫码后可保存多个 Wi-Fi，\n也能设置语音服务和屏保。"
    };
    if (page < 0 || page > 3 || !Lock(3000)) return;
    RenderMenuLocked(messages[page]);
    if (menu_label_) {
        auto label = [&](const char* text, int x, int y) {
            auto* obj = lv_label_create(menu_label_);
            lv_label_set_text(obj, text);
            lv_obj_set_pos(obj, x, y);
        };
        if (page == 1 || page == 2) {
            // Rear view as in Waveshare's V2 hardware diagram, USB on the right.
            auto* outline = lv_obj_create(menu_label_);
            lv_obj_remove_style_all(outline);
            lv_obj_set_pos(outline, 24, 76);
            lv_obj_set_size(outline, 140, 42);
            lv_obj_set_style_border_width(outline, 1, 0);
            lv_obj_set_style_border_color(outline, lv_color_black(), 0);
            label("背面       USB >", 34, 83);
            for (int x : {100, 130}) {
                auto* key = lv_obj_create(menu_label_);
                lv_obj_remove_style_all(key);
                lv_obj_set_pos(key, x, 117);
                lv_obj_set_size(key, 10, 4);
                lv_obj_set_style_bg_color(key, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
            }
            static const lv_point_precise_t arrow[] = {{5,15},{5,0},{0,6},{5,0},{10,6}};
            auto* line = lv_line_create(menu_label_);
            lv_line_set_points(line, arrow, 5);
            lv_obj_set_style_line_color(line, lv_color_black(), 0);
            lv_obj_set_style_line_width(line, 2, 0);
            lv_obj_set_pos(line, page == 1 ? 100 : 130, 121);
            label(page == 1 ? "BOOT" : "PWR", page == 1 ? 87 : 122, 137);
        }
        label(page == 3 ? "BOOT 完成    PWR 跳过" : "BOOT 下一页  PWR 跳过", 0, 169);
    }
    Unlock();
}
