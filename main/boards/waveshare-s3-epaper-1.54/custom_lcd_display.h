#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include "lcd_display.h"
#include "driver/gpio.h"

/* Display color */
typedef enum {
    DRIVER_COLOR_WHITE  = 0xff,
    DRIVER_COLOR_BLACK  = 0x00,
    FONT_BACKGROUND = DRIVER_COLOR_WHITE,
}COLOR_IMAGE;

typedef struct {
    uint8_t cs;
    uint8_t dc;
    uint8_t rst;
    uint8_t busy;
    uint8_t mosi;
    uint8_t scl;
    int spi_host;
    int buffer_len;
}custom_lcd_spi_t;


class CustomLcdDisplay : public LcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,custom_lcd_spi_t _lcd_spi_data);
    ~CustomLcdDisplay();

    static constexpr size_t saver_bytes = 200 * 200 / 8;
    bool SaveSaver(const uint8_t* bitmap);
    bool ReadSaver(uint8_t* bitmap);
    bool WakeScreen();
    void SetStatus(const char* status) override;
    void ShowVoiceHint(const char* text, int duration_ms) override;
    void ShowGuide(int page);
    void ShowMenu(const std::string& text);
    void CloseMenu();
    lv_obj_t* home_page_ = nullptr;
    void ShowHome(bool visible) override;
    void ShowNetworkPage(const char* text);
    void ShowSetupCode(const char* ssid, const char* password, unsigned pairing_code);
    void SetChatMessage(const char* role, const char* content) override;
    void EPD_Init();    /* 墨水屏初始化 */
    void EPD_Clear();   /* 清空屏幕 */
    void EPD_Display(); /* 刷buffer到墨水屏 */
    
    /*局部刷新*/
    void EPD_DisplayPartBaseImage();
    void EPD_Init_Partial();
    void EPD_DisplayPart();
    bool EPD_DrawColorPixel(uint16_t x, uint16_t y,uint8_t color);
    
private:
    lv_obj_t* voice_hint_ = nullptr;
    lv_timer_t* voice_hint_timer_ = nullptr;
    void RenderMenuLocked(const std::string& text);
    void ResetSaver();
    void UpdateSaver();
    bool saver_available_ = false, saver_allowed_ = false;
    uint32_t last_activity_ = 0;
    lv_obj_t* saver_layer_ = nullptr;
    lv_image_dsc_t saver_image_{};
    uint8_t saver_pixels_[8 + saver_bytes]{};
    lv_obj_t* CreatePageLabel();
    lv_obj_t* network_page_ = nullptr;
    lv_obj_t* menu_label_ = nullptr;
    lv_obj_t* setup_overlay_ = nullptr;
    const custom_lcd_spi_t lcd_spi_data;
    const int Width;
    const int Height;
    spi_device_handle_t spi;
    uint8_t *buffer = NULL;
    unsigned partial_updates_ = 0;
    
    static void lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);
    
    void spi_gpio_init();
    void spi_port_init();
    void read_busy();

    void set_cs_1(){gpio_set_level((gpio_num_t)lcd_spi_data.cs,1);}
    void set_cs_0(){gpio_set_level((gpio_num_t)lcd_spi_data.cs,0);}
    void set_dc_1(){gpio_set_level((gpio_num_t)lcd_spi_data.dc,1);}
    void set_dc_0(){gpio_set_level((gpio_num_t)lcd_spi_data.dc,0);}
    void set_rst_1(){gpio_set_level((gpio_num_t)lcd_spi_data.rst,1);}
    void set_rst_0(){gpio_set_level((gpio_num_t)lcd_spi_data.rst,0);}

    void SPI_SendByte(uint8_t data);
    void EPD_SendData(uint8_t data);
    void EPD_SendCommand(uint8_t command);
    void writeBytes(uint8_t *buffer,int len);
    void writeBytes(const uint8_t *buffer, int len);
    void EPD_SetWindows(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend);
    void EPD_SetCursor(uint16_t Xstart, uint16_t Ystart);
    void EPD_SetLut(const uint8_t *lut);
    void EPD_TurnOnDisplay();
    void EPD_TurnOnDisplayPart();
};

#endif // __CUSTOM_LCD_DISPLAY_H__