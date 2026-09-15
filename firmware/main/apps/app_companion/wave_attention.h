/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <atomic>
#include <cstdint>

namespace companion {

/**
 * @brief Camera-based hand-wave detection. Samples low-res luma frames in a
 * background task, frame-differences them, and flags a wave when the motion
 * centroid oscillates horizontally (>= 2 direction reversals within ~1.3s).
 * Detection pauses automatically while the head is moving to avoid
 * self-motion false positives.
 */
class WaveAttention {
public:
    void start();
    void stop();

    /**
     * @brief Consume a pending wave event.
     *
     * @param x On true, the horizontal position of the wave in the camera
     *          view, normalized to [-1, 1] (negative = image left)
     * @return true once per detected wave
     */
    bool fetchWave(float& x);

    void setPaused(bool paused)
    {
        _paused = paused;
    }

private:
    static void task_entry(void* arg);
    void run();

    std::atomic<bool> _running{false};
    std::atomic<bool> _stop_requested{false};
    std::atomic<bool> _paused{false};
    std::atomic<bool> _wave_flag{false};
    std::atomic<float> _wave_x{0.0f};
};

}  // namespace companion
