#ifdef OMNI_MUSIC_HIL
#include "music_hil.h"
#endif
#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "codecs/es8311_audio_codec.h"

#include <esp_log.h>
#include <algorithm>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali_scheme.h>
#include "settings.h"
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include "mcp_server.h"
#include "music_player.h"
#include "phone_setup.h"
#include "voice/voice_profile.h"
#include "lvgl.h"
#include "custom_lcd_display.h"
#include "board_power_bsp.h"

#define TAG "waveshare_epaper_1_54"

class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    MusicPlayer music_;
    Button boot_button_;
    Button pwr_button_;
    CustomLcdDisplay *display_;
    board_power_bsp *power_;

    adc_oneshot_unit_handle_t battery_adc_ = nullptr;
    adc_cali_handle_t battery_cali_ = nullptr;
    std::atomic<bool> low_battery_{false};
    enum class NetworkPage { Hidden, Disconnected, Local };
    std::atomic<NetworkPage> network_page_{NetworkPage::Hidden};
    std::atomic<bool> local_only_{false};
    std::atomic<bool> menu_open_{false};
    size_t item_ = 0;
    std::atomic<int> guide_page_{-1};
    std::atomic<TaskHandle_t> guide_waiter_{nullptr};
    void BeginGuide() {
        display_->ShowGuide(0);
        guide_page_ = 0;
    }

    bool GuideButton(bool skip) {
        int page = guide_page_;
        if (page < 0) return false;
        if (!skip && page < 3) {
            guide_page_ = page + 1;
            display_->ShowGuide(page + 1);
        } else {
            guide_page_ = -1;
            display_->CloseMenu();
            if (auto waiter = guide_waiter_.exchange(nullptr)) xTaskNotifyGive(waiter);
            else { menu_open_ = true; RenderMenu(); }
        }
        return true;
    }
    enum MenuItem { Chat, Play, Next, Sequential, Shuffle, VolumeDown, VolumeUp, Refresh,
                    Phone, OfflineVoice, InstallModel, Help, Return, ItemCount };
    static constexpr const char* items_[ItemCount] = {
        "进入语音通话模式", "本地播放 / 暂停", "下一首", "顺序播放", "随机播放",
        "音量减 10", "音量加 10", "刷新曲库", "手机配网与模型", "离线语音控制", "安装离线模型", "使用帮助", "返回"
    };

    esp_timer_handle_t green_off_ = nullptr;
    std::atomic<int64_t> next_green_{0};
    void InitBattery() {
        gpio_set_level(GPIO_NUM_3, 1);
        gpio_set_direction(GPIO_NUM_3, GPIO_MODE_OUTPUT);
        esp_timer_create_args_t led{};
        led.callback = [](void*) { gpio_set_level(GPIO_NUM_3, 1); };
        led.name = "low_battery_led";
        if (esp_timer_create(&led, &green_off_) != ESP_OK) green_off_ = nullptr;
        adc_oneshot_unit_init_cfg_t unit{};
        unit.unit_id = ADC_UNIT_1;
        adc_oneshot_chan_cfg_t chan{};
        chan.atten = ADC_ATTEN_DB_12;
        chan.bitwidth = ADC_BITWIDTH_12;
        adc_cali_curve_fitting_config_t cal{};
        cal.unit_id = ADC_UNIT_1;
        cal.chan = ADC_CHANNEL_3;
        cal.atten = chan.atten;
        cal.bitwidth = chan.bitwidth;
        if (adc_oneshot_new_unit(&unit, &battery_adc_) != ESP_OK ||
            adc_oneshot_config_channel(battery_adc_, ADC_CHANNEL_3, &chan) != ESP_OK ||
            adc_cali_create_scheme_curve_fitting(&cal, &battery_cali_) != ESP_OK)
            ESP_LOGW(TAG, "Battery measurement unavailable");
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!battery_adc_ || !battery_cali_) return false;
        int total = 0;
        for (int i = 0; i < 8; ++i) {
            int raw, mv;
            if (adc_oneshot_read(battery_adc_, ADC_CHANNEL_3, &raw) != ESP_OK ||
                adc_cali_raw_to_voltage(battery_cali_, raw, &mv) != ESP_OK) return false;
            total += mv * 2;
        }
        int mv = total / 8;
        if (mv < 2400 || mv > 4450) return false;
        level = std::clamp((mv - 3000) * 100 / 1120, 0, 100);
        int64_t now = esp_timer_get_time(), next = next_green_.load();
        if (level < 20 && green_off_ && now >= next && next_green_.compare_exchange_strong(next, now + 60000000)) {
            gpio_set_level(GPIO_NUM_3, 0);
            if (esp_timer_start_once(green_off_, 150000) != ESP_OK) gpio_set_level(GPIO_NUM_3, 1);
        }
        if (level < 20 && !low_battery_.exchange(true)) {
            Application::GetInstance().Schedule([this]() { display_->WakeScreen(); display_->ShowNotification("电量低，请充电", 10000); });
        } else if (level >= 25) {
            low_battery_ = false;
        }
        charging = discharging = false; // This ADC measures voltage, not current direction.
        return true;
    }

    std::string ServiceName() const {
        return VoiceProfile::Load().Custom() ? "自建网关" : "小智服务";
    }

    void RenderMenu() {
        int level; bool charging, discharging;
        std::string battery = GetBatteryLevel(level, charging, discharging)
            ? (level < 20 ? "低电 " : "约 ") + std::to_string(level) + "%" : "电量未知";
        std::string text = "主菜单   " + battery + "\n" +
            ((local_only_ || Application::GetInstance().IsVoicePaused() || network_page_ == NetworkPage::Local) ? "首页" : ServiceName()) + "   音量 " +
            std::to_string(GetAudioCodec()->output_volume()) + "\n";
        size_t first = item_ / 3 * 3;
        for (size_t i = first; i < std::min(first + 3, size_t(ItemCount)); ++i) {
            text += i == item_ ? "> " : "   ";
            text += (i == Chat && !local_only_ && network_page_ == NetworkPage::Hidden &&
                     !Application::GetInstance().IsVoicePaused() && !setup_waiter_ &&
                     WifiStation::GetInstance().IsConnected()) ? "退出语音通话" : items_[i];
            text += "\n";
        }
        text += "PWR 换项  BOOT 确认\n长按 BOOT 返回";
        display_->ShowMenu(text);
    }

    void ToggleMenu() {
        if (menu_open_) {
            menu_open_ = false;
            display_->CloseMenu();
        } else {
            item_ = 0;
            menu_open_ = true;
            RenderMenu();
        }
    }

    void SelectItem() {
        if (item_ == VolumeDown || item_ == VolumeUp) {
            auto* codec = GetAudioCodec();
            codec->SetOutputVolume(std::clamp(codec->output_volume() + (item_ == VolumeUp ? 10 : -10), 0, 100));
            RenderMenu();
            return;
        }
        ToggleMenu();
        switch (item_) {
            case Chat: {
                if (network_page_ != NetworkPage::Hidden) {
                    display_->ShowNotification("正在重连 Wi-Fi");
                    break;
                }
                if (Application::GetInstance().IsVoicePaused()) {
                    Application::GetInstance().SetVoicePaused(false);
                    break;
                }
                if (local_only_) {
                    { Settings mode("omni_mode", true); mode.SetInt("offline", 0); }
                    esp_restart();
                }
                auto& app = Application::GetInstance();
                if (WifiStation::GetInstance().IsConnected()) app.SetVoicePaused(true);
                else display_->ShowNotification("请先连接 Wi-Fi");
                break;
            }
            case Play: RequestMusicFromButton(MusicAction::Toggle); break;
            case Next: RequestMusicFromButton(MusicAction::Next); break;
            case Sequential: RequestMusicFromButton(MusicAction::Sequential); break;
            case Shuffle: RequestMusicFromButton(MusicAction::Shuffle); break;
            case Refresh: RequestMusicFromButton(MusicAction::Refresh); break;
            case InstallModel:
                if (local_only_) {
                    {
                        Settings mode("omni_mode", true);
                        mode.SetInt("install_model", 1);
                        mode.SetInt("offline", 0);
                    }
                    esp_restart();
                } else if (WifiStation::GetInstance().IsConnected()) {
                    RequestMusicFromButton(MusicAction::InstallModel);
                } else {
                    display_->ShowNotification("请连接 Wi-Fi 后再安装模型");
                }
                break;
            case OfflineVoice:
                if (local_only_) RequestMusicFromButton(MusicAction::Listen);
                else RebootToLocalMode();
                break;
            case Help: BeginGuide(); break;
            case Phone: ResetWifiConfiguration(); break;
            default: break;
        }
    }

#ifdef OMNI_MUSIC_HIL
    void StartNetwork() override {
        Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
        music_hil::Run(music_);
    }
#else
    void StartNetwork() override {
        if (!Settings("omni_ui").GetInt("guided", 0)) {
            guide_waiter_ = xTaskGetCurrentTaskHandle();
            BeginGuide();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            Settings guide("omni_ui", true);
            guide.SetInt("guided", 1);
        }
        int offline = Settings("omni_mode").GetInt("offline", 0);
        local_only_ = offline == 1 && !wifi_config_mode_;
        if (offline == 2) Settings("omni_mode", true).SetInt("offline", 0);
        if (local_only_) {
            Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
            display_->SetStatus("离线首页");
            display_->SetChatMessage("system", "BOOT 说离线口令，PWR 播放或暂停\n长按 BOOT 打开主菜单");
            vTaskSuspend(nullptr);
        }
        if (!VoiceProfile::ApplyPendingWifi()) {
            WaitForSetupButton("网络设置读写失败。");
            return;
        }
        WifiBoard::StartNetwork();
        if (Settings("omni_mode").GetInt("install_model", 0) && WifiStation::GetInstance().IsConnected()) {
            Settings("omni_mode", true).SetInt("install_model", 0);
            RequestMusicFromButton(MusicAction::InstallModel);
        }
        auto created = xTaskCreate([](void* board) {
            static_cast<CustomBoard*>(board)->MonitorWifi();
        }, "wifi_monitor", 4096, this, 3, nullptr);
        if (created != pdPASS) ESP_LOGE(TAG, "Cannot start Wi-Fi monitor");
    }

    bool WaitForInitialWifi() override {
        if (!WifiStation::GetInstance().WaitForConnected(8000)) {
            Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
            WaitForWifiRecovery();
        }
        return true;
    }

    void WaitForWifiRecovery() {
        auto& station = WifiStation::GetInstance();
        network_page_ = NetworkPage::Disconnected;
        NetworkPage rendered = NetworkPage::Hidden;
        do {
            auto page = network_page_.load();
            if (page != rendered) {
                display_->ShowNetworkPage(page == NetworkPage::Local
                    ? "离线首页\n\nPWR 播放或暂停音乐\n双击 PWR 下一首\n长按 BOOT 打开菜单\n\n后台正在自动联网"
                    : "Wi-Fi 未连接\n\n正在寻找已保存的网络\n\n短按 BOOT 返回首页\n短按 PWR 进入手机配网");
                rendered = page;
            }
        } while (!station.WaitForConnected(250));
        network_page_ = NetworkPage::Hidden;
        display_->ShowNetworkPage(nullptr);
    }

    void MonitorWifi() {
        for (;;) {
            if (!WifiStation::GetInstance().IsConnected()) WaitForWifiRecovery();
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
#endif
    std::atomic<TaskHandle_t> setup_waiter_{nullptr};

    void WaitForSetupButton(const char* reason) {
        Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
        setup_waiter_.store(xTaskGetCurrentTaskHandle());
        display_->SetStatus("联网失败");
        const std::string hint = std::string("短按机身侧面 BOOT 键一次，进入配网。\n") + reason;
        display_->SetChatMessage("system", hint.c_str());
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        setup_waiter_.store(nullptr);
        EnterWifiConfigMode();
    }

    void OnWifiConnectionFailed(bool found_network) override {
        WaitForSetupButton(found_network ?
            "已发现网络，但连接失败。请检查密码和路由器。" :
            "未找到已保存的 Wi-Fi。请检查名称和 2.4 GHz 信号。");
    }

    void EnterWifiConfigMode() override {
        Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
        RunPhoneSetup(display_);
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = 
            {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void RequestMusicFromButton(MusicAction action) {
        if (!music_.Request(action)) {
            Application::GetInstance().Schedule([this]() {
                display_->ShowNotification("播放器未就绪或正忙，请稍后再按");
            });
        }
    }

    void InitializeButtons() { 
        boot_button_.OnClick([this]() {
            if (GuideButton(false)) return;
            if (display_->WakeScreen()) return;
            if (menu_open_) { SelectItem(); return; }
            auto expected = NetworkPage::Disconnected;
            if (network_page_.compare_exchange_strong(expected, NetworkPage::Local)) return;
            if (local_only_) { RequestMusicFromButton(MusicAction::Listen); return; }
            if (expected == NetworkPage::Local) { ToggleMenu(); return; }
            if (auto task = setup_waiter_.load()) {
                xTaskNotifyGive(task);
                return;
            }
            if (ShowPhoneSetupIfActive()) return;
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            if (app.IsVoicePaused()) app.SetVoicePaused(false);
            else app.ToggleChatState();
        });

        pwr_button_.OnClick([this]() {
            if (GuideButton(true)) return;
            if (display_->WakeScreen()) return;
            if (menu_open_) { item_ = (item_ + 1) % ItemCount; RenderMenu(); }
            else if (network_page_ == NetworkPage::Disconnected) ResetWifiConfiguration();
            else RequestMusicFromButton(MusicAction::Toggle);
        });
        pwr_button_.OnDoubleClick([this]() {
            if (guide_page_ >= 0) return;
            if (menu_open_) { item_ = (item_ + ItemCount - 1) % ItemCount; RenderMenu(); }
            else RequestMusicFromButton(MusicAction::Next);
        });
        boot_button_.OnLongPress([this]() { if (!GuideButton(true)) ToggleMenu(); });
        boot_button_.OnDoubleClick([this]() { if (guide_page_ < 0 && !menu_open_) RequestMusicFromButton(MusicAction::ChangeMode); });

        boot_button_.OnMultipleClick([this]() { if (guide_page_ < 0 && !menu_open_) RequestMusicFromButton(MusicAction::Refresh); }, 3);

        pwr_button_.OnLongPress([this]() {
            if (!music_.RequestPowerOff()) {
                Application::GetInstance().Schedule([this]() { PowerOff(); });
            }
        });
    }

    void EnterLocalMode() {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        if (!WifiStation::GetInstance().IsConnected() &&
            (state == kDeviceStateActivating || state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring)) {
            RebootToLocalMode();
            return;
        }
        app.SetVoicePaused(true);
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.voice.enter_local_mode", "用户明确说返回首页、退出语音通话或结束语音服务时调用。关闭当前语音通话并回到首页，保留 Wi-Fi。", PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                EnterLocalMode();
                return true;
            });
        mcp_server.AddTool("self.music.play_file", "用户点播歌曲时优先调用此工具，按歌名查找并播放存储卡上的 MP3。path 填用户说的歌名（文件名可省略扩展名），或包含扩展名的完整相对路径。",
            PropertyList({ Property("path", kPropertyTypeString) }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto path = properties["path"].value<std::string>();
                bool queued = music_.Request(MusicAction::PlayFile, path.c_str());
                if (queued) Application::GetInstance().FinishChatForMusic();
                return queued;
            });
        mcp_server.AddTool("self.music.control", "音乐控制，action 可为 sequential 顺序播放、shuffle 随机播放、next 下一首或 stop 停止。",
            PropertyList({ Property("action", kPropertyTypeString) }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto action = properties["action"].value<std::string>();
                MusicAction command;
                if (action == "sequential") command = MusicAction::Sequential;
                else if (action == "shuffle") command = MusicAction::Shuffle;
                else if (action == "next") command = MusicAction::Next;
                else if (action == "stop") command = MusicAction::Stop;
                else return false;
                bool queued = music_.Request(command);
                if (queued && command != MusicAction::Stop) Application::GetInstance().FinishChatForMusic();
                return queued;
            });
        mcp_server.AddTool("self.audio_speaker.adjust_volume", "调高或调低当前音量，delta 为增减幅度。",
            PropertyList({Property("delta", kPropertyTypeInteger, -100, 100)}),
            [this](const PropertyList& props) -> ReturnValue {
                auto* codec = GetAudioCodec();
                int volume = std::clamp(codec->output_volume() + props["delta"].value<int>(), 0, 100);
                codec->SetOutputVolume(volume);
                display_->ShowNotification(("音量 " + std::to_string(volume)).c_str());
                return volume;
            });
        mcp_server.AddTool("self.disp.network", "重新配网", PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            ResetWifiConfiguration();
            return true;
        });
    }

    void InitializeLcdDisplay() {
        custom_lcd_spi_t lcd_spi_data = {};
            lcd_spi_data.cs = EPD_CS_PIN;
            lcd_spi_data.dc = EPD_DC_PIN;
            lcd_spi_data.rst = EPD_RST_PIN;
            lcd_spi_data.busy = EPD_BUSY_PIN;
            lcd_spi_data.mosi = EPD_MOSI_PIN;
            lcd_spi_data.scl = EPD_SCK_PIN;
            lcd_spi_data.spi_host = EPD_SPI_NUM;
            lcd_spi_data.buffer_len  = 5000;
        display_ = new CustomLcdDisplay(NULL, NULL, EXAMPLE_LCD_WIDTH,EXAMPLE_LCD_HEIGHT,DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,lcd_spi_data);
    }

    void PowerOff() {
#ifdef OMNI_MUSIC_HIL
        music_hil::PowerOffReached();
        return;
#endif
        display_->CloseMenu();
        GetDisplay()->SetChatMessage("system", "已关机");
        vTaskDelay(pdMS_TO_TICKS(1000));
        power_->POWEER_Audio_OFF();
        power_->POWEER_EPD_OFF();
        power_->VBAT_POWER_OFF();
        // USB can continue to power the MCU after the battery latch opens.
        while (!gpio_get_level(VBAT_PWR_GPIO)) vTaskDelay(1);
        esp_sleep_enable_ext1_wakeup_io(1ULL << VBAT_PWR_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
        esp_deep_sleep_start();
    }

    void Power_Init() {
        power_ = new board_power_bsp(EPD_PWR_PIN,Audio_PWR_PIN,VBAT_PWR_PIN);
        power_->VBAT_POWER_ON();
        power_->POWEER_Audio_ON();
        power_->POWEER_EPD_ON();
        do {
            vTaskDelay(pdMS_TO_TICKS(10));
        } while(!gpio_get_level(VBAT_PWR_GPIO));
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO),
    pwr_button_(VBAT_PWR_GPIO) {
        Power_Init();
        InitBattery();     
        InitializeI2c();  
        InitializeLcdDisplay();
        if (!music_.Start([this]() { PowerOff(); })) {
            ESP_LOGE(TAG, "SD player task allocation failed");
        }
        InitializeButtons();
        InitializeTools();
    }

    bool CanStartVoiceShortcut() override {
        return !local_only_ && WifiStation::GetInstance().IsConnected();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(CustomBoard);