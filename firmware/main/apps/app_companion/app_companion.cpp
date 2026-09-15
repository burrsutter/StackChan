/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_companion.h"
#include "chirp.h"
#include "lean_pet.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <smooth_lvgl.hpp>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <memory>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace stackchan;

// How long the wave reaction (yellow happy face looking at you) lasts
static constexpr uint32_t WAVE_REACTION_MS = 2600;
// Minimum time between pet chirps, so continuous petting doesn't machine-gun
static constexpr uint32_t CHIRP_COOLDOWN_MS = 3500;
// Wave reaction face background
static constexpr uint32_t WAVE_FACE_COLOR = 0xF7C948;

AppCompanion::AppCompanion()
{
    // Configure App name
    setAppInfo().name = "COMPANION";
    // Configure App icon
    static auto icon  = assets::get_image("icon_sentinel.bin");
    setAppInfo().icon = (void*)&icon;
    // Configure App theme color
    static uint32_t theme_color = 0xFFC24D;
    setAppInfo().userData       = (void*)&theme_color;
}

void AppCompanion::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppCompanion::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    {
        LvglLockGuard lock;

        auto& stackchan = GetStackChan();

        auto avatar = std::make_unique<avatar::DefaultAvatar>();
        avatar->init(lv_screen_active());
        _face_panel = avatar->getPanel();
        stackchan.attachAvatar(std::move(avatar));

        // Same liveliness as the AI agent mode, without starting Xiaozhi:
        // no wake word, no mic streaming, no cloud calls
        stackchan.clearModifiers();
        stackchan.addModifier(std::make_unique<BreathModifier>());
        stackchan.addModifier(std::make_unique<BlinkModifier>());
        stackchan.addModifier(std::make_unique<IdleExpressionModifier>());
        auto idle_motion = std::make_unique<IdleMotionModifier>();
        _idle_motion     = idle_motion.get();
        stackchan.addModifier(std::move(idle_motion));
        stackchan.addModifier(std::make_unique<companion::LeanPetModifier>());

        GetHAL().setLaserEnabled(false);

        view::create_home_indicator([&]() { close(); }, 0xFFC24D, 0x4A3413);
        view::create_status_bar(0xFFC24D, 0x4A3413);
    }

    // Happy chirp when petting starts (gestures fire on the head touch task)
    _pet_signal_id = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture gesture) {
        if (gesture != HeadPetGesture::SwipeForward && gesture != HeadPetGesture::SwipeBackward) {
            return;
        }
        uint32_t now  = GetHAL().millis();
        uint32_t last = _last_chirp_ms.load();
        if (now - last > CHIRP_COOLDOWN_MS && _last_chirp_ms.compare_exchange_strong(last, now)) {
            companion::play_happy_chirp();
        }
    });

    _wave_reaction_until = 0;
    _wave.start();
}

void AppCompanion::onRunning()
{
    LvglLockGuard lock;

    float wave_x;
    if (_wave.fetchWave(wave_x)) {
        react_to_wave(wave_x);
    }
    if (_wave_reaction_until != 0 && GetHAL().millis() > _wave_reaction_until) {
        end_wave_reaction();
    }

    GetStackChan().update();

    view::update_home_indicator();
    view::update_status_bar();
}

void AppCompanion::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _wave.stop();
    GetHAL().onHeadPetGesture.disconnect(_pet_signal_id);

    LvglLockGuard lock;

    auto& stackchan = GetStackChan();

    // Recenter the head before handing back to the launcher
    stackchan.motion().lookAtNormalized(0.0f, 0.0f, 200);

    stackchan.clearModifiers();
    _idle_motion = nullptr;
    _face_panel  = nullptr;
    stackchan.resetAvatar();

    view::destroy_home_indicator();
    view::destroy_status_bar();
}

void AppCompanion::react_to_wave(float x)
{
    mclog::tagInfo(getAppInfo().name, "reacting to wave at x {:.2f}", x);

    auto& stackchan = GetStackChan();

    // Hold still and give the greeter full attention
    if (_idle_motion) {
        _idle_motion->pause();
    }
    _wave.setPaused(true);

    // Turn toward the wave. The camera rides on the head, so the image offset
    // is relative to the current heading. The GC0308 sees roughly +-28
    // degrees, so image x = +-1 maps to +-280 tenth-degree servo units.
    constexpr float CAMERA_HALF_FOV_UNITS = 280.0f;
    constexpr float WAVE_TURN_SIGN        = 1.0f;  // flip if the head turns away from the hand
    auto current                          = stackchan.motion().getCurrentAngles();
    const int target_yaw =
        uitk::clamp(static_cast<int>(current.x + WAVE_TURN_SIGN * x * CAMERA_HALF_FOV_UNITS), -800, 800);
    mclog::tagInfo(getAppInfo().name, "turning: yaw {} -> {}", current.x, target_yaw);
    stackchan.motion().moveWithSpeed(target_yaw, current.y, 300);

    // Yellow face with a bigger smile
    stackchan.avatar().setEmotion(avatar::Emotion::Happy);
    if (_face_panel) {
        _face_panel->setBgColor(lv_color_hex(WAVE_FACE_COLOR));
    }

    _wave_reaction_until = GetHAL().millis() + WAVE_REACTION_MS;
}

void AppCompanion::end_wave_reaction()
{
    auto& stackchan = GetStackChan();

    stackchan.avatar().setEmotion(avatar::Emotion::Neutral);
    if (_face_panel) {
        _face_panel->setBgColor(lv_color_black());
    }

    if (_idle_motion) {
        _idle_motion->resume();
    }
    _wave.setPaused(false);
    _wave_reaction_until = 0;
}
