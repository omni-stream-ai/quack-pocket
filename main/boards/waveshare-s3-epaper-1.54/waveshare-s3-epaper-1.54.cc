#include "voice/control_policy.h"
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
#include "standby.h"
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include <esp_wifi.h>
#include <ssid_manager.h>
#include <font_awesome.h>
#include "mcp_server.h"
#include "music_player.h"
#include "pocket_checklist.h"
#include "pocket_memo.h"
#include "pocket_media.h"
#include "phone_setup.h"
#include "setup_ble.h"
#include "setup_access.h"
#include "pocket_update.h"
#include <esp_app_desc.h>
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
    enum class NetworkPage { Hidden, Disconnected, Home };
    std::atomic<NetworkPage> network_page_{NetworkPage::Hidden};

    std::atomic<bool> menu_open_{false};
    size_t group_ = 0;
    size_t item_ = 0;
    bool in_group_ = false;
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
    enum MenuItem { Play, Next, Sequential, Shuffle, VolumeDown, VolumeUp, Refresh, Story, Favorites,
                    Chat, OfflineVoice, InstallModel, WakeSetting, RecordMemo, HearMemo,
                    Phone, LanSetup, Bluetooth, Trusted, StandbyTimeout, Update, Help, Checklist, ItemCount };
    struct MenuEntry { const char* title; const char* icon; };
    static constexpr MenuEntry items_[ItemCount] = {
        {"播放 / 暂停", FONT_AWESOME_MUSIC}, {"下一首", FONT_AWESOME_FORWARD_STEP},
        {"顺序播放", FONT_AWESOME_ARROWS_REPEAT}, {"随机播放", FONT_AWESOME_MUSIC},
        {"调小音量", FONT_AWESOME_VOLUME_LOW}, {"调大音量", FONT_AWESOME_VOLUME_HIGH},
        {"刷新曲库", FONT_AWESOME_ARROWS_ROTATE}, {"继续听故事", FONT_AWESOME_MUSIC}, {"我的收藏", FONT_AWESOME_MUSIC},
        {"语音通话", FONT_AWESOME_COMMENT}, {"说一句音乐口令", FONT_AWESOME_MICROPHONE},
        {"安装离线模型", FONT_AWESOME_DOWNLOAD}, {"语音唤醒", FONT_AWESOME_MICROPHONE}, {"录一条留言", FONT_AWESOME_MICROPHONE}, {"听最近留言", FONT_AWESOME_COMMENT},
        {"手机配网与模型", FONT_AWESOME_WIFI}, {"同网扫码管理", FONT_AWESOME_WIFI},
        {"蓝牙开关", FONT_AWESOME_GEAR}, {"已授权设备", FONT_AWESOME_GEAR}, {"自动待机时间", FONT_AWESOME_CLOCK},
        {"检查更新", FONT_AWESOME_DOWNLOAD}, {"使用帮助", FONT_AWESOME_CIRCLE_QUESTION}, {"口袋清单", FONT_AWESOME_GEAR}
    };
    struct MenuGroup { const char* title; const char* icon; const char* hint; MenuItem first, last; };
    static constexpr MenuGroup groups_[] = {
        {"音乐播放", FONT_AWESOME_MUSIC, "播放 · 音量 · 曲库", Play, Favorites},
        {"语音互动", FONT_AWESOME_MICROPHONE, "对话 · 口令 · 唤醒", Chat, HearMemo},
        {"手机与网络", FONT_AWESOME_WIFI, "配网 · 扫码 · 授权", Phone, Trusted},
        {"设备设置", FONT_AWESOME_GEAR, "待机 · 更新 · 帮助", StandbyTimeout, Checklist}
    };

    void MoveMenu(bool previous) {
        const auto& group = groups_[group_];
        size_t first = in_group_ ? static_cast<size_t>(group.first) : 0;
        size_t count = in_group_ ? group.last - group.first + 1 : std::size(groups_);
        size_t& cursor = in_group_ ? item_ : group_;
        cursor = first + (cursor - first + (previous ? count - 1 : 1)) % count;
        RenderMenu();
    }

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

    void RenderMenu() {
        int level;
        bool charging, discharging;
        std::string battery = GetBatteryLevel(level, charging, discharging)
            ? std::to_string(level) + "%" : "--%";
        const auto& group = groups_[group_];
        const size_t page = in_group_ ? item_ - group.first + 1 : group_ + 1;
        const size_t count = in_group_ ? group.last - group.first + 1 : std::size(groups_);
        std::string header = std::string(in_group_ ? group.title : "主菜单") + "   " +
            std::to_string(page) + "/" + std::to_string(count) + "      " + battery;
        if (!in_group_) {
            display_->ShowMenu(header, group.icon, group.title, group.hint);
            return;
        }
        auto title = items_[item_].title;
        if (item_ == Chat && WifiStation::GetInstance().IsConnected() &&
            !Application::GetInstance().IsVoicePaused()) title = "退出语音通话";
        std::string detail;
        if (item_ == VolumeDown || item_ == VolumeUp)
            detail = "当前音量 " + std::to_string(GetAudioCodec()->output_volume());
        if (item_ == Bluetooth) detail = SetupBluetoothEnabled() ? "已开启，确认关闭" : "已关闭，确认开启";
        if (item_ == WakeSetting) detail = wake_allowed_ ? "已开启，确认关闭" : "已关闭，确认开启";
        if (item_ == StandbyTimeout) detail = standby::configured_minutes ?
            std::to_string(standby::configured_minutes.load()) + (wake_allowed_ ? " 分钟 · 确认更改" : " 分钟 · 需开语音唤醒") : "关闭 · 确认更改";
        if (item_ == Update) detail = std::string("当前版本 ") + esp_app_get_description()->version;
        display_->ShowMenu(header, items_[item_].icon, title, detail);
    }

    std::atomic<bool> lan_switching_{false};
    int lan_option_ = -1;
    void ShowLanOptions() {
        bool background = BackgroundManagementEnabled();
        display_->ShowMenu("同网扫码管理", FONT_AWESOME_WIFI,
            lan_option_ == 0 ? "显示管理二维码" : "后台设置服务",
            lan_option_ == 0 ? "BOOT 打开本次管理" : (background ? "已开启 · BOOT 关闭" : "已关闭 · BOOT 开启"),
            "PWR 切换 · 长按 BOOT 返回");
    }
    bool LanButton(bool confirm) {
        if (lan_option_ < 0) return false;
        if (lan_switching_) return true;
        if (!confirm) {
            lan_option_ = 1 - lan_option_;
            ShowLanOptions();
        } else if (lan_option_ == 0) {
            if (!WifiStation::GetInstance().IsConnected()) {
                display_->ShowNotification("请先连接家庭 Wi-Fi");
            } else {
                { Settings mode("omni_mode",true); mode.SetInt("lan_setup",1); }
                esp_restart();
            }
        } else {
            // TLS startup and shutdown must not block the button timer task.
            lan_switching_ = true;
            auto switch_service = [](void* arg) {
                auto* board = static_cast<CustomBoard*>(arg);
                bool ok = ApplyBackgroundManagement(board->display_,!BackgroundManagementEnabled());
                board->ShowLanOptions();
                if (!ok) board->display_->ShowNotification("后台服务切换失败，请重试");
                board->lan_switching_ = false;
                vTaskDelete(nullptr);
            };
            if (xTaskCreate(switch_service,"lan_switch",8192,this,3,nullptr)!=pdPASS) {
                lan_switching_ = false;
                display_->ShowNotification("后台服务正忙，请重试");
            }
        }
        return true;
    }

    int peer_index_ = -1;
    bool confirm_remove_ = false;
    std::vector<SetupPeer> peers_;
    int favorite_index_ = -1;
    void FavoritePage(bool play, bool next) {
        auto favorites = pocket_media::Load("favorites");
        int count = cJSON_GetArraySize(favorites.get());
        if (!count) { favorite_index_ = -1; RenderMenu(); display_->ShowVoiceHint("请先在手机上收藏歌曲", 4000); return; }
        favorite_index_ = (favorite_index_ + next) % count;
        auto* item = cJSON_GetArrayItem(favorites.get(),favorite_index_);
        if (!cJSON_IsString(item)) { favorite_index_ = -1; RenderMenu(); return; }
        if (play) {
            if (!music_.Request(MusicAction::PlayFile,item->valuestring)) { display_->ShowNotification("播放器正忙"); return; }
            favorite_index_ = -1; menu_open_=false; display_->CloseMenu();
        } else display_->ShowUpdatePage("我的收藏 " + std::to_string(favorite_index_+1) + "/" +
            std::to_string(count) + "\n\n" + item->valuestring + "\n\nBOOT 播放 · PWR 下一首\n长按 BOOT 返回");
    }
    int checklist_index_ = -1;
    void ChecklistPage(bool toggle, bool next) {
        std::lock_guard<std::mutex> guard(pocket_list::lock);
        pocket_list::List items;
        auto result = pocket_list::Read(items);
        if (result != ESP_OK || items.empty()) {
            checklist_index_ = -1;
            RenderMenu();
            display_->ShowVoiceHint(result == ESP_OK ? "请先在手机端设置清单" : "清单读取失败，请重试", 4000);
            return;
        }
        checklist_index_ = (checklist_index_ + static_cast<int>(next)) % items.size();
        auto& item = items[checklist_index_];
        if (toggle) {
            item.checked = !item.checked;
            if (pocket_list::Write(items) != ESP_OK) {
                display_->ShowNotification("保存失败，请重试");
                return;
            }
        }
        display_->ShowUpdatePage("口袋清单 " + std::to_string(checklist_index_ + 1) +
            "/" + std::to_string(items.size()) + "\n\n" + (item.checked ? "已完成 " : "待完成 ") +
            item.text + "\n\nBOOT 切换 · PWR 下一项\n长按 BOOT 返回");
    }
    void ShowPeers() {
        if (peer_index_ == static_cast<int>(peers_.size())) {
            display_->ShowMenu("已授权设备", FONT_AWESOME_ARROW_LEFT, "返回",
                peers_.empty() ? "尚未授权浏览器" : "BOOT 返回 · PWR 翻页", "PWR 短按翻页");
            return;
        }
        const auto& peer = peers_[peer_index_];
        std::string detail = peer.id.substr(0,8) + (confirm_remove_ ? " · BOOT 确认删除" : " · BOOT 删除");
        display_->ShowMenu(confirm_remove_ ? "删除授权？PWR 取消" : "已授权设备 · PWR 翻页",
                           FONT_AWESOME_GEAR,peer.name.c_str(),detail, confirm_remove_ ? "PWR 短按取消" : "PWR 短按翻页");
    }
    bool PeerButton(bool confirm) {
        if (peer_index_ < 0) return false;
        if (!confirm && !confirm_remove_) {
            peer_index_ = (peer_index_ + 1) % (peers_.size()+1);
        } else if (!confirm) {
            confirm_remove_=false;
        } else if (peer_index_ == static_cast<int>(peers_.size())) {
            peer_index_=-1; RenderMenu(); return true;
        } else if (confirm_remove_) {
            bool removed=RemoveSetupPeer(peers_[peer_index_].id);
            peers_=SetupPeers();peer_index_=0;confirm_remove_=false;
            if (!removed) display_->ShowNotification("删除失败，请重试");
        } else {
            confirm_remove_=true;
        }
        ShowPeers();
        return true;
    }

    void ToggleMenu() {
        if (menu_open_ && in_group_) {
            in_group_ = false;
            RenderMenu();
        } else if (menu_open_) {
            menu_open_ = false;
            display_->CloseMenu();
        } else {
            group_ = 0;
            in_group_ = false;
            menu_open_ = true;
            display_->ShowVoiceHint("", 0);
            RenderMenu();
        }
    }

    std::atomic<bool> wake_allowed_{true};
    std::mutex standby_mutex_;
    standby::Timer standby_timer_;
    wifi_ps_type_t previous_wifi_ps_ = WIFI_PS_MIN_MODEM;
    bool restore_wifi_ps_ = false;
    static uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
    enum class UpdateState { Closed, Checking, Confirm, Installing, Result };
    std::atomic<UpdateState> update_{UpdateState::Closed};
    pocket_update::Release release_;

    void UpdateResult(const std::string& message) {
        display_->ShowUpdatePage("检查更新\n\n" + message + "\n\n按 BOOT 或 PWR 返回");
        update_ = UpdateState::Result;
    }

    void RunUpdate(bool install) {
        if (!WifiStation::GetInstance().IsConnected()) { UpdateResult("请先连接 Wi-Fi"); return; }
        if (install) {
            if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle || music_.IsBusy()) {
                UpdateResult("请结束通话或暂停音乐后重试");
                return;
            }
            auto error = pocket_update::Install(release_, [this](int percent) {
                display_->ShowUpdatePage("正在安装 " + release_.version + "\n\n" +
                    std::to_string(percent) + "%\n请保持供电");
            });
            if (!error.empty()) { UpdateResult(error); return; }
            display_->ShowUpdatePage("更新完成\n\n正在重新启动");
            vTaskDelay(pdMS_TO_TICKS(800));
            esp_restart();
        }
        auto error = pocket_update::Check(release_);
        if (!error.empty()) { UpdateResult(error); return; }
        std::string current = esp_app_get_description()->version;
        if (release_.version.empty()) { UpdateResult("已是最新版本 " + current); return; }
        display_->ShowUpdatePage("发现新版本\n\n当前 " + current + "\n新版 " + release_.version +
                                 "\n\n短按 BOOT 安装\n短按 PWR 取消");
        update_ = UpdateState::Confirm;
    }

    void StartUpdate(bool install) {
        update_ = install ? UpdateState::Installing : UpdateState::Checking;
        display_->ShowUpdatePage(install ? "准备安装，请保持供电" : "正在检查新版本…");
        if (xTaskCreate([](void* ctx) {
                auto* self = static_cast<CustomBoard*>(ctx);
                self->RunUpdate(self->update_ == UpdateState::Installing);
                vTaskDelete(nullptr);
            }, "pocket_ota", 12288, this, 3, nullptr) != pdPASS)
            UpdateResult("内存不足，请稍后重试");
    }

    bool UpdateButton(bool confirm) {
        auto state = update_.load();
        if (state == UpdateState::Closed) return false;
        if (state == UpdateState::Confirm && confirm) StartUpdate(true);
        else if (state == UpdateState::Confirm || state == UpdateState::Result) {
            update_ = UpdateState::Closed;
            RenderMenu();
        }
        return true;
    }

    void SelectItem() {
        if (!in_group_) {
            in_group_ = true;
            item_ = groups_[group_].first;
            RenderMenu();
            return;
        }
        if (item_ == Favorites) { favorite_index_ = 0; FavoritePage(false,false); return; }
        if (item_ == Checklist) { checklist_index_ = 0; ChecklistPage(false, false); return; }
        if (item_ == Update) { StartUpdate(false); return; }
        if (item_ == Bluetooth) {
            bool enable=!SetupBluetoothEnabled();
            if (!SetSetupBluetoothEnabled(enable)) display_->ShowNotification("蓝牙切换失败，请重试");
            RenderMenu(); return;
        }
        if (item_ == LanSetup) { lan_option_ = 0; ShowLanOptions(); return; }
        if (item_ == Trusted) {
            peers_ = SetupPeers(); peer_index_=0; confirm_remove_=false; ShowPeers(); return;
        }
        if (item_ == StandbyTimeout) {
            standby::configured_minutes = standby::Next(standby::configured_minutes);
            Settings("omni_ui", true).SetInt("standby_min", standby::configured_minutes);
            RenderMenu();
            return;
        }
        if (item_ == WakeSetting) {
            wake_allowed_ = !wake_allowed_;
            Settings("omni_ui", true).SetInt("wake", wake_allowed_);
            auto& app = Application::GetInstance();
            app.Schedule([allowed = wake_allowed_.load()]() {
                auto& app = Application::GetInstance();
                auto& audio = app.GetAudioService();
                audio.SetWakeWordAllowed(allowed);
                if (allowed && (app.GetDeviceState() == kDeviceStateIdle || app.GetDeviceState() == kDeviceStateWifiConfiguring)) audio.EnableWakeWordDetection(true);
            });
            display_->RefreshHome();
            RenderMenu();
            return;
        }
        if (item_ == VolumeDown || item_ == VolumeUp) {
            auto* codec = GetAudioCodec();
            codec->SetOutputVolume(std::clamp(codec->output_volume() + (item_ == VolumeUp ? 10 : -10), 0, 100));
            RenderMenu();
            return;
        }
        menu_open_ = false;
        display_->CloseMenu();
        switch (item_) {
            case Chat: {
                if (!WifiStation::GetInstance().IsConnected()) {
                    display_->ShowVoiceHint("语音通话需要 Wi-Fi。可在菜单打开手机配网", 4500);
                    break;
                }
                auto& app = Application::GetInstance();
                app.SetVoicePaused(!app.IsVoicePaused());
                break;
            }
            case RecordMemo: RequestMusicFromButton(MusicAction::RecordMemo); break;
            case HearMemo: RequestMusicFromButton(MusicAction::HearMemo); break;
            case Story: RequestMusicFromButton(MusicAction::Story); break;
            case Play: RequestMusicFromButton(MusicAction::Toggle); break;
            case Next: RequestMusicFromButton(MusicAction::Next); break;
            case Sequential: RequestMusicFromButton(MusicAction::Sequential); break;
            case Shuffle: RequestMusicFromButton(MusicAction::Shuffle); break;
            case Refresh: RequestMusicFromButton(MusicAction::Refresh); break;
            case InstallModel:
                if (WifiStation::GetInstance().IsConnected()) {
                    RequestMusicFromButton(MusicAction::InstallModel);
                } else {
                    display_->ShowNotification("请连接 Wi-Fi 后再安装模型");
                }
                break;
            case OfflineVoice:
                RequestMusicFromButton(MusicAction::Listen);
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
        InitializeSetupBluetooth(display_);
        int timeout = Settings("omni_ui").GetInt("standby_min", standby::default_minutes);
        standby::configured_minutes = standby::Valid(timeout) ? timeout : standby::default_minutes;
        wake_allowed_ = Settings("omni_ui").GetInt("wake", 1);
        Application::GetInstance().GetAudioService().SetWakeWordAllowed(wake_allowed_);
        if (!Settings("omni_ui").GetInt("guided", 0)) {
            guide_waiter_ = xTaskGetCurrentTaskHandle();
            BeginGuide();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            Settings guide("omni_ui", true);
            guide.SetInt("guided", 1);
        }
        if (!VoiceProfile::ApplyPendingWifi()) {
            WaitForSetupButton("网络设置读写失败。");
            return;
        }
        if (!wifi_config_mode_ && SsidManager::GetInstance().GetSsidList().empty()) {
            EnableOfflineSetupScan();
            Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
            network_page_ = NetworkPage::Home;
            display_->ShowDisconnectedHome(false);
            Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
            // Adding a network from phone setup restarts the normal boot path.
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            return;
        }
        WifiBoard::StartNetwork();
        if (Settings("omni_mode").GetInt("lan_setup",0)) {
            { Settings mode("omni_mode",true); mode.SetInt("lan_setup",0); }
            Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
            RunPhoneSetup(display_,true);
        }
        StartBackgroundManagement(display_);
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
            Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
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
                if (page == NetworkPage::Home) display_->ShowDisconnectedHome(true);
                else display_->ShowNetworkPage("Wi-Fi 未连接\n\n正在寻找已保存的网络\n\n短按 BOOT 返回首页\n短按 PWR 进入手机配网");
                rendered = page;
            }
        } while (!station.WaitForConnected(250));
        network_page_ = NetworkPage::Hidden;
        display_->ShowNetworkPage(nullptr);
    }

    void MonitorWifi() {
        for (;;) {
            auto& station = WifiStation::GetInstance();
            if (!station.IsConnected()) {
                if (!station.WaitForConnected(3000)) WaitForWifiRecovery();
                StartBackgroundManagement(display_);
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
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
            if (music_.FinishRecording()) return;
            if (WakeFromVoiceStandby()) return;
            if (favorite_index_ >= 0) { FavoritePage(true,false); return; }
            if (checklist_index_ >= 0) { ChecklistPage(true, false); return; }
            if (UpdateButton(true) || ShowPhoneSetupIfActive(true) || LanButton(true) || PeerButton(true)) return;
            if (!AcceptUserAction()) return;
            if (GuideButton(false)) return;
            if (display_->WakeScreen()) return;
            if (menu_open_) { SelectItem(); return; }
            if (PocketMemoUnread() && (network_page_ == NetworkPage::Home || (network_page_ == NetworkPage::Hidden && Application::GetInstance().GetDeviceState() == kDeviceStateIdle))) { RequestMusicFromButton(MusicAction::HearMemo); return; }
            auto expected = NetworkPage::Disconnected;
            if (network_page_.compare_exchange_strong(expected, NetworkPage::Home)) return;
            if (expected == NetworkPage::Home) { RequestMusicFromButton(MusicAction::Listen); return; }
            if (auto task = setup_waiter_.load()) {
                xTaskNotifyGive(task);
                return;
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            if (app.IsVoicePaused()) app.SetVoicePaused(false);
            else app.ToggleChatState();
        });

        pwr_button_.OnClick([this]() {
            if (WakeFromVoiceStandby()) return;
            if (favorite_index_ >= 0) { FavoritePage(false,true); return; }
            if (checklist_index_ >= 0) { ChecklistPage(false, true); return; }
            if (UpdateButton(false) || ShowPhoneSetupIfActive(false) || LanButton(false) || PeerButton(false)) return;
            if (!AcceptUserAction()) return;
            if (GuideButton(true)) return;
            if (display_->WakeScreen()) return;
            if (menu_open_) MoveMenu(false);
            else if (network_page_ == NetworkPage::Disconnected) ResetWifiConfiguration();
            else RequestMusicFromButton(MusicAction::Toggle);
        });
        pwr_button_.OnDoubleClick([this]() {
            if (PhoneSetupActive()) return;
            if (lan_option_ >= 0 || peer_index_ >= 0 || favorite_index_ >= 0 || checklist_index_ >= 0) return;
            if (!AcceptUserAction()) return;
            if (guide_page_ >= 0) return;
            if (menu_open_) MoveMenu(true);
            else RequestMusicFromButton(MusicAction::Next);
        });
        boot_button_.OnLongPress([this]() {
            if (PhoneSetupActive()) return;
            if (!AcceptUserAction()) return;
            if (favorite_index_ >= 0) { favorite_index_=-1; RenderMenu(); return; }
            if (checklist_index_ >= 0) { checklist_index_=-1; RenderMenu(); return; }
            if (lan_switching_) return;
            if (lan_option_ >= 0) { lan_option_=-1; RenderMenu(); return; }
            if (peer_index_ >= 0) { peer_index_=-1; RenderMenu(); return; }
            if (!GuideButton(true)) ToggleMenu(); });
        boot_button_.OnDoubleClick([this]() {
            if (PhoneSetupActive()) return;
            if (lan_option_ >= 0 || peer_index_ >= 0 || favorite_index_ >= 0 || checklist_index_ >= 0) return;
            if (!AcceptUserAction()) return;
            if (guide_page_ < 0 && !menu_open_) RequestMusicFromButton(MusicAction::ChangeMode); });

        boot_button_.OnMultipleClick([this]() {
            if (PhoneSetupActive()) return;
            if (lan_option_ >= 0 || peer_index_ >= 0 || favorite_index_ >= 0 || checklist_index_ >= 0) return;
            if (!AcceptUserAction()) return;
            if (guide_page_ < 0 && !menu_open_) RequestMusicFromButton(MusicAction::Refresh); }, 3);

        pwr_button_.OnLongPress([this]() {
            if (!AcceptUserAction()) return;
            if (!music_.RequestPowerOff()) {
                Application::GetInstance().Schedule([this]() { PowerOff(); });
            }
        });
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.voice.enter_local_mode", "用户明确说返回首页、退出语音通话或结束语音服务时调用。关闭当前语音通话并回到首页，保留 Wi-Fi。", PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                Application::GetInstance().SetVoicePaused(true);
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
        display_->ShowShutdownSaver();
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

    bool FirmwareUpdateActive() const override { return update_ != UpdateState::Closed; }

    bool AcceptUserAction() override {
        if (FirmwareUpdateActive()) return false;
        std::lock_guard<std::mutex> lock(standby_mutex_);
        return standby_timer_.Input(NowMs());
    }

    void HandleVoiceTranscript(const std::string& transcript) override {
        std::string phrase = transcript;
        for (auto suffix : {"。","！","？",".","!","?"}) {
            size_t n = strlen(suffix);
            if (phrase.size() >= n && phrase.compare(phrase.size()-n,n,suffix) == 0)
                phrase.resize(phrase.size()-n);
        }
        auto policy = voice_control::Load();
        auto skills = cJSON_GetObjectItem(policy.get(), "skills");
        cJSON* skill;
        cJSON_ArrayForEach(skill, skills) {
            auto trigger = cJSON_GetObjectItem(skill, "phrase");
            auto action = cJSON_GetObjectItem(skill, "action");
            if (!cJSON_IsString(trigger) || phrase != trigger->valuestring || !cJSON_IsString(action)) continue;
            std::string group = action->valuestring;
            if (!cJSON_IsTrue(cJSON_GetObjectItem(policy.get(),group.c_str()))) return;
            auto arg = cJSON_GetObjectItem(skill,"argument");
            if (group == "state") Application::GetInstance().SetVoicePaused(true);
            if (group == "volume") {
                auto* codec = GetAudioCodec();
                codec->SetOutputVolume(cJSON_IsNumber(arg) ? arg->valueint : std::min(100, codec->output_volume()+10));
            }
            if (group == "music" && (cJSON_IsString(arg) ? music_.Request(MusicAction::PlayFile,arg->valuestring)
                                                         : music_.Request(MusicAction::Sequential)))
                Application::GetInstance().FinishChatForMusic();
            return;
        }
    }

    void CheckVoiceStandby() override {
        if (ManagementInUse()) {
            WakeFromVoiceStandby();
            return;
        }
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        auto state = app.GetDeviceState();
        bool home = state == kDeviceStateIdle ||
            (state == kDeviceStateWifiConfiguring && network_page_ != NetworkPage::Hidden);
        bool ready = !FirmwareUpdateActive() && home && guide_page_ < 0 && wake_allowed_ && audio.IsWakeWordRunning() &&
            !audio.IsAudioProcessorRunning() && audio.IsIdle() && !music_.IsBusy();
        std::lock_guard<std::mutex> lock(standby_mutex_);
        auto now = NowMs();
        if (!standby_timer_.Tick(now, standby::configured_minutes, ready)) return;
        if (!display_->ShowVoiceStandby(true)) {
            standby_timer_.Wake(now);
            return;
        }
        restore_wifi_ps_ = WifiStation::GetInstance().IsConnected() &&
            esp_wifi_get_ps(&previous_wifi_ps_) == ESP_OK && esp_wifi_set_ps(WIFI_PS_MAX_MODEM) == ESP_OK;
    }

    bool WakeFromVoiceStandby() override {
        std::lock_guard<std::mutex> lock(standby_mutex_);
        if (!standby_timer_.Asleep()) {
            standby_timer_.Input(NowMs());
            return false;
        }
        if (!display_->ShowVoiceStandby(false)) return true;
        standby_timer_.Wake(NowMs());
        if (restore_wifi_ps_) {
            esp_wifi_set_ps(previous_wifi_ps_);
            restore_wifi_ps_ = false;
        }
        return true;
    }

    void StartOfflineVoiceCommand() override {
        RequestMusicFromButton(MusicAction::Listen);
    }

    bool CanStartVoiceShortcut() override {
        return WifiStation::GetInstance().IsConnected();
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