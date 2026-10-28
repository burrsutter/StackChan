/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "beat_detector.h"
#include "../app_avatar/view/video_window.hpp"
#include <stackchan/stackchan.h>
#include <mooncake.h>
#include <cstdint>
#include <memory>

/**
 * @brief AVATAR's remote control and DANCE's music reaction in one app.
 *
 * The robot connects to the self-hosted server (CONFIG_STACKCHAN_SERVER_URL)
 * for head, face, LED, text and camera control, and between commands it
 * listens to the room: nodding and flashing on the beat when music plays,
 * drifting calmly when it does not. The stock AVATAR and DANCE apps are left
 * exactly as M5Stack ships them.
 *
 * Who owns the head, highest first:
 *   1. the server -- a motion command, or a dance sequence still playing
 *   2. the music  -- a nod on every beat
 *   3. idle drift -- small slow moves around the neutral pose
 */
class AppParty : public mooncake::AppAbility {
public:
    AppParty();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    uint32_t _last_motion_cmd_tick = 0;
    uint32_t _last_remote_led_tick = 0;

    int _ws_call_view_id = -1;

    // The server's dance sequence, if one is playing. The modifier destroys
    // itself when its timeline ends, so the pointer is kept to tell a
    // finished sequence from a different modifier reusing the same id.
    int _dance_modifier_id                    = -1;
    stackchan::Modifier* _dance_modifier_ptr = nullptr;

    std::unique_ptr<view::VideoWindow> _video_window;
    bool _screen_clicked_flag = false;

    // Music-reactive choreography. Tunables live in dance_params.h.
    dance::BeatDetector _beat_detector;
    int _beat_step           = 0;      // position in the optional sway cycle
    uint32_t _last_beat_tick = 0;      // when the last beat dipped the chin
    bool _nod_down           = false;  // chin is down, waiting to come back up
    int _nod_yaw             = 0;      // yaw held across both halves of a nod

    // Quiet-room behaviour, paused while the server or the music has the head.
    stackchan::IdleMotionModifier* _idle_motion = nullptr;

    // Last time the idle sleep/shutdown timer was pushed back, so it is done
    // once a second rather than on every frame.
    uint32_t _last_engagement_tick = 0;

    // True once the head has been parked at the neutral pose, so the quiet
    // branch does not re-command the same position every frame.
    bool _at_neutral = false;

    // Beat LEDs: a dim glow while music plays, a bright flash on each beat,
    // dark when quiet. Throttled, since each refresh writes all 12 LEDs.
    int _led_hue              = -1;  // current beat colour, degrees; -1 = unset
    uint32_t _led_flash_tick  = 0;
    uint32_t _led_update_tick = 0;
    bool _leds_lit            = false;

    bool server_owns_head();
    bool server_dance_playing();
    void update_head_leds(bool music_playing);
    void park_at_neutral();
    void note_engagement();
    void check_auto_angle_sync_mode();
    void update_music_dance();
    void move_to_beat(float strength);
    void lift_chin();
};
