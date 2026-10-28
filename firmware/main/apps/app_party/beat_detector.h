/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <atomic>
#include <cstdint>

namespace dance {

/**
 * @brief Microphone-driven beat detection.
 *
 * Runs a background task that reads short windows of mic audio, measures the
 * energy of each, and reports a beat when a window jumps above the running
 * average of the last second or so -- the standard energy-based approach,
 * which is cheap enough to leave running and works well on music with an
 * audible percussive pulse.
 *
 * This class only listens. It never touches the servos: the servo bus is
 * half-duplex and owned by the motion update loop, and issuing commands or
 * reads from a second task corrupts transactions on it. The app polls
 * fetchBeat() from its own loop and does the moving there.
 */
class BeatDetector {
public:
    void start();
    void stop();

    /**
     * @brief Consume a pending beat.
     *
     * @param strength How far over the threshold the beat was, from 0.0 (only
     *                 just detected) upward, clamped at 1.0 for a beat at
     *                 twice the threshold or louder.
     * @return true once per detected beat
     */
    bool fetchBeat(float& strength);

    /**
     * @brief Whether music is currently audible -- true while the room is
     * above the noise floor and beats are still arriving.
     */
    bool isMusicPlaying() const
    {
        return _music_playing;
    }

private:
    static void task_entry(void* arg);
    void run();

    std::atomic<bool> _running{false};
    std::atomic<bool> _stop_requested{false};
    std::atomic<bool> _beat_flag{false};
    std::atomic<float> _beat_strength{0.0f};
    std::atomic<bool> _music_playing{false};
};

}  // namespace dance
