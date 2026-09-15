/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "wave_attention.h"
#include <mooncake.h>
#include <stackchan/stackchan.h>
#include <atomic>
#include <cstdint>

/**
 * @brief Quiet desktop companion: lively idle behavior (blinking, breathing,
 * random expressions and head movements), happy chirp on head petting, and
 * camera-based wave detection — with no microphone, no wake word and no AI
 * agent. Voice mode stays opt-in via the AI.AGENT app.
 *
 */
class AppCompanion : public mooncake::AppAbility {
public:
    AppCompanion();

    // Override lifecycle callbacks
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    companion::WaveAttention _wave;
    stackchan::IdleMotionModifier* _idle_motion       = nullptr;
    uitk::lvgl_cpp::Container* _face_panel = nullptr;
    int _pet_signal_id                                = -1;
    std::atomic<uint32_t> _last_chirp_ms{0};
    uint32_t _wave_reaction_until = 0;

    void react_to_wave(float x);
    void end_wave_reaction();
};
