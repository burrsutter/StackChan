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

    /* --------------------------- Quiet-room idling --------------------------- */
    // With no music playing the robot should still feel alive rather than
    // frozen: blink, and look around inquisitively the way Companion mode
    // does. update_music_dance() pauses this the moment a beat takes over.
    {
        auto& stackchan = GetStackChan();
        stackchan.clearModifiers();
        stackchan.addModifier(std::make_unique<BlinkModifier>());
        auto idle_motion = std::make_unique<IdleMotionModifier>(2500, 5000);
        _idle_motion     = idle_motion.get();
        stackchan.addModifier(std::move(idle_motion));
    }

    /* ------------------------------ Music mode ------------------------------- */
    // Listen for a beat and groove to it. A connected BLE choreographer still
    // wins whenever it is sending motion -- see update_music_dance().
    _beat_step       = 0;
    _last_beat_tick  = 0;
    _beat_detector.start();
}

void AppDance::onRunning()
{
    std::lock_guard<std::mutex> lock(_mutex);

    LvglLockGuard lvgl_lock;

    if (_ble_avatar_data.update_flag) {
        GetStackChan().updateAvatarFromJson(_ble_avatar_data.data_ptr);
        _ble_avatar_data.update_flag = false;
        _ble_avatar_data.data_ptr    = nullptr;
    }

    if (_ble_motion_data.update_flag) {
        check_auto_angle_sync_mode();
        GetStackChan().updateMotionFromJson(_ble_motion_data.data_ptr);
        _ble_motion_data.update_flag = false;
        _ble_motion_data.data_ptr    = nullptr;
    }

    if (_ble_rgb_data.update_flag) {
        GetStackChan().updateNeonLightFromJson(_ble_rgb_data.data_ptr);
        _ble_rgb_data.update_flag = false;
        _ble_rgb_data.data_ptr    = nullptr;
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
        // Finish any dip still in progress before handing the head back.
        if (_nod_down) {
            lift_chin();
            return;
        }
        if (_idle_motion) {
            _idle_motion->resume();
        }
        return;
    }

    if (_idle_motion) {
        _idle_motion->pause();
    }

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

