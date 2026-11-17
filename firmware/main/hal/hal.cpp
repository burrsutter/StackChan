/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include <driver/temperature_sensor.h>
#include <memory>
#include <algorithm>
#include <iterator>
#include <mooncake_log.h>
#include <nvs_flash.h>
#include <esp_attr.h>
#include <cstring>
#include <esp_system.h>

// Reset history kept in RTC memory that is not cleared on boot. It survives
// ESP32-only resets (brownout, panic, watchdog, software) but not a PMIC
// power-off, which the AXP2101 0x21 register already records. On a run with
// no serial link (base USB socket) this is the only evidence of why the
// chip restarted and how long the previous run lasted.
namespace {
constexpr uint32_t kResetHistMagic = 0x52535431;  // "RST1"
constexpr int kResetHistLen        = 8;
struct ResetHistory {
    uint32_t magic;
    uint32_t boots;
    uint32_t up_s;  // heartbeat: uptime of the current run, updated every 2 s
    uint8_t reason[kResetHistLen];
    uint32_t prev_up_s[kResetHistLen];
};
RTC_NOINIT_ATTR ResetHistory s_reset_hist;
int s_this_reason = -1;

const char* reset_reason_name(int r)
{
    switch (r) {
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_EXT: return "ext";
        case ESP_RST_SW: return "sw";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "jtag";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "pwr_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        default: return "unknown";
    }
}

void reset_history_on_boot()
{
    s_this_reason = static_cast<int>(esp_reset_reason());
    if (s_reset_hist.magic != kResetHistMagic) {
        // Cold start (or RTC memory lost): previous run length unknown.
        std::memset(&s_reset_hist, 0, sizeof(s_reset_hist));
        s_reset_hist.magic = kResetHistMagic;
        s_reset_hist.up_s  = UINT32_MAX;
    }
    for (int i = kResetHistLen - 1; i > 0; --i) {
        s_reset_hist.reason[i]    = s_reset_hist.reason[i - 1];
        s_reset_hist.prev_up_s[i] = s_reset_hist.prev_up_s[i - 1];
    }
    s_reset_hist.reason[0]    = static_cast<uint8_t>(s_this_reason);
    s_reset_hist.prev_up_s[0] = s_reset_hist.up_s;
    s_reset_hist.up_s         = 0;
    ++s_reset_hist.boots;
}

void reset_history_log()
{
    std::string hist;
    const int n = std::min<uint32_t>(s_reset_hist.boots, kResetHistLen);
    for (int i = 0; i < n; ++i) {
        if (i) hist += ' ';
        hist += reset_reason_name(s_reset_hist.reason[i]);
        hist += '@';
        hist += s_reset_hist.prev_up_s[i] == UINT32_MAX ? std::string("?")
                                                         : std::to_string(s_reset_hist.prev_up_s[i]) + "s";
    }
    // hist: newest first; "reason@N s" = this boot's cause after a run of N s.
    mclog::tagInfo("HAL", "resets: boots={} this={} prev_up={} hist=[{}]", s_reset_hist.boots,
                   reset_reason_name(s_this_reason),
                   s_reset_hist.prev_up_s[0] == UINT32_MAX ? std::string("?")
                                                           : std::to_string(s_reset_hist.prev_up_s[0]) + "s",
                   hist);
}
}  // namespace

static std::unique_ptr<Hal> _hal_instance;
static const std::string_view _tag = "HAL";

Hal& GetHAL()
{
    if (!_hal_instance) {
        mclog::tagInfo(_tag, "creating hal instance");
        _hal_instance = std::make_unique<Hal>();
    }
    return *_hal_instance.get();
}

void Hal::init()
{
    mclog::tagInfo(_tag, "init");
    reset_history_on_boot();
    reset_history_log();

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    xiaozhi_board_init();
    xiaozhi_mcp_init();
    head_touch_init();
    io_expander_init();
    rtc_init();
    imu_init();
    servo_init();
    lvgl_init();
}

/* -------------------------------------------------------------------------- */
/*                                   System                                   */
/* -------------------------------------------------------------------------- */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <system_info.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_mac.h>

void Hal::delay(std::uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

std::uint32_t Hal::millis()
{
    return esp_timer_get_time() / 1000;
}

void Hal::feedTheDog()
{
    vTaskDelay(1);
}

std::array<uint8_t, 6> Hal::getFactoryMac()
{
    std::array<uint8_t, 6> mac;
    esp_efuse_mac_get_default(mac.data());
    return mac;
}

std::string Hal::getFactoryMacString(std::string divider)
{
    auto mac = getFactoryMac();
    return fmt::format("{:02X}{}{:02X}{}{:02X}{}{:02X}{}{:02X}{}{:02X}", mac[0], divider, mac[1], divider, mac[2],
                       divider, mac[3], divider, mac[4], divider, mac[5]);
}

void Hal::reboot()
{
    esp_restart();
}

static void _confirm_ota_image_if_stable()
{
    constexpr uint32_t ota_confirm_delay_ms = 20000;
    static bool ota_confirm_checked         = false;
    if (ota_confirm_checked || GetHAL().millis() < ota_confirm_delay_ms) {
        return;
    }
    ota_confirm_checked = true;

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) {
        mclog::tagError(_tag, "failed to get running partition for ota confirmation");
        return;
    }

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        mclog::tagError(_tag, "failed to get ota state for partition: {}", running->label);
        return;
    }

    mclog::tagInfo(_tag, "ota confirm check: partition={}, state={}", running->label, static_cast<int>(ota_state));
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        mclog::tagInfo(_tag, "ota image is stable, marking current app valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

// ESP32-S3 on-die temperature. Separate from the PMIC's own die sensor: this
// one says how hot the SoC is, the PMIC's says how hot the power path is, and
// only having both distinguishes general board heat from a hot regulator.
static float _read_soc_temp_celsius()
{
    static temperature_sensor_handle_t handle = nullptr;
    if (handle == nullptr) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &handle) != ESP_OK) {
            return -1000.0f;
        }
        if (temperature_sensor_enable(handle) != ESP_OK) {
            temperature_sensor_uninstall(handle);
            handle = nullptr;
            return -1000.0f;
        }
    }
    float celsius = 0.0f;
    if (temperature_sensor_get_celsius(handle, &celsius) != ESP_OK) {
        return -1000.0f;
    }
    return celsius;
}

void Hal::updatePowerTelemetryLog()
{
    // Sample in the app loop at up to 10 Hz, retaining minima across each
    // two-second report. Count and maximum gap expose stalls in this loop;
    // even these samples cannot capture a regulator transient below 100 ms.
    static uint32_t last_sample_tick = 0;
    static uint32_t last_report_tick = 0;
    static uint32_t max_gap_ms       = 0;
    static uint32_t samples          = 0;
    static uint32_t read_errors      = 0;
    static int minima[3]            = {-1, -1, -1};
    static int previous_irq         = -1;
    const uint32_t now              = millis();
    if (last_sample_tick != 0 && now - last_sample_tick < 100) {
        return;
    }
    if (last_sample_tick != 0) {
        max_gap_ms = std::max(max_gap_ms, now - last_sample_tick);
    }
    last_sample_tick = now;

    int adc[4] = {-1, -1, -1, -1};
    hal_bridge::board_get_adc_block(adc);
    if (adc[0] >= 0) {
        const int volts[3] = {adc[0], adc[2], adc[3]};
        for (int i = 0; i < 3; ++i) {
            minima[i] = minima[i] < 0 ? volts[i] : std::min(minima[i], volts[i]);
        }
        ++samples;
    } else {
        ++read_errors;
    }
    if (last_report_tick != 0 && now - last_report_tick < 2000) {
        return;
    }
    last_report_tick = now;
    s_reset_hist.up_s = now / 1000;

    const int irq = hal_bridge::board_get_irq_status();
    if (irq != previous_irq) {
        mclog::tagInfo(_tag, "PMIC IRQ change: prev={} now={} (-1=read_error; flags retained)",
                       previous_irq, irq);
        previous_irq = irq;
    }
    mclog::tagInfo(_tag, "pwr up={}s batt={}% {} ext={} axp=0x{:04X} adc=[{},{},{},{}] "
                   "min_mv=[{},{},{}] samples={} read_errors={} max_gap_ms={} "
                   "irq={} die_raw={} soc_c={:.1f}", now / 1000,
                   getBatteryLevel(), isBatteryCharging() ? "chg" : "dis", isExternalPowerConnected() ? 1 : 0,
                   hal_bridge::board_get_power_status_regs(), adc[0], adc[1], adc[2], adc[3],
                   minima[0], minima[1], minima[2], samples, read_errors, max_gap_ms, irq,
                   hal_bridge::board_get_die_temp_raw(), _read_soc_temp_celsius());
    std::fill(std::begin(minima), std::end(minima), -1);
    samples = read_errors = max_gap_ms = 0;
}

void Hal::updateHeapStatusLog()
{
    _confirm_ota_image_if_stable();

    updatePowerTelemetryLog();

    static uint32_t last_log_tick = 0;
    if (millis() - last_log_tick < 10000) {
        return;
    }
    last_log_tick = millis();
    SystemInfo::PrintHeapStats();
    reset_history_log();
    // External power is logged alongside the charge state because they
    // disagree in the case that matters: a full battery on USB reads
    // "discharging" with the cable still in, and that disagreement used to
    // arm the idle shutdown. The raw AXP2101 status registers make the
    // decision auditable from the log alone.
    mclog::tagInfo(_tag, "battery: {}% {} external_power={} axp_status=0x{:04X}", getBatteryLevel(),
                   isBatteryCharging() ? "charging" : "discharging", isExternalPowerConnected() ? 1 : 0,
                   hal_bridge::board_get_power_status_regs());
}

/* -------------------------------------------------------------------------- */
/*                                   Xiaozhi                                  */
/* -------------------------------------------------------------------------- */
#include "board/hal_bridge.h"
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <assets/assets.h>

void Hal::xiaozhi_board_init()
{
    mclog::tagInfo(_tag, "xiaozhi board init");

    hal_bridge::xiaozhi_board_init();
}

static void _stackchan_update_task(void* param)
{
    bool is_setup_done = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));

        tools::update_reminders();

        LvglLockGuard lock;

        if (!hal_bridge::is_xiaozhi_idle()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        GetStackChan().update();

        if (!hal_bridge::is_xiaozhi_ready()) {
            continue;
        }

        if (!is_setup_done) {
            // Setup when xiaozhi ready
            GetHAL().startSntp();
            view::create_home_indicator([]() { GetHAL().requestWarmReboot(0); }, 0x81DBBD, 0x134233);
            view::create_status_bar(0x81DBBD, 0x134233);
            is_setup_done = true;
        }

        view::update_home_indicator();
        view::update_status_bar();
    }
}

void Hal::startXiaozhi()
{
    mclog::tagInfo(_tag, "start xiaozhi");

    auto& motion = GetStackChan().motion();
    motion.setAutoAngleSyncEnabled(true);
    motion.setAutoTorqueReleaseEnabled(true);

    // Setup reminder handler
    tools::on_reminder_triggered().clear();
    tools::on_reminder_triggered().connect([](int id, std::string_view msg) {
        mclog::tagInfo(_tag, "reminder triggered: id: {}, msg: {}", id, msg);
        {
            LvglLockGuard lock;
            auto& avatar = GetStackChan().avatar();
            avatar.addDecorator(std::make_unique<view::ReminderView>(lv_screen_active(), msg));
        }
        hal_bridge::app_play_sound(OGG_NEW_NOTIFICATION);
    });

    // Start stackchan update task
    xTaskCreatePinnedToCore(_stackchan_update_task, "stackchan", 4096, NULL, 3, NULL, 1);

    hal_bridge::start_xiaozhi_app();
}

XiaozhiConfig_t Hal::getXiaozhiConfig()
{
    auto bridge_config = hal_bridge::get_xiaozhi_config();
    return XiaozhiConfig_t{
        .idleShutdownTimeSeconds   = bridge_config.idleShutdownTimeSeconds,
        .allowShutdownWhenCharging = bridge_config.allowShutdownWhenCharging,
        .idleRandomMovementLevel   = bridge_config.idleRandomMovementLevel,
        .startAiAgentOnBoot        = bridge_config.startAiAgentOnBoot,
    };
}

void Hal::setXiaozhiConfig(XiaozhiConfig_t config)
{
    hal_bridge::set_xiaozhi_config({
        .idleShutdownTimeSeconds   = config.idleShutdownTimeSeconds,
        .allowShutdownWhenCharging = config.allowShutdownWhenCharging,
        .idleRandomMovementLevel   = config.idleRandomMovementLevel,
        .startAiAgentOnBoot        = config.startAiAgentOnBoot,
    });
}

uint8_t Hal::getBatteryLevel()
{
    return hal_bridge::board_get_battery_level();
}

bool Hal::isBatteryCharging()
{
    return hal_bridge::board_is_battery_charging();
}

bool Hal::isExternalPowerConnected()
{
    return hal_bridge::board_is_external_power_connected();
}

void Hal::notifyUserInteraction()
{
    hal_bridge::board_notify_user_interaction();
}

void Hal::factoryReset()
{
    mclog::tagInfo(_tag, "start factory reset");
    ESP_ERROR_CHECK(nvs_flash_erase());
    reboot();
}

/* -------------------------------------------------------------------------- */
/*                                   Display                                  */
/* -------------------------------------------------------------------------- */
#include "board/hal_bridge.h"

void Hal::lvglLock()
{
    hal_bridge::disply_lvgl_lock();
}

void Hal::lvglUnlock()
{
    hal_bridge::disply_lvgl_unlock();
}

void Hal::setBackLightBrightness(uint8_t brightness, bool permanent)
{
    hal_bridge::board_set_backlight_brightness(brightness, permanent);
}

uint8_t Hal::getBackLightBrightness()
{
    return hal_bridge::board_get_backlight_brightness();
}

void Hal::setSpeakerVolume(uint8_t volume, bool permanent)
{
    hal_bridge::board_set_speaker_volume(volume, permanent);
}

uint8_t Hal::getSpeakerVolume()
{
    return hal_bridge::board_get_speaker_volume();
}

/* -------------------------------------------------------------------------- */
/*                                    Lvgl                                    */
/* -------------------------------------------------------------------------- */
#include "board/hal_bridge.h"
#include <stackchan/stackchan.h>

static void lvgl_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    hal_bridge::lock();
    auto& bridge_data = hal_bridge::get_data();

    // mclog::tagInfo(_tag, "touchpoint: {}, x: {}, y: {}", bridge_data.touchPoint.num, bridge_data.touchPoint.x,
    //                bridge_data.touchPoint.y);

    if (bridge_data.touchPoint.num == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
    } else {
        data->state   = LV_INDEV_STATE_PRESSED;
        data->point.x = bridge_data.touchPoint.x;
        data->point.y = bridge_data.touchPoint.y;
    }

    hal_bridge::unlock();
}

void Hal::lvgl_init()
{
    mclog::tagInfo(_tag, "lvgl init");

    hal_bridge::disply_lvgl_lock();

    mclog::tagInfo(_tag, "create lvgl touchpad indev");
    lvTouchpad = lv_indev_create();
    lv_indev_set_type(lvTouchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvTouchpad, lvgl_read_cb);
    lv_indev_set_group(lvTouchpad, lv_group_get_default());
    lv_indev_set_display(lvTouchpad, hal_bridge::display_get_lvgl_display());

    hal_bridge::disply_lvgl_unlock();
}

/* -------------------------------------------------------------------------- */
/*                                 Warm Reboot                                */
/* -------------------------------------------------------------------------- */
#include <settings.h>
#include <string_view>

static std::string_view _warm_boot_nvs_ns  = "warm_boot";
static std::string_view _warm_boot_nvs_key = "app_index";

void Hal::requestWarmReboot(int appIndex)
{
    mclog::tagInfo(_tag, "warm reboot request to app index: {}", appIndex);

    {
        Settings settings(_warm_boot_nvs_ns.data(), true);
        settings.SetInt(_warm_boot_nvs_key.data(), appIndex);
    }

    delay(100);
    esp_restart();
}

int Hal::getWarmRebootTarget()
{
    Settings settings(_warm_boot_nvs_ns.data(), false);
    return settings.GetInt(_warm_boot_nvs_key.data(), -1);
}

void Hal::clearWarmRebootRequest()
{
    mclog::tagInfo(_tag, "clear warm reboot request");

    Settings settings(_warm_boot_nvs_ns.data(), true);
    settings.SetInt(_warm_boot_nvs_key.data(), -1);
}
