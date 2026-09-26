/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_dance.h"
#include "dance_params.h"
#include <hal/hal.h>
#include <algorithm>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <assets/assets.h>
#include <hal/board/hal_bridge.h>

// ---------------------------------------------------------------------------
// TEMPORARY DIAGNOSTIC -- set back to 0 to restore normal DANCE behaviour.
//
// Tap the touchscreen to advance one LED at a time, 1..12 and back to 1, so
// each position can be inspected at your own pace instead of a 700ms auto
// walk. Only the selected LED is lit, in white. The spec says two rows of six
// (WS2812C x12); only 0-5 have ever lit on this unit, and because WS2812s are
// daisy-chained a break at index 6 would kill everything after it.
// ---------------------------------------------------------------------------
#define DIAG_LED_TAP 0

using namespace mooncake;
using namespace stackchan;

AppDance::AppDance()
{
    // 配置 App 名
    setAppInfo().name = "DANCE";
    // 配置 App 图标
    static auto icon  = assets::get_image("icon_dance.bin");
    setAppInfo().icon = (void*)&icon;
    // 配置 App 主题颜色
    static uint32_t theme_color = 0xB77BFF;
    setAppInfo().userData       = (void*)&theme_color;
}

// App 被安装时会被调用
void AppDance::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppDance::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    // Create loading page
    std::unique_ptr<view::LoadingPage> loading_page;
    {
        LvglLockGuard lock;
        loading_page = std::make_unique<view::LoadingPage>(0xB77BFF, 0x422268);
        loading_page->setMessage("Starting\n BLE server...");
    }

    // Start BLE service
    GetHAL().startBleServer();

    LvglLockGuard lock;

    // Destroy loading page
    loading_page.reset();

    // Create default avatar. Magenta face marks Dance mode at a glance
    // (Companion is purple, ESP-NOW remote is yellow)
    auto avatar            = std::make_unique<avatar::DefaultAvatar>();
    avatar->secondaryColor = lv_color_hex(0xC2158A);
    avatar->init(lv_screen_active());
    GetStackChan().attachAvatar(std::move(avatar));

    /* ------------------------------- BLE events ------------------------------- */
    GetHAL().onBleAvatarData.connect([&](const char* data) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_ble_avatar_data.update_flag) {
            return;
        }
        _ble_avatar_data.update_flag = true;
        _ble_avatar_data.data_ptr    = (char*)data;
    });

    GetHAL().onBleMotionData.connect([&](const char* data) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_ble_motion_data.update_flag) {
            return;
        }
        _ble_motion_data.update_flag = true;
        _ble_motion_data.data_ptr    = (char*)data;
    });

    GetHAL().onBleRgbData.connect([&](const char* data) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_ble_rgb_data.update_flag) {
            return;
        }
        _ble_rgb_data.update_flag = true;
        _ble_rgb_data.data_ptr    = (char*)data;
    });

    /* ----------------------------- Common widgets ----------------------------- */
    view::create_home_indicator([&]() { close(); }, 0xB77BFF, 0x422268);
    view::create_status_bar(0xB77BFF, 0x422268);

    /* ------------------------------ Head posture ----------------------------- */
    // Blink always, and drift gently while waiting for music. The idle motion
    // is deliberately calmer than the stock profile: small offsets from the
    // current pose, no wide look-arounds and no quick glances, held near the
    // neutral pitch and driven slowly. Sharp servo moves were what loaded the
    // rail hard enough to upset the servo bus, so the waiting behaviour stays
    // smooth and modest; the nodding is what should draw the eye.
    {
        auto& stackchan = GetStackChan();

        // Let the servos relax when the head is not moving. Only
        // Hal::startXiaozhi() ever turned this on, so every app outside the
        // voice path held torque continuously -- the pitch servo fighting
        // gravity for as long as the app was open. That standing current, not
        // just the movement spikes, is what loaded the rail hard enough to
        // upset the servo bus. The AI Agent mode has always done this.
        stackchan.motion().setAutoTorqueReleaseEnabled(true);

        stackchan.clearModifiers();

        // Breath and idle expression are avatar-only -- they move the face on
        // screen, never the servos -- so they cost nothing on the rail and are
        // most of what makes the robot read as continuously alive rather than
        // still-then-twitch. clearModifiers() above drops the default stack,
        // so an app that wants them has to say so.
        stackchan.addModifier(std::make_unique<BreathModifier>());
        stackchan.addModifier(std::make_unique<BlinkModifier>());
        stackchan.addModifier(std::make_unique<IdleExpressionModifier>());

        IdleMotionLimits_t calm;
        calm.yawMax         = 250;  // +-25 deg, well inside the safety window
        calm.pitchMin       = 260;  // stay around the neutral pitch (330)
        calm.pitchMax       = 400;
        calm.speedMin       = 100;  // slow == smooth
        calm.speedMax       = 200;
        calm.smallMovesOnly = true;

        auto idle_motion = std::make_unique<IdleMotionModifier>(4000, 8000, calm);
        _idle_motion     = idle_motion.get();
        stackchan.addModifier(std::move(idle_motion));

        park_at_neutral();
    }

    /* ------------------------------ Music mode ------------------------------- */
    // Listen for a beat and groove to it. A connected BLE choreographer still
    // wins whenever it is sending motion -- see update_music_dance().
    _beat_step            = 0;
    _last_beat_tick       = 0;
    _last_engagement_tick = 0;
    _led_hue              = -1;
    _led_flash_tick       = 0;
    _beat_detector.start();
}

#if DIAG_LED_TAP
void AppDance::led_tap_test()
{
    // The PY32 controller can address 32 positions; the robot is wired for 12.
    // Walking all 32 covers the possibility that the second row answers to an
    // address outside the usual range, which every earlier test would have
    // missed. The count is widened once, on the first pass.
    constexpr int kSlots = 32;

    static int index          = -1;
    static bool was_touched   = false;
    static bool widened       = false;
    static uint32_t last_tick = 0;

    if (!widened) {
        widened = true;
        GetHAL().setRgbLedCount(kSlots);
        mclog::tagWarn("LED-TAP", "LED count widened to {} for the scan; tap to advance", kSlots);
    }

    const auto tp      = hal_bridge::get_touch_point();
    const bool touched = tp.num > 0;
    const uint32_t now = GetHAL().millis();

    // Rising edge only, with a debounce so one tap advances one step.
    if (touched && !was_touched && now - last_tick > 250) {
        last_tick = now;
        index     = (index + 1) % kSlots;

        for (int i = 0; i < kSlots; i++) {
            GetHAL().setRgbColor(i, 0, 0, 0);
        }
        GetHAL().setRgbColor(index, 255, 255, 255);
        GetHAL().refreshRgb();

        const char* zone = index < 6    ? "row A / known good"
                           : index < 12 ? "row B / SUSPECT"
                                        : "beyond the wired 12 -- probing";
        mclog::tagWarn("LED-TAP", "tap -> position {:2} of {}  (array index {:2}, {})", index + 1, kSlots, index,
                       zone);
    }
    was_touched = touched;
}
#endif

void AppDance::onRunning()
{
#if DIAG_LED_TAP
    {
        LvglLockGuard lvgl_lock;
        led_tap_test();
        view::update_home_indicator();
        view::update_status_bar();
    }
    return;
#endif

    std::lock_guard<std::mutex> lock(_mutex);

    LvglLockGuard lvgl_lock;

    if (_ble_avatar_data.update_flag) {
        GetStackChan().updateAvatarFromJson(_ble_avatar_data.data_ptr);
        _ble_avatar_data.update_flag = false;
        _ble_avatar_data.data_ptr    = nullptr;
        note_engagement();
    }

    if (_ble_motion_data.update_flag) {
        check_auto_angle_sync_mode();
        GetStackChan().updateMotionFromJson(_ble_motion_data.data_ptr);
        _ble_motion_data.update_flag = false;
        _ble_motion_data.data_ptr    = nullptr;
        note_engagement();
    }

    if (_ble_rgb_data.update_flag) {
        GetStackChan().updateNeonLightFromJson(_ble_rgb_data.data_ptr);
        _ble_rgb_data.update_flag = false;
        _ble_rgb_data.data_ptr    = nullptr;
        note_engagement();
    }

    update_music_dance();

    GetStackChan().update();

    view::update_home_indicator();
    view::update_status_bar();
}

void AppDance::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _beat_detector.stop();

    GetHAL().showRgbColor(0, 0, 0);
    _leds_lit = false;

    {
        LvglLockGuard lock;

        GetStackChan().clearModifiers();
        _idle_motion = nullptr;

        GetStackChan().resetAvatar();

        GetHAL().onBleAvatarData.clear();
        GetHAL().onBleMotionData.clear();

        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    GetHAL().requestWarmReboot(5);
}

namespace {

// Fully saturated hue (degrees) to RGB. Brightness is applied by the caller,
// so this is the pure colour at full value.
void hue_to_rgb(int hue_deg, uint8_t& r, uint8_t& g, uint8_t& b)
{
    const int h      = ((hue_deg % 360) + 360) % 360;
    const int sector = h / 60;
    const int offset = h % 60;
    const uint8_t up   = static_cast<uint8_t>(offset * 255 / 60);
    const uint8_t down = static_cast<uint8_t>(255 - up);

    switch (sector) {
        case 0:  r = 255;  g = up;   b = 0;    break;
        case 1:  r = down; g = 255;  b = 0;    break;
        case 2:  r = 0;    g = 255;  b = up;   break;
        case 3:  r = 0;    g = down; b = 255;  break;
        case 4:  r = up;   g = 0;    b = 255;  break;
        default: r = 255;  g = 0;    b = down; break;
    }
}

}  // namespace

void AppDance::update_head_leds(bool music_playing)
{
    using namespace dance;

    // Dark whenever the room is quiet -- written once on the transition so a
    // silent room costs no I2C traffic at all.
    if (!music_playing) {
        if (_leds_lit) {
            _leds_lit = false;
            GetHAL().showRgbColor(0, 0, 0);
        }
        return;
    }

    const uint32_t now = GetHAL().millis();
    if (_leds_lit && now - _led_update_tick < 50) {
        return;
    }
    _led_update_tick = now;
    _leds_lit        = true;

    // Each beat flashes to full and fades back to a dim glow over the time the
    // chin takes to come up, so the light tracks the nod.
    const float fade_ms  = static_cast<float>(kDanceLeds.fadeMs);
    const uint32_t since = now - _led_flash_tick;
    float level          = kDanceLeds.glowLevel;
    if (_led_flash_tick != 0 && since < kDanceLeds.fadeMs) {
        const float fade = 1.0f - (static_cast<float>(since) / fade_ms);
        level            = kDanceLeds.glowLevel + (1.0f - kDanceLeds.glowLevel) * fade;
    }

    uint8_t r = kDanceLeds.fixedR;
    uint8_t g = kDanceLeds.fixedG;
    uint8_t b = kDanceLeds.fixedB;
    if (kDanceLeds.randomHue && _led_hue >= 0) {
        hue_to_rgb(_led_hue, r, g, b);
    }

    GetHAL().showRgbColor(static_cast<uint8_t>(r * level), static_cast<uint8_t>(g * level),
                          static_cast<uint8_t>(b * level));
}

void AppDance::park_at_neutral()
{
    using namespace dance;

    if (_at_neutral) {
        return;
    }
    _at_neutral = true;

    // Centred yaw with the chin up at pitchCenter -- the same pose each beat
    // returns to, so a dip reads as a nod away from a known rest rather than
    // from wherever the head happened to stop.
    auto& motion = GetStackChan().motion();

    // Sync from the servo's actual angle first: with torque release enabled the
    // head can droop or be moved by hand while idle, and animating from a stale
    // commanded pose would start the move with a jump.
    motion.setAutoAngleSyncEnabled(true);
    _nod_yaw  = std::clamp(kDanceMoves.yawNeutralOffset, kDanceMoves.yawSafeMin, kDanceMoves.yawSafeMax);
    _nod_down = false;
    motion.moveWithSpeed(_nod_yaw,
                         std::clamp(kDanceMoves.pitchCenter, kDanceMoves.pitchSafeMin, kDanceMoves.pitchSafeMax),
                         kDanceMoves.nodUpSpeed);
}

void AppDance::note_engagement()
{
    // Something is actually going on -- a BLE client driving the robot, or
    // music in the room -- so push back the board's idle sleep/shutdown
    // timer. Only the Xiaozhi voice path resets that timer by itself, and
    // DANCE deliberately never starts Xiaozhi, so without this the screen
    // blanks after 5 minutes and the board powers itself off after 10 on
    // battery no matter how hard it is dancing. Idle look-around alone does
    // not count, by design: an unattended robot in a quiet room should still
    // be allowed to shut down.
    const uint32_t now = GetHAL().millis();
    if (_last_engagement_tick != 0 && now - _last_engagement_tick < 1000) {
        return;
    }
    _last_engagement_tick = now;
    GetHAL().notifyUserInteraction();
}

void AppDance::check_auto_angle_sync_mode()
{
    auto& motion = GetStackChan().motion();

    // If far from last command, enable auto angle sync
    if (GetHAL().millis() - _last_motion_cmd_tick > 2000) {
        motion.setAutoAngleSyncEnabled(true);
    } else {
        motion.setAutoAngleSyncEnabled(false);
    }

    _last_motion_cmd_tick = GetHAL().millis();
}

void AppDance::update_music_dance()
{
    using namespace dance;

    // A BLE client driving the robot owns it outright. Stand down while
    // choreography is streaming in and pick up again once it stops, so the
    // puppeteering the DANCE app exists for is never fought over.
    // _last_motion_cmd_tick is only touched when BLE motion data arrives.
    constexpr uint32_t kBleHandoverMs = 3000;
    if (_last_motion_cmd_tick != 0 && GetHAL().millis() - _last_motion_cmd_tick < kBleHandoverMs) {
        if (_idle_motion) {
            _idle_motion->pause();
        }
        _nod_down = false;
        return;
    }

    // Two tiers, chosen by how much sustained sound is in the room:
    //
    //   music playing -> the head nods hard on the beat
    //   anything else -> Companion-style inquisitive looking around
    //
    // Voice and room noise land in the second tier by design: they lift the
    // average enough to be interesting but nowhere near a track with a bass
    // line in it, so talking near the robot makes it glance about rather than
    // headbang.
    if (!_beat_detector.isMusicPlaying()) {
        // Finish any dip still in progress, then hold the head still.
        if (_nod_down) {
            lift_chin();
            return;
        }
        // No music: drift gently. The next time music starts the head snaps
        // back to neutral first, so a nod always begins from the same pose.
        if (_idle_motion) {
            _idle_motion->resume();
        }
        _at_neutral = false;
        update_head_leds(false);
        return;
    }

    // Music is playing: someone is around and the robot is performing.
    note_engagement();

    if (_idle_motion) {
        _idle_motion->pause();
    }

    // Square up before dancing, so every nod reads against the same centred,
    // chin-up pose rather than from wherever the idle drift left the head.
    park_at_neutral();

    update_head_leds(true);

    float strength = 0.0f;
    if (_beat_detector.fetchBeat(strength)) {
        move_to_beat(strength);
        return;
    }

    // Second half of the nod: bring the chin back up once it has been down
    // long enough. Doing this from the loop rather than blocking in
    // move_to_beat() keeps the app responsive between beats.
    if (_nod_down && GetHAL().millis() - _last_beat_tick >= kDanceMoves.nodHoldMs) {
        lift_chin();
    }
}

void AppDance::move_to_beat(float strength)
{
    using namespace dance;

    auto& motion = GetStackChan().motion();

    // Chain each move from the last commanded pose rather than a fresh servo
    // reading: it keeps the nod smooth, and avoids a bus read per beat.
    motion.setAutoAngleSyncEnabled(false);

    // Harder beats dip deeper, softer ones barely move.
    const float scale =
        (1.0f - kDanceMoves.beatStrengthInfluence) + kDanceMoves.beatStrengthInfluence * strength;

    const int dip = static_cast<int>(kDanceMoves.nodDip * scale);

    // Kick the LEDs on the beat; update_head_leds() fades them back down.
    _led_flash_tick = GetHAL().millis();
    if (kDanceLeds.randomHue) {
        // Step to a hue that is clearly different from the last one, so
        // successive beats never read as the same colour.
        const int span = 360 - 2 * kDanceLeds.minHueStepDeg;
        const int step = kDanceLeds.minHueStepDeg + (span > 0 ? Random::getInstance().getInt(0, span) : 0);
        _led_hue       = (_led_hue < 0 ? Random::getInstance().getInt(0, 359) : (_led_hue + step)) % 360;
    }

    // Optional slow sway, one step every four beats. Off unless yawSwing is set.
    int yaw = kDanceMoves.yawCenter;
    if (kDanceMoves.yawSwing != 0) {
        const int swing = static_cast<int>(kDanceMoves.yawSwing * scale);
        switch (_beat_step) {
            case 0:
                yaw = kDanceMoves.yawCenter - swing;
                break;
            case 2:
                yaw = kDanceMoves.yawCenter + swing;
                break;
            default:
                yaw = kDanceMoves.yawCenter;
                break;
        }
    }
    _beat_step = (_beat_step + 1) % 4;

    // Chin down. The recovery is issued from the app loop once nodHoldMs has
    // passed -- see update_music_dance().
    const int pitch = kDanceMoves.pitchCenter + kDanceMoves.chinDipSign * dip;

    _nod_yaw = std::clamp(yaw, kDanceMoves.yawSafeMin, kDanceMoves.yawSafeMax);
    motion.moveWithSpeed(_nod_yaw, std::clamp(pitch, kDanceMoves.pitchSafeMin, kDanceMoves.pitchSafeMax),
                         kDanceMoves.nodDownSpeed);

    _last_beat_tick = GetHAL().millis();
    _nod_down       = true;
}

void AppDance::lift_chin()
{
    using namespace dance;

    auto& motion = GetStackChan().motion();
    motion.setAutoAngleSyncEnabled(false);

    motion.moveWithSpeed(_nod_yaw,
                         std::clamp(kDanceMoves.pitchCenter, kDanceMoves.pitchSafeMin, kDanceMoves.pitchSafeMax),
                         kDanceMoves.nodUpSpeed);
    _nod_down = false;
}

