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

    void check_auto_angle_sync_mode();
    void update_music_dance();
    void move_to_beat(float strength);
    void lift_chin();
};
