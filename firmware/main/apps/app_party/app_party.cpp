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

    // LEDs dark until music plays. The PY32 expander keeps its LED state
    // across an ESP reboot, so whatever the last app or a crash left lit
    // would otherwise still be showing.
    GetHAL().showRgbColor(0, 0, 0);
    _leds_lit = false;

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
            // Shown for kServerLedHoldMs, then switched off again unless
            // music is playing: in PARTY the LEDs are dark by default.
            _last_remote_led_tick = GetHAL().millis();
            _leds_lit             = true;
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

    update_servo_health();
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

    // Dark whenever the room is quiet, including after a server colour's hold
    // runs out -- written once on the transition so a silent room costs no
    // I2C traffic.
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
    float flash          = kDanceLeds.glowLevel;
    if (_led_flash_tick != 0 && since < kDanceLeds.fadeMs) {
        const float fade = 1.0f - (static_cast<float>(since) / fade_ms);
        flash            = kDanceLeds.glowLevel + (1.0f - kDanceLeds.glowLevel) * fade;
    }

    uint8_t r = kDanceLeds.fixedR, g = kDanceLeds.fixedG, b = kDanceLeds.fixedB;
    uint8_t r2 = r, g2 = g, b2 = b;
    if (kDanceLeds.randomHue && _led_hue >= 0) {
        hue_to_rgb(_led_hue, r, g, b);
        hue_to_rgb(_led_hue + 180, r2, g2, b2);
    }

    // Right bar is LEDs 0-5, left bar 6-11. A normal beat flashes one bar,
    // alternating, while the other holds the glow; a strong beat flashes both
    // in complementary colours.
    const float right_level = (_flash_both_bars || !_flash_left_bar) ? flash : kDanceLeds.glowLevel;
    const float left_level  = (_flash_both_bars || _flash_left_bar) ? flash : kDanceLeds.glowLevel;
    const bool two_colours  = _flash_both_bars;

    for (uint8_t i = 0; i < 12; i++) {
        const bool left   = i >= 6;
        const float level = left ? left_level : right_level;
        const uint8_t cr  = (left && two_colours) ? r2 : r;
        const uint8_t cg  = (left && two_colours) ? g2 : g;
        const uint8_t cb  = (left && two_colours) ? b2 : b;
        GetHAL().setRgbColor(i, static_cast<uint8_t>(cr * level), static_cast<uint8_t>(cg * level),
                             static_cast<uint8_t>(cb * level));
    }
    GetHAL().refreshRgb();
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

void AppParty::set_party_face(bool on)
{
    if (on == _party_face) {
        return;
    }
    _party_face  = on;
    auto& avatar = GetStackChan().avatar();
    avatar.setEmotion(on ? avatar::Emotion::Happy : avatar::Emotion::Neutral);
    avatar.mouth().setWeight(dance::kPartyShow.beatMouthRest);
    _mouth_open = false;
}

void AppParty::update_music_dance()
{
    using namespace dance;

    const bool music   = _beat_detector.isMusicPlaying();
    const uint32_t now = GetHAL().millis();

    update_stamina(music);
    set_party_face(music);

    // Close the mouth with the chin, whether or not the head nodded.
    if (_mouth_open && now - _last_beat_tick >= kDanceMoves.nodHoldMs) {
        GetStackChan().avatar().mouth().setWeight(kPartyShow.beatMouthRest);
        _mouth_open = false;
    }

    // The server owns the head outright while it is driving or a dance
    // sequence is playing. Music and idle drift pick up again afterwards,
    // from the head's real position.
    if (server_owns_head()) {
        if (_idle_motion) {
            _idle_motion->pause();
        }
        _nod_down   = false;
        _at_neutral = false;
        update_head_leds(music);
        return;
    }

    if (!music) {
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

    // Square up before dancing, so every nod starts from the same pose -- but
    // not while resting for heat or strain, when the servos should stay still.
    if (nod_scale() > 0.0f) {
        park_at_neutral();
    }

    update_head_leds(true);

    float strength = 0.0f;
    if (_beat_detector.fetchBeat(strength)) {
        move_to_beat(strength);
        return;
    }

    // Second half of the nod, issued from the loop rather than blocking.
    if (_nod_down && now - _last_beat_tick >= kDanceMoves.nodHoldMs) {
        lift_chin();
    }
}

void AppParty::move_to_beat(float strength)
{
    using namespace dance;

    _last_beat_tick = GetHAL().millis();

    // LEDs: kick the flash, alternate the lead bar, and light both bars in two
    // colours on a strong beat. update_head_leds() fades them back down.
    _led_flash_tick  = _last_beat_tick;
    _flash_left_bar  = !_flash_left_bar;
    _flash_both_bars = strength >= kPartyShow.strongBeat;
    if (kDanceLeds.randomHue) {
        // Step to a hue clearly different from the last one.
        const int span = 360 - 2 * kDanceLeds.minHueStepDeg;
        const int step = kDanceLeds.minHueStepDeg + (span > 0 ? Random::getInstance().getInt(0, span) : 0);
        _led_hue       = (_led_hue < 0 ? Random::getInstance().getInt(0, 359) : (_led_hue + step)) % 360;
    }

    // Face: the mouth pops open on the beat. Free -- it is only pixels.
    GetStackChan().avatar().mouth().setWeight(kPartyShow.beatMouthOpen);
    _mouth_open = true;

    // The nod is the only part that wears anything, so it is the part that
    // backs off when he is tired, warm or straining.
    const float safety = nod_scale();
    if (safety <= 0.0f) {
        _nod_down = false;
        return;
    }

    auto& motion = GetStackChan().motion();

    // Chain each move from the last commanded pose rather than a fresh servo
    // reading: smoother, and no bus read per beat.
    motion.setAutoAngleSyncEnabled(false);

    // Harder beats dip deeper, softer ones barely move.
    const float groove = (1.0f - kDanceMoves.beatStrengthInfluence) + kDanceMoves.beatStrengthInfluence * strength;
    const float scale  = groove * safety;
    const int dip      = static_cast<int>(kDanceMoves.nodDip * scale);

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
    _nod_down = true;
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

/* --------------------------- Party without injury -------------------------- */

namespace {
const char* rest_name(int rest)
{
    switch (rest) {
        case 1:  return "stamina";
        case 2:  return "heat";
        case 3:  return "strain";
        default: return "none";
    }
}
}  // namespace

void AppParty::begin_rest(Rest reason)
{
    _rest            = reason;
    _rest_since_tick = GetHAL().millis();
    mclog::tagWarn("PartySafety", "resting ({}): yaw {}C pitch {}C -- face and LEDs carry on, nods {}",
                   rest_name(static_cast<int>(reason)), _servo_temp_c[0], _servo_temp_c[1],
                   reason == Rest::Stamina ? "shrink to a bob" : "stop");
}

float AppParty::nod_scale() const
{
    using namespace dance;

    float scale = 1.0f;
    switch (_rest) {
        case Rest::Heat:
        case Rest::Strain:
            return 0.0f;
        case Rest::Stamina:
            scale = kPartySafety.staminaRestScale;
            break;
        case Rest::None:
            break;
    }

    const int hottest = std::max(_servo_temp_c[0], _servo_temp_c[1]);
    if (hottest >= kPartySafety.warmC) {
        scale = std::min(scale, kPartySafety.warmScale);
    }
    return scale;
}

void AppParty::update_stamina(bool music_playing)
{
    using namespace dance;

    const uint32_t now = GetHAL().millis();

    if (music_playing) {
        _last_music_tick = now;
        if (_stint_start_tick == 0) {
            _stint_start_tick = now;
        }
    } else if (_stint_start_tick != 0 && now - _last_music_tick >= kPartySafety.stintResetGapMs) {
        // A real break between songs: the stint starts over.
        _stint_start_tick = 0;
    }

    if (_rest == Rest::None && _stint_start_tick != 0 && now - _stint_start_tick >= kPartySafety.stintMs) {
        begin_rest(Rest::Stamina);
    }

    if (_rest == Rest::Stamina && now - _rest_since_tick >= kPartySafety.staminaRestMs) {
        _rest             = Rest::None;
        _stint_start_tick = music_playing ? now : 0;
        mclog::tagInfo("PartySafety", "rested, back to full nods");
    }

    if (_rest == Rest::Strain && now - _rest_since_tick >= kPartySafety.strainRestMs) {
        _rest = Rest::None;
        mclog::tagInfo("PartySafety", "strain rest over, nodding again");
    }
}

void AppParty::update_servo_health()
{
    using namespace dance;

    const uint32_t now = GetHAL().millis();
    if (_last_telemetry_tick != 0 && now - _last_telemetry_tick < kPartySafety.telemetryIntervalMs) {
        return;
    }
    _last_telemetry_tick = now;

    // Strain only counts on a head that is meant to be still, so read whether
    // it is animating (bus-free) before touching the bus.
    const bool still = !GetStackChan().motion().isAnimating();

    ServoTelemetry_t t[2];
    bool ok[2];
    for (int i = 0; i < 2; i++) {
        ok[i] = GetHAL().readServoTelemetry(i + 1, t[i]);
        if (!ok[i]) {
            continue;
        }
        _servo_temp_c[i] = t[i].temperatureC;

        if (still && std::abs(t[i].load) >= kPartySafety.strainLoad) {
            _strain_count[i]++;
        } else {
            _strain_count[i] = 0;
        }
    }

    mclog::tagInfo("PartySafety",
                   "yaw {} | pitch {} | still={} rest={} nod_scale={:.2f}",
                   ok[0] ? fmt::format("{}C {}.{}V load={} cur={}", t[0].temperatureC, t[0].voltageDeciV / 10,
                                       t[0].voltageDeciV % 10, t[0].load, t[0].current)
                         : std::string("no reply"),
                   ok[1] ? fmt::format("{}C {}.{}V load={} cur={}", t[1].temperatureC, t[1].voltageDeciV / 10,
                                       t[1].voltageDeciV % 10, t[1].load, t[1].current)
                         : std::string("no reply"),
                   still, rest_name(static_cast<int>(_rest)), nod_scale());

    if (_rest != Rest::Heat && _rest != Rest::Strain &&
        std::max(_strain_count[0], _strain_count[1]) >= kPartySafety.strainSamples) {
        _strain_count[0] = _strain_count[1] = 0;
        begin_rest(Rest::Strain);
        return;
    }

    const int hottest = std::max(_servo_temp_c[0], _servo_temp_c[1]);
    if (_rest != Rest::Heat && hottest >= kPartySafety.hotC) {
        begin_rest(Rest::Heat);
        return;
    }

    if (_rest == Rest::Heat && now - _rest_since_tick >= kPartySafety.minHeatRestMs && _servo_temp_c[0] >= 0 &&
        _servo_temp_c[1] >= 0 && hottest < kPartySafety.cooledC) {
        _rest = Rest::None;
        mclog::tagInfo("PartySafety", "cooled to {}C, nodding again", hottest);
    }
}
