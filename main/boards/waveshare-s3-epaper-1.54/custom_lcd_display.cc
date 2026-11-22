#include "phone_setup.h"
#include "custom_lcd_display.h"
#include "pocket_memo.h"

#include "lcd_display.h"

#include <vector>
#include <algorithm>
#include <esp_attr.h>
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



// 48-pixel monochrome rendition of branding/quack-pocket.svg.
static void AddPocketDuck(lv_obj_t* parent, int x, int y) {
    static const uint8_t pixels[] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
    0x00, 0x00, 0x00, 0x00, 0x01, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x39, 0x30,
    0x00, 0x00, 0x00, 0x00, 0x3f, 0x30, 0x00, 0x00, 0x00, 0x00, 0x33, 0x3e,
    0x00, 0x00, 0x00, 0x00, 0x19, 0x0f, 0x80, 0x00, 0x00, 0x00, 0x18, 0x00,
    0xe0, 0x00, 0x00, 0x00, 0x30, 0x00, 0x70, 0x00, 0x00, 0x00, 0x60, 0x00,
    0x30, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x18, 0x00, 0x00, 0x00, 0xc0, 0x04,
    0x08, 0x00, 0x00, 0x01, 0x80, 0x06, 0x0c, 0x00, 0x00, 0x01, 0x80, 0x06,
    0x0c, 0x00, 0x00, 0x01, 0x00, 0x00, 0x7f, 0x80, 0x00, 0x01, 0x00, 0x01,
    0xff, 0x80, 0x00, 0x03, 0x00, 0x01, 0x81, 0x80, 0x00, 0x03, 0x00, 0x01,
    0x81, 0x80, 0x00, 0x07, 0x00, 0x00, 0xff, 0x00, 0x00, 0x06, 0x00, 0x00,
    0x7e, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x1e, 0x00, 0x03, 0xf8, 0x00, 0x01,
    0xfe, 0x00, 0x03, 0xff, 0xff, 0xff, 0xc2, 0x00, 0x01, 0x03, 0xff, 0xf0,
    0x0e, 0x00, 0x01, 0xe0, 0x00, 0x00, 0xfe, 0x00, 0x01, 0xff, 0xc0, 0x7f,
    0xe6, 0x00, 0x01, 0x8f, 0xff, 0xfe, 0x06, 0x00, 0x00, 0x80, 0x03, 0x00,
    0x04, 0x00, 0x00, 0x80, 0x03, 0x80, 0x14, 0x00, 0x00, 0xd0, 0x0e, 0xe0,
    0x04, 0x00, 0x00, 0xc0, 0x08, 0x20, 0x0c, 0x00, 0x00, 0xc0, 0x0c, 0x40,
    0x0c, 0x00, 0x00, 0x60, 0x04, 0x40, 0x2c, 0x00, 0x00, 0x64, 0x07, 0xc0,
    0x18, 0x00, 0x00, 0x30, 0x04, 0x40, 0xb8, 0x00, 0x00, 0x18, 0x00, 0x02,
    0x70, 0x00, 0x00, 0x0e, 0x40, 0x08, 0xe0, 0x00, 0x00, 0x07, 0x80, 0x23,
    0x80, 0x00, 0x00, 0x01, 0xe0, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x78, 0x7c,
    0x00, 0x00, 0x00, 0x00, 0x1f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x03, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const lv_image_dsc_t image = [] {
        lv_image_dsc_t icon{};
        icon.header.magic = LV_IMAGE_HEADER_MAGIC;
        icon.header.cf = LV_COLOR_FORMAT_I1;
        icon.header.w = icon.header.h = 48;
        icon.header.stride = 6;
        icon.data_size = sizeof(pixels);
        icon.data = pixels;
        return icon;
    }();
    auto* duck = lv_image_create(parent);
    lv_image_set_src(duck, &image);
    lv_obj_set_pos(duck, x, y);
}

LV_FONT_DECLARE(font_awesome_30_4);

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
    if (driver->shutdown_frame_) {
        lv_disp_flush_ready(disp);
        return;
    }
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
    EPD_DisplayPartBaseImage();
    EPD_Init_Partial();            //局部刷新初始化

    lvgl_port_unlock();
	if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    ESP_LOGI(TAG, "ui start");

    SetupUI();
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_init_partition("omni_ui"));
    if (Lock(0)) {
        std::memset(saver_pixels_, 255, 4);
        saver_pixels_[7] = 255;
        saver_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
        saver_image_.header.cf = LV_COLOR_FORMAT_I1;
        saver_image_.header.w = saver_image_.header.h = 200;
        saver_image_.header.stride = 25;
        saver_image_.data_size = sizeof(saver_pixels_);
        saver_image_.data = saver_pixels_;
        saver_available_ = ReadSaver(Saver::Standby, saver_pixels_ + 8);
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

void CustomLcdDisplay::writeBytes(const uint8_t* data, int len) {
    // The frame lives in PSRAM. This panel has one serialized SPI writer.
    static DMA_ATTR uint8_t chunk[256];
    set_dc_1();
    set_cs_0();
    while (len > 0) {
        const int size = std::min(len, static_cast<int>(sizeof(chunk)));
        memcpy(chunk, data, size);
        spi_transaction_t transaction{};
        transaction.length = size * 8;
        transaction.tx_buffer = chunk;
        ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &transaction));
        data += size;
        len -= size;
    }
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

void CustomLcdDisplay::ShowBluetoothPrompt(const std::string& message) {
    if (!Lock(3000)) return;
    ResetSaver();
    if(message.empty()) {
        if(bluetooth_prompt_) lv_obj_delete(bluetooth_prompt_);
        bluetooth_prompt_=nullptr;
    } else {
        if(!bluetooth_prompt_) bluetooth_prompt_=CreatePageLabel();
        lv_label_set_text(bluetooth_prompt_,message.c_str());
        lv_obj_move_foreground(bluetooth_prompt_);
    }
    Unlock();
}

void CustomLcdDisplay::ShowSetupCode(const char* ssid, const char* password, unsigned pairing_code, const std::string& lan_url, const std::string& fingerprint, const std::string& address) {
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
        if (!lan_url.empty()) payload = lan_url;
        auto* qr = lv_qrcode_create(setup_overlay_);
        lv_qrcode_set_size(qr, 112);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_obj_set_pos(qr, 6, 5);
        if (lv_qrcode_update(qr, payload.data(), payload.size()) != LV_RESULT_OK)
            ESP_LOGW(TAG, "Wi-Fi QR encoding failed");

        auto* steps = lv_label_create(setup_overlay_);
        lv_label_set_text(steps, !lan_url.empty() ? "同一 Wi-Fi\n扫码管理\n\nPWR 退出" : "1 手机扫码\n加入热点\n\n2 浏览器\n打开下方\n网址");
        lv_obj_set_pos(steps, 130, 4);
        auto* details = lv_label_create(setup_overlay_);
        lv_obj_set_style_text_line_space(details, 0, 0);
        if (!lan_url.empty()) lv_label_set_text_fmt(details, "备用 https://%s\n证书 SHA256 前 24 位\n%.12s\n%s", address.c_str(),fingerprint.c_str(),fingerprint.c_str()+12);
        else if(pairing_code>=1000000) lv_label_set_text_fmt(details,"http://192.168.4.1\n热点 %s\n密码 %s\n蓝牙已关闭",ssid,password);
        else lv_label_set_text_fmt(details, "http://192.168.4.1\n热点 %s\n密码 %s\n蓝牙配对码 %06u", ssid, password, pairing_code);
        lv_obj_set_pos(details, 6, 126);
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
        std::string home = std::string(source) + (Settings("omni_ui").GetInt("wake", 1) ? "\n说“小鸭小鸭”开始对话" : "\n按 BOOT 开始对话") + "\nPWR 播放音乐\n长按 BOOT 主菜单";
        LcdDisplay::SetChatMessage(role, home.c_str());
    } else if (content && !strcmp(role, "system") &&
               Application::GetInstance().GetDeviceState() == kDeviceStateActivating) {
        std::string message = std::string(content) + "\n长按 BOOT 打开主菜单";
        LcdDisplay::SetChatMessage(role, message.c_str());
    } else {
        LcdDisplay::SetChatMessage(role, content);
    }
}

void CustomLcdDisplay::ShowMenu(const std::string& header, const char* icon, const char* title, const std::string& detail, const char* navigation) {
    if (!Lock(3000)) return;
    RenderMenuLocked(header);
    auto centered = [this](const char* text, int y) {
        auto* label = lv_label_create(menu_label_);
        lv_label_set_text(label, text);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
        return label;
    };
    auto* glyph = centered(icon, 44);
    lv_obj_set_style_text_font(glyph, &font_awesome_30_4, 0);
    centered(title, 88);
    if (!detail.empty()) centered(detail.c_str(), 110);
    centered(navigation, 145);
    centered("BOOT 短按确认 · 长按返回", 167);
    Unlock();
}

void CustomLcdDisplay::ShowDisconnectedHome(bool reconnecting) {
    if (!Lock(3000)) return;
    ResetSaver();
    if (!network_page_) network_page_ = CreatePageLabel();
    lv_obj_clean(network_page_);
    RenderHomeText(network_page_, reconnecting ? "Wi-Fi 重连中" : "尚未设置 Wi-Fi", "短按 BOOT 说音乐口令");
    AddPocketDuck(network_page_, 136, 5);
    network_home_ = true;
    if (menu_label_) lv_obj_move_foreground(menu_label_);
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
        if (standby_page_) lv_obj_move_foreground(standby_page_);
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

void CustomLcdDisplay::RenderHomeText(lv_obj_t* page, const char* greeting, const char* action) {
    if (PocketMemoUnread()) action = "有新留言 · BOOT 收听";
    lv_label_set_text_fmt(page, "嘎嘎口袋\n%s\n\n%s\n短按 PWR 播放或暂停音乐\n长按 BOOT 打开主菜单", greeting, action);
}

void CustomLcdDisplay::RenderConnectedHome() {
    const char* greeting = Settings("omni_ui").GetInt("wake", 1) ? "叫我“小鸭小鸭”" : "语音唤醒已关闭";
    RenderHomeText(home_page_, greeting, "短按 BOOT 进入语音通话");
}

void CustomLcdDisplay::RefreshHome() {
    if (!Lock(3000)) return;
    if (home_page_) RenderConnectedHome();
    if (network_home_ && network_page_) RenderHomeText(network_page_, "离线首页", "短按 BOOT 说音乐口令");
    Unlock();
}

void CustomLcdDisplay::ShowHome(bool visible) {
    if (!Lock(3000)) return;
    ResetSaver();
    if (visible) {
        if (!home_page_) {
            home_page_ = CreatePageLabel();
            AddPocketDuck(home_page_, 136, 5);
        }
        RenderConnectedHome();
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
    network_home_ = false;
    if (text) {
        if (!network_page_) network_page_ = CreatePageLabel();
        lv_obj_clean(network_page_);
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
            saver_allowed_ = !strcmp(status, Lang::Strings::STANDBY);
            Unlock();
        }
    }
    LcdDisplay::SetStatus(status);
}

void CustomLcdDisplay::UpdateSaver() {
    if (ManagementInUse()) {
        ResetSaver();
        return;
    }
    if (standby_page_ || !saver_available_ || (!saver_allowed_ && !home_page_ && !network_home_) || menu_label_ || (network_page_ && !network_home_) || setup_overlay_ ||
        saver_layer_ || lv_tick_elaps(last_activity_) < 60000) return;
    saver_layer_ = lv_image_create(lv_display_get_screen_active(display_));
    lv_obj_add_flag(saver_layer_, LV_OBJ_FLAG_FLOATING);
    lv_image_set_src(saver_layer_, &saver_image_);
    lv_obj_center(saver_layer_);
}

namespace {
extern const uint8_t listening_duck[] asm("_binary_duck_listening_pbm_start");
extern const uint8_t sleeping_duck[] asm("_binary_duck_sleeping_pbm_start");
const uint8_t* DefaultSaver(CustomLcdDisplay::Saver slot) {
    // Both embedded P4 images use the header "P4\n200 200\n", MSB first, black = 1.
    return (slot == CustomLcdDisplay::Saver::Standby ? listening_duck : sleeping_duck) + sizeof("P4\n200 200\n") - 1;
}
const char* SaverKey(CustomLcdDisplay::Saver slot) {
    return slot == CustomLcdDisplay::Saver::Standby ? "saver" : "shutdown";
}
}

// Always supplies a complete image. The return value identifies a custom image.
bool CustomLcdDisplay::ReadSaver(Saver slot, uint8_t* pixels) {
    nvs_handle_t nvs;
    if (nvs_open_from_partition("omni_ui", "display", NVS_READONLY, &nvs) == ESP_OK) {
        size_t length = saver_bytes;
        auto err = nvs_get_blob(nvs, SaverKey(slot), pixels, &length);
        nvs_close(nvs);
        if (err == ESP_OK && length == saver_bytes) return true;
    }
    memcpy(pixels, DefaultSaver(slot), saver_bytes);
    return false;
}

bool CustomLcdDisplay::SaveSaver(Saver slot, const uint8_t* pixels) {
    if (!Lock(3000)) return false;
    nvs_handle_t nvs;
    auto err = nvs_open_from_partition("omni_ui", "display", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        auto key = SaverKey(slot);
        err = pixels ? nvs_set_blob(nvs, key, pixels, saver_bytes) : nvs_erase_key(nvs, key);
        if (err == ESP_ERR_NVS_NOT_FOUND && !pixels) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err == ESP_OK && slot == Saver::Standby) {
        ResetSaver();
        lv_image_cache_drop(&saver_image_);
        memcpy(saver_pixels_ + 8, pixels ? pixels : DefaultSaver(slot), saver_bytes);
        saver_available_ = pixels != nullptr;
        if (standby_page_) lv_obj_invalidate(standby_page_);
    }
    Unlock();
    return err == ESP_OK;
}

void CustomLcdDisplay::ShowShutdownSaver() {
    Lock(0);
    shutdown_frame_ = true;
    ReadSaver(Saver::Shutdown, buffer);
    for (size_t i = 0; i < saver_bytes; ++i) buffer[i] ^= 0xff; // panel RAM uses white = 1
    EPD_Init();
    EPD_DisplayPartBaseImage(); // waits for the physical BUSY pin after the full refresh
    Unlock();
}

bool CustomLcdDisplay::ShowVoiceStandby(bool visible) {
    if (!Lock(3000)) return false;
    ResetSaver();
    if (visible && !standby_page_) {
        standby_page_ = lv_image_create(lv_display_get_layer_top(display_));
        lv_image_set_src(standby_page_, &saver_image_);
        lv_obj_center(standby_page_);
    } else if (!visible && standby_page_) {
        lv_obj_delete(standby_page_);
        standby_page_ = nullptr;
    }
    Unlock();
    return true;
}

void CustomLcdDisplay::ShowUpdatePage(const std::string& text) {
    if (!Lock(3000)) return;
    RenderMenuLocked(text);
    Unlock();
}

void CustomLcdDisplay::ShowGuide(int page) {
    static const char* const messages[] = {
        "嘎嘎口袋   1/4\n嘎，按 BOOT 翻页。\n看背面，USB 朝右。",
        "口袋音乐   2/4\n卡内 MP3 可离线听。\nPWR 播放或暂停。\n双击切歌，长按关机。",
        "叫醒小鸭   3/4\n\n待机短按任一键返回。\n喊“小鸭小鸭”：\n联网通话，离线说口令。\n离线口令需装模型。",
        "手机设置   4/4\n\n长按 BOOT 打开菜单。\n选“手机与网络”组，\n再选“手机配网与模型”。\n重看：设备设置→使用帮助"
    };
    if (page < 0 || page > 3 || !Lock(3000)) return;
    RenderMenuLocked(messages[page]);
    if (menu_label_) {
        auto label = [&](const char* text, int x, int y) {
            auto* obj = lv_label_create(menu_label_);
            lv_label_set_text(obj, text);
            lv_obj_set_pos(obj, x, y);
        };
        if (page == 0 || page == 1) {
            // Rear view as in Waveshare's V2 hardware diagram, USB on the right.
            auto* outline = lv_obj_create(menu_label_);
            lv_obj_remove_style_all(outline);
            lv_obj_set_pos(outline, 24, 80);
            lv_obj_set_size(outline, 140, 42);
            lv_obj_set_style_border_width(outline, 1, 0);
            lv_obj_set_style_border_color(outline, lv_color_black(), 0);
            label("背面       USB >", 34, 87);
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
            lv_obj_set_pos(line, page == 0 ? 100 : 130, 121);
            label(page == 0 ? "BOOT" : "PWR", page == 0 ? 87 : 122, 137);
        }
        if (page == 0 || page == 2) AddPocketDuck(menu_label_, 142, 0);
        label(page == 3 ? "BOOT 完成    PWR 跳过" : "BOOT 下一页  PWR 跳过", 0, 169);
    }
    Unlock();
}
