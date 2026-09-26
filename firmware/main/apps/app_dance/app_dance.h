/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "beat_detector.h"
#include <stackchan/stackchan.h>
#include <mooncake.h>
#include <memory>
#include <mutex>

/**
 * @brief
 *
 */
class AppDance : public mooncake::AppAbility {
public:
    AppDance();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    std::mutex _mutex;

    struct BleHandlerData_t {
        bool update_flag = false;
        char* data_ptr   = nullptr;
    };
    BleHandlerData_t _ble_avatar_data;
    BleHandlerData_t _ble_motion_data;
    BleHandlerData_t _ble_rgb_data;

    uint32_t _last_motion_cmd_tick = 0;

    // Music-reactive choreography. Tunables live in dance_params.h.
    dance::BeatDetector _beat_detector;
    int _beat_step           = 0;      // position in the optional sway cycle
    uint32_t _last_beat_tick = 0;      // when the last beat dipped the chin
    bool _nod_down           = false;  // chin is down, waiting to come back up
    int _nod_yaw             = 0;      // yaw held across both halves of a nod

    // Quiet-room behaviour: the same idle look-around Companion mode uses,
    // paused while the music has the head nodding.
    stackchan::IdleMotionModifier* _idle_motion = nullptr;

    // Last time the idle sleep/shutdown timer was pushed back, so it is
    // done once a second rather than on every 20ms frame.
    uint32_t _last_engagement_tick = 0;

    // True once the head has been parked at the neutral pose. Stops the
    // no-music branch re-commanding the same position every frame, which
    // would be constant servo bus traffic for no movement.
    bool _at_neutral = false;

    // Head LEDs: a dim glow while music plays, flashing bright on each beat
    // and dark when the room is quiet. Refreshing them writes all 12 LEDs
    // over I2C to the PY32 expander, so the update is throttled rather than
    // run every frame.
    int _led_hue               = -1;  // current beat colour, degrees; -1 = unset
    uint32_t _led_flash_tick   = 0;
    uint32_t _led_update_tick  = 0;
    bool _leds_lit             = false;

    void led_tap_test();
    void update_head_leds(bool music_playing);
    void park_at_neutral();
    void note_engagement();
    void check_auto_angle_sync_mode();
    void update_music_dance();
    void move_to_beat(float strength);
    void lift_chin();
};
