/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_party.h"
#include "dance_params.h"
#include "../app_avatar/view/ws_call.h"
#include <hal/hal.h>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <mooncake.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <smooth_lvgl.hpp>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace stackchan;

namespace {

// How long a server command keeps the head (and the LEDs) before the music
// and idle behaviour take back over.
constexpr uint32_t kServerMotionHoldMs = 3000;
constexpr uint32_t kServerLedHoldMs    = 5000;

// Launcher position this app restores to after the warm reboot on close.
// Apps are indexed in install order after the launcher; PARTY is installed
// last in main.cpp.
constexpr int kWarmRebootIndex = 8;

bool contains_word(const std::string& text, const std::unordered_set<std::string>& words)
{
    std::istringstream iss(text);
    std::string token;
    while (iss >> token) {
        std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return std::tolower(c); });
        if (words.find(token) != words.end()) {
            return true;
        }
    }
    return false;
}

// The RGB keys a ControlAvatar frame may carry (leftRgbColor, rightRgbColor,
// ...). Any of them means the server is driving the LEDs.
bool carries_led_fields(std::string_view data)
{
    return data.find("RgbColor") != std::string_view::npos;
}

// Fully saturated hue (degrees) to RGB. Brightness is applied by the caller.
void hue_to_rgb(int hue_deg, uint8_t& r, uint8_t& g, uint8_t& b)
{
    const int h        = ((hue_deg % 360) + 360) % 360;
    const int sector   = h / 60;
    const int offset   = h % 60;
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

AppParty::AppParty()
{
    setAppInfo().name = "PARTY";
    static auto icon  = assets::get_image("icon_dance.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = 0x2EC4B6;
    setAppInfo().userData       = (void*)&theme_color;
}

void AppParty::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppParty::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    std::unique_ptr<view::LoadingPage> loading_page;
    {
        LvglLockGuard lock;
        loading_page = std::make_unique<view::LoadingPage>(0x2EC4B6, 0x0E3B37);
    }

    GetHAL().startWebSocketAvatarService([&](std::string_view msg) {
        LvglLockGuard lock;
        loading_page->setMessage(msg);
    });

    LvglLockGuard lock;

    loading_page.reset();

    // Teal face marks Party mode at a glance.
    auto avatar            = std::make_unique<avatar::DefaultAvatar>();
    avatar->secondaryColor = lv_color_hex(0x2EC4B6);
    avatar->init(lv_screen_active());
    avatar->getPanel()->onClick().connect([&]() { _screen_clicked_flag = true; });
    GetStackChan().attachAvatar(std::move(avatar));

    /* ------------------------------ Head posture ----------------------------- */
    {
        auto& stackchan = GetStackChan();

        // Relax the servos whenever the head is still. Holding torque the whole
        // time the app is open keeps the pitch servo fighting gravity.
        stackchan.motion().setAutoTorqueReleaseEnabled(true);

        stackchan.clearModifiers();

        // Face-only liveliness: these never touch the servos.
        stackchan.addModifier(std::make_unique<BreathModifier>());
        stackchan.addModifier(std::make_unique<BlinkModifier>());
        stackchan.addModifier(std::make_unique<IdleExpressionModifier>());

        // Deliberately calm idle drift: small offsets near the neutral pitch,
        // slow, no wide look-arounds and no quick glances.
        IdleMotionLimits_t calm;
        calm.yawMax         = 250;
        calm.pitchMin       = 260;
        calm.pitchMax       = 400;
        calm.speedMin       = 100;
        calm.speedMax       = 200;
        calm.smallMovesOnly = true;

        auto idle_motion = std::make_unique<IdleMotionModifier>(4000, 8000, calm);
        _idle_motion     = idle_motion.get();
        stackchan.addModifier(std::move(idle_motion));

        park_at_neutral();
    }

    /* ---------------------------- Websocket events ---------------------------- */
    GetHAL().onWsAvatarData.connect([&](std::string_view data) {
        LvglLockGuard lvgl_lock;
        GetStackChan().updateAvatarFromJson(data.data());

        // A ControlAvatar frame can carry LED fields as well as face fields;
        // the two parsers ignore each other's keys.
        GetStackChan().updateNeonLightFromJson(data.data());
        if (carries_led_fields(data)) {
            _last_remote_led_tick = GetHAL().millis();
            _leds_lit             = false;  // hand the LEDs over without a final beat write
        }
        note_engagement();
    });

    GetHAL().onWsMotionData.connect([&](std::string_view data) {
        LvglLockGuard lvgl_lock;
        if (_idle_motion) {
            _idle_motion->pause();
        }
        _nod_down = false;
        check_auto_angle_sync_mode();
        GetStackChan().updateMotionFromJson(data.data());
        note_engagement();
    });

    GetHAL().onWsCallRequest.connect([&](std::string caller) {
        if (_ws_call_view_id >= 0) {
            mclog::tagWarn(getAppInfo().name, "ws call view already exists");
            return;
        }

        LvglLockGuard lvgl_lock;

        auto& avatar = GetStackChan().avatar();
        avatar.setSpeech("");
        avatar.leftEye().setVisible(false);
        avatar.rightEye().setVisible(false);
        avatar.mouth().setVisible(false);

        auto view      = std::make_unique<view::WsCallView>(lv_screen_active(), caller);
        view->onAccept = []() {
            auto& avatar = GetStackChan().avatar();
            avatar.setSpeech("");
            avatar.leftEye().setVisible(true);
            avatar.rightEye().setVisible(true);
            avatar.mouth().setVisible(true);

            GetHAL().onWsCallResponse.emit(true);
        };
        view->onDecline = []() {
            auto& avatar = GetStackChan().avatar();
            avatar.setSpeech("");
            avatar.leftEye().setVisible(true);
            avatar.rightEye().setVisible(true);
            avatar.mouth().setVisible(true);

            GetHAL().onWsCallResponse.emit(false);
        };
        view->onEnd     = []() { GetHAL().onWsCallEnd.emit(WsSignalSource::Local); };
        view->onDestory = [&]() { _ws_call_view_id = -1; };

        _ws_call_view_id = avatar.addDecorator(std::move(view));
    });

    GetHAL().onWsCallEnd.connect([&](WsSignalSource source) {
        if (source != WsSignalSource::Remote) {
            return;
        }

        LvglLockGuard lvgl_lock;

        if (_ws_call_view_id < 0) {
            mclog::tagWarn(getAppInfo().name, "ws call view does not exist");
            return;
        }

        auto& avatar = GetStackChan().avatar();
        avatar.setSpeech("");
        avatar.leftEye().setVisible(true);
        avatar.rightEye().setVisible(true);
        avatar.mouth().setVisible(true);

        avatar.removeDecorator(_ws_call_view_id);
        _ws_call_view_id = -1;
    });

    GetHAL().onWsTextMessage.connect([&](const WsTextMessage_t& message) {
        LvglLockGuard lvgl_lock;

        auto& stackchan = GetStackChan();

        stackchan.addModifier(
            std::make_unique<TimedSpeechModifier>(fmt::format("{} says: {}", message.name, message.content), 6000));
        stackchan.addModifier(std::make_unique<SpeakingModifier>(2000));

        if (contains_word(message.content, {"hello", "hi", "love"})) {
            stackchan.addModifier(std::make_unique<TimedEmotionModifier>(avatar::Emotion::Happy, 2000));
        }
        note_engagement();
    });

    GetHAL().onWsDanceData.connect([&](std::string_view data) {
        LvglLockGuard lvgl_lock;
        auto sequence = stackchan::animation::parse_sequence_from_json(data.data());
        if (sequence.empty()) {
            return;
        }
        if (server_dance_playing()) {
            GetStackChan().removeModifier(_dance_modifier_id);
        }
        if (_idle_motion) {
            _idle_motion->pause();
        }
        _nod_down           = false;
        _dance_modifier_id  = GetStackChan().addModifier(std::make_unique<DanceModifier>(sequence));
        _dance_modifier_ptr = GetStackChan().getModifier(_dance_modifier_id);
        note_engagement();
    });

    GetHAL().onWsLog.connect([&](CommonLogLevel level, std::string_view msg) {
        auto type         = static_cast<view::ToastType>(level);
        uint32_t duration = type == view::ToastType::Error ? 12000 : 1600;
        view::pop_a_toast(msg, type, duration);
    });

    /* ------------------------------ Video window ------------------------------ */
    _video_window = std::make_unique<view::VideoWindow>(lv_screen_active());

    /* ----------------------------- Common widgets ----------------------------- */
    view::create_home_indicator([&]() { close(); }, 0x2EC4B6, 0x0E3B37);
    view::create_status_bar(0x2EC4B6, 0x0E3B37);

    /* ------------------------------ Music mode ------------------------------- */
    _beat_step            = 0;
    _last_beat_tick       = 0;
    _last_engagement_tick = 0;
    _led_hue              = -1;
    _led_flash_tick       = 0;
    _beat_detector.start();
}

void AppParty::onRunning()
{
    LvglLockGuard lvgl_lock;

    // Tapping the face stops a server dance sequence, as in AVATAR.
    if (_screen_clicked_flag) {
        _screen_clicked_flag = false;
        if (server_dance_playing()) {
            GetStackChan().removeModifier(_dance_modifier_id);
            mclog::tagInfo(getAppInfo().name, "dance sequence stopped");
        }
        _dance_modifier_id  = -1;
        _dance_modifier_ptr = nullptr;
    }

    update_music_dance();

    GetStackChan().update();

    view::update_home_indicator();
    view::update_status_bar();
}

void AppParty::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _beat_detector.stop();

    GetHAL().showRgbColor(0, 0, 0);
    _leds_lit = false;

    {
        LvglLockGuard lock;

        GetStackChan().clearModifiers();
        _idle_motion        = nullptr;
        _dance_modifier_id  = -1;
        _dance_modifier_ptr = nullptr;

        GetStackChan().resetAvatar();
        _video_window.reset();

        GetHAL().onWsAvatarData.clear();
        GetHAL().onWsMotionData.clear();
        GetHAL().onWsCallRequest.clear();
        GetHAL().onWsCallEnd.clear();
        GetHAL().onWsTextMessage.clear();
        GetHAL().onWsDanceData.clear();
        GetHAL().onWsLog.clear();

        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    GetHAL().requestWarmReboot(kWarmRebootIndex);
}

bool AppParty::server_dance_playing()
{
    if (_dance_modifier_id < 0) {
        return false;
    }
    auto* modifier = GetStackChan().getModifier(_dance_modifier_id);
    if (modifier == nullptr || modifier != _dance_modifier_ptr || modifier->isDestroyRequested()) {
        _dance_modifier_id  = -1;
        _dance_modifier_ptr = nullptr;
        return false;
    }
    return true;
}

bool AppParty::server_owns_head()
{
    if (server_dance_playing()) {
        return true;
    }
    return _last_motion_cmd_tick != 0 && GetHAL().millis() - _last_motion_cmd_tick < kServerMotionHoldMs;
}

void AppParty::update_head_leds(bool music_playing)
{
    using namespace dance;

    // The server set the LEDs recently: leave them alone.
    if (_last_remote_led_tick != 0 && GetHAL().millis() - _last_remote_led_tick < kServerLedHoldMs) {
        return;
    }

    // Dark whenever the room is quiet -- written once on the transition so a
    // silent room costs no I2C traffic.
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

void AppParty::park_at_neutral()
{
    using namespace dance;

    if (_at_neutral) {
        return;
    }
    _at_neutral = true;

    // Centred yaw, chin up at pitchCenter: the pose each beat returns to.
    auto& motion = GetStackChan().motion();

    // Sync from the servo's real angle first: with torque release the head can
    // droop or be moved by hand, and animating from a stale pose would jump.
    motion.setAutoAngleSyncEnabled(true);
    _nod_yaw  = std::clamp(kDanceMoves.yawNeutralOffset, kDanceMoves.yawSafeMin, kDanceMoves.yawSafeMax);
    _nod_down = false;
    motion.moveWithSpeed(_nod_yaw,
                         std::clamp(kDanceMoves.pitchCenter, kDanceMoves.pitchSafeMin, kDanceMoves.pitchSafeMax),
                         kDanceMoves.nodUpSpeed);
}

void AppParty::note_engagement()
{
    // Someone is driving the robot or music is playing, so push back the idle
    // sleep/shutdown timer -- only the Xiaozhi voice path resets it on its
    // own. Idle drift alone does not count: an unattended robot in a quiet
    // room should still be allowed to sleep.
    const uint32_t now = GetHAL().millis();
    if (_last_engagement_tick != 0 && now - _last_engagement_tick < 1000) {
        return;
    }
    _last_engagement_tick = now;
    GetHAL().notifyUserInteraction();
}

void AppParty::check_auto_angle_sync_mode()
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

void AppParty::update_music_dance()
{
    using namespace dance;

    // The server owns the head outright while it is driving or a dance
    // sequence is playing. Music and idle drift pick up again afterwards,
    // from the head's real position.
    if (server_owns_head()) {
        if (_idle_motion) {
            _idle_motion->pause();
        }
        _nod_down   = false;
        _at_neutral = false;
        update_head_leds(_beat_detector.isMusicPlaying());
        return;
    }

    if (!_beat_detector.isMusicPlaying()) {
        // Finish any dip still in progress, then drift.
        if (_nod_down) {
            lift_chin();
            return;
        }
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

    // Square up before dancing, so every nod starts from the same pose.
    park_at_neutral();

    update_head_leds(true);

    float strength = 0.0f;
    if (_beat_detector.fetchBeat(strength)) {
        move_to_beat(strength);
        return;
    }

    // Second half of the nod, issued from the loop rather than blocking.
    if (_nod_down && GetHAL().millis() - _last_beat_tick >= kDanceMoves.nodHoldMs) {
        lift_chin();
    }
}

void AppParty::move_to_beat(float strength)
{
    using namespace dance;

    auto& motion = GetStackChan().motion();

    // Chain each move from the last commanded pose rather than a fresh servo
    // reading: smoother, and no bus read per beat.
    motion.setAutoAngleSyncEnabled(false);

    // Harder beats dip deeper, softer ones barely move.
    const float scale = (1.0f - kDanceMoves.beatStrengthInfluence) + kDanceMoves.beatStrengthInfluence * strength;
    const int dip     = static_cast<int>(kDanceMoves.nodDip * scale);

    // Kick the LEDs on the beat; update_head_leds() fades them back down.
    _led_flash_tick = GetHAL().millis();
    if (kDanceLeds.randomHue) {
        // Step to a hue clearly different from the last one.
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

    // Chin down; the recovery is issued from the loop once nodHoldMs passes.
    const int pitch = kDanceMoves.pitchCenter + kDanceMoves.chinDipSign * dip;

    _nod_yaw = std::clamp(yaw, kDanceMoves.yawSafeMin, kDanceMoves.yawSafeMax);
    motion.moveWithSpeed(_nod_yaw, std::clamp(pitch, kDanceMoves.pitchSafeMin, kDanceMoves.pitchSafeMax),
                         kDanceMoves.nodDownSpeed);

    _last_beat_tick = GetHAL().millis();
    _nod_down       = true;
}

void AppParty::lift_chin()
{
    using namespace dance;

    auto& motion = GetStackChan().motion();
    motion.setAutoAngleSyncEnabled(false);

    motion.moveWithSpeed(_nod_yaw,
                         std::clamp(kDanceMoves.pitchCenter, kDanceMoves.pitchSafeMin, kDanceMoves.pitchSafeMax),
                         kDanceMoves.nodUpSpeed);
    _nod_down = false;
}
