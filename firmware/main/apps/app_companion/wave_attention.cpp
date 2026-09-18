/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "wave_attention.h"
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <stackchan/stackchan.h>
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstring>

namespace companion {

namespace {

constexpr const char* _tag = "WaveAttention";

// Luma grid resolution (camera streams 320x240 -> 20x20 px cells)
constexpr int GRID_W = 16;
constexpr int GRID_H = 12;

// A grid cell counts as motion when its luma changed by more than this
constexpr int DIFF_THRESHOLD = 14;
// Minimum number of motion cells to consider the frame "moving"
constexpr int MIN_MOTION_CELLS = 2;
// Centroid must travel at least this far (normalized) to register a direction
constexpr float MIN_SWING = 0.08f;
// Direction reversals needed within the window to call it a wave
constexpr int REVERSALS_FOR_WAVE  = 2;
constexpr uint32_t WINDOW_MS      = 1300;
// Consecutive motion samples must be at most this far apart to stay connected
constexpr uint32_t SAMPLE_LINK_MS = 400;
// Ignore frames for this long after the head stops moving (self-motion settle)
constexpr uint32_t SETTLE_MS      = 400;
// Minimum time between reported waves
constexpr uint32_t WAVE_COOLDOWN_MS = 3000;

}  // namespace

void WaveAttention::start()
{
    if (_running) {
        return;
    }
    _stop_requested = false;
    _running        = true;
    if (xTaskCreatePinnedToCore(task_entry, "wave", 1024 * 6, this, 2, nullptr, 1) != pdPASS) {
        mclog::tagError(_tag, "failed to create task");
        _running = false;
    }
}

void WaveAttention::stop()
{
    if (!_running) {
        return;
    }
    _stop_requested = true;

    // Bounded wait. Blocking forever here would freeze app shutdown if the
    // task were ever stuck in a driver call; giving up is safe because this
    // object outlives the app's close (apps are only destroyed on uninstall),
    // and _stop_requested stays set so the task still exits on its own.
    const uint32_t deadline = GetHAL().millis() + 2000;
    while (_running && GetHAL().millis() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (_running) {
        mclog::tagWarn(_tag, "task did not stop within 2s, leaving it to exit on its own");
    }

    _wave_flag = false;
}

bool WaveAttention::fetchWave(float& x)
{
    if (_wave_flag.exchange(false)) {
        x = _wave_x;
        return true;
    }
    return false;
}

void WaveAttention::task_entry(void* arg)
{
    static_cast<WaveAttention*>(arg)->run();
    vTaskDelete(nullptr);
}

void WaveAttention::run()
{
    mclog::tagInfo(_tag, "task started");

    auto camera = hal_bridge::board_get_camera();
    if (!camera) {
        mclog::tagError(_tag, "no camera available");
        _running = false;
        return;
    }

    uint8_t prev[GRID_W * GRID_H];
    uint8_t cur[GRID_W * GRID_H];
    bool have_prev = false;

    // Wave state machine
    float last_cx             = 0.0f;
    uint32_t last_cx_time     = 0;
    int last_dir              = 0;
    uint32_t reversal_times[8] = {0};
    int reversal_head         = 0;
    uint32_t settle_until     = 0;
    uint32_t last_wave_time   = 0;

    auto reset_wave_state = [&]() {
        have_prev     = false;
        last_cx_time  = 0;
        last_dir      = 0;
        reversal_head = 0;
        std::memset(reversal_times, 0, sizeof(reversal_times));
    };

    while (!_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(80));

        if (_paused) {
            reset_wave_state();
            continue;
        }

        // Everything moves in the frame while the head moves — skip and settle.
        // isAnimating(), not isMoving(): this runs on its own task, and
        // isMoving() reads the servo bus, which the main loop is already
        // using -- overlapping transactions cross replies between the two
        // servos ("wrong servo id") and drop reads. SETTLE_MS covers the
        // physical lag behind the animation.
        if (GetStackChan().motion().isAnimating()) {
            settle_until = GetHAL().millis() + SETTLE_MS;
            reset_wave_state();
            continue;
        }
        const uint32_t now = GetHAL().millis();
        if (now < settle_until) {
            continue;
        }

        if (!camera->SampleLumaGrid(cur, GRID_W, GRID_H)) {
            continue;
        }
        if (!have_prev) {
            std::memcpy(prev, cur, sizeof(cur));
            have_prev = true;
            continue;
        }

        // Frame difference: motion cells and their weighted x centroid
        int motion_cells  = 0;
        int32_t weight    = 0;
        int32_t weighted_x = 0;
        for (int gy = 0; gy < GRID_H; gy++) {
            for (int gx = 0; gx < GRID_W; gx++) {
                const int idx  = gy * GRID_W + gx;
                const int diff = std::abs(static_cast<int>(cur[idx]) - static_cast<int>(prev[idx]));
                if (diff > DIFF_THRESHOLD) {
                    motion_cells++;
                    weight += diff;
                    weighted_x += diff * gx;
                }
            }
        }
        std::memcpy(prev, cur, sizeof(cur));

        if (motion_cells < MIN_MOTION_CELLS || weight == 0) {
            continue;
        }

        // Normalize centroid to [-1, 1]
        const float cx = (static_cast<float>(weighted_x) / weight) / (GRID_W - 1) * 2.0f - 1.0f;

        if (last_cx_time != 0 && now - last_cx_time <= SAMPLE_LINK_MS) {
            const float dx = cx - last_cx;
            if (std::abs(dx) > MIN_SWING) {
                const int dir = dx > 0 ? 1 : -1;
                if (last_dir != 0 && dir != last_dir) {
                    reversal_times[reversal_head] = now;
                    reversal_head                 = (reversal_head + 1) % 8;

                    int recent = 0;
                    for (uint32_t t : reversal_times) {
                        if (t != 0 && now - t <= WINDOW_MS) {
                            recent++;
                        }
                    }
                    if (recent >= REVERSALS_FOR_WAVE && now - last_wave_time > WAVE_COOLDOWN_MS) {
                        mclog::tagInfo(_tag, "wave detected at x {:.2f} ({} cells)", cx, motion_cells);
                        last_wave_time = now;
                        _wave_x        = cx;
                        _wave_flag     = true;
                        reset_wave_state();
                        continue;
                    }
                }
                last_dir = dir;
                last_cx  = cx;
                last_cx_time = now;
            }
        } else {
            last_cx      = cx;
            last_cx_time = now;
            last_dir     = 0;
        }
    }

    mclog::tagInfo(_tag, "task stopped");
    _running = false;
}

}  // namespace companion
