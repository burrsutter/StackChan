/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "beat_detector.h"
#include "dance_params.h"
#include <hal/hal.h>
#include <hal/board/config.h>
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <vector>

namespace dance {

namespace {

constexpr const char* _tag = "BeatDetect";

// Samples per analysis window. At the 24 kHz capture rate this is ~10.7 ms,
// fine enough to place a beat accurately and coarse enough that the maths
// costs nothing.
// Kept small because the beat is only detected at the END of a window, so
// the window length is dead time between the music and the head moving.
constexpr size_t WINDOW_FRAMES = 128;

// Windows of loudness history the current window is compared against.
// 192 * 5.3 ms is a little over a second -- long enough to average across a
// bar or two, short enough to follow a track getting louder or quieter.
constexpr size_t HISTORY_LEN = 192;

// How often to report the running tempo while music plays, for tuning.
constexpr uint32_t BPM_LOG_INTERVAL_MS = 10000;

// How often to report the ambient level while NOT hearing music. This is the
// number to set BeatDetectParams_t::noiseFloor against -- the gap between a
// quiet room and a sparse musical passage is narrow, and guessing it wrong
// either makes the robot nod at room noise or go still mid-track.
constexpr uint32_t QUIET_LOG_INTERVAL_MS = 5000;

}  // namespace

void BeatDetector::start()
{
    if (_running) {
        return;
    }
    _stop_requested = false;
    _running        = true;
    if (xTaskCreatePinnedToCore(task_entry, "beat", 1024 * 5, this, 2, nullptr, 1) != pdPASS) {
        mclog::tagError(_tag, "failed to create task");
        _running = false;
    }
}

void BeatDetector::stop()
{
    if (!_running) {
        return;
    }
    _stop_requested = true;

    // Bounded wait, for the same reason the camera wave task uses one: a read
    // stuck in the codec driver must not be able to hang app shutdown. The
    // task still exits on its own afterwards because _stop_requested stays set.
    const uint32_t deadline = GetHAL().millis() + 2000;
    while (_running && GetHAL().millis() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (_running) {
        mclog::tagWarn(_tag, "task did not stop within 2s, leaving it to exit on its own");
    }

    _beat_flag     = false;
    _music_playing = false;
}

bool BeatDetector::fetchBeat(float& strength)
{
    if (_beat_flag.exchange(false)) {
        strength = _beat_strength;
        return true;
    }
    return false;
}

void BeatDetector::task_entry(void* arg)
{
    static_cast<BeatDetector*>(arg)->run();
    vTaskDelete(nullptr);
}

void BeatDetector::run()
{
    mclog::tagInfo(_tag, "task started");

    if (!GetHAL().micCaptureStart()) {
        mclog::tagError(_tag, "could not open the microphone");
        _running = false;
        return;
    }

    std::vector<int16_t> samples;
    std::array<float, HISTORY_LEN> history{};
    size_t history_pos     = 0;
    size_t history_filled  = 0;
    float history_sum      = 0.0f;

    uint32_t last_beat_tick = 0;
    bool was_playing        = false;
    float prev_energy       = 0.0f;

    // Band-pass filter state and coefficients. For a one-pole low pass,
    // alpha = 2*pi*fc/fs is the standard small-angle approximation, which is
    // accurate well past the cutoffs used here.
    constexpr float kSampleRate = static_cast<float>(AUDIO_INPUT_SAMPLE_RATE);
    const float alpha_high      = 2.0f * 3.14159265f * kBeatDetect.bassHighHz / kSampleRate;
    const float alpha_low       = 2.0f * 3.14159265f * kBeatDetect.bassLowHz / kSampleRate;
    float lp1 = 0.0f, lp2 = 0.0f, dc = 0.0f;

    // Rolling tempo estimate, reported periodically to help with tuning.
    uint32_t bpm_window_start = GetHAL().millis();
    uint32_t bpm_beat_count   = 0;
    uint32_t quiet_log_tick   = GetHAL().millis();
    uint32_t above_floor_since = 0;
    uint32_t below_stop_since  = 0;

    // Recent bass hits, for the rhythm check. Recorded whether or not music
    // mode is on, since starting it now depends on them.
    std::array<uint32_t, 24> onsets{};
    size_t onset_count = 0;
    size_t onset_pos   = 0;
    float rhythm       = 0.0f;
    int rhythm_gaps    = 0;
    uint32_t rhythm_period_ms = 0;

    auto score_rhythm = [&](uint32_t now) {
        // Gaps between consecutive hits inside the window, oldest first.
        std::array<uint32_t, 24> times{};
        size_t n = 0;
        for (size_t i = 0; i < onset_count; i++) {
            const uint32_t t = onsets[(onset_pos + onsets.size() - onset_count + i) % onsets.size()];
            if (now - t <= kBeatDetect.rhythmWindowMs) {
                times[n++] = t;
            }
        }
        std::array<uint32_t, 24> gaps{};
        size_t g = 0;
        for (size_t i = 1; i < n; i++) {
            gaps[g++] = times[i] - times[i - 1];
        }
        rhythm_gaps = static_cast<int>(g);
        if (static_cast<int>(g) < kBeatDetect.rhythmMinGaps) {
            rhythm_period_ms = 0;
            return 0.0f;
        }

        // Typical gap = the period. Median, so a few stray hits do not drag it.
        std::array<uint32_t, 24> sorted = gaps;
        std::sort(sorted.begin(), sorted.begin() + g);
        const uint32_t period = sorted[g / 2];
        rhythm_period_ms      = period;
        if (period < kBeatDetect.rhythmMinPeriodMs) {
            return 0.0f;
        }

        int on_grid = 0;
        for (size_t i = 0; i < g; i++) {
            const float ratio   = static_cast<float>(gaps[i]) / static_cast<float>(period);
            const float nearest = std::clamp(std::round(ratio), 1.0f, 2.0f);
            if (std::fabs(ratio - nearest) <= kBeatDetect.rhythmTolerance) {
                on_grid++;
            }
        }
        return static_cast<float>(on_grid) / static_cast<float>(g);
    };

    while (!_stop_requested) {
        if (!GetHAL().micCaptureRead(samples, WINDOW_FRAMES)) {
            // Either capture was torn down under us or the codec errored;
            // either way there is nothing to analyse. Yield so a persistent
            // failure cannot spin this task at full speed.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Energy of the BASS BAND only. Measuring the whole spectrum means
        // vocals, cymbals and room noise all count, and the detector lands
        // everywhere except the beat; the kick and bass line carry the pulse
        // people actually hear as "the beat".
        //
        // Two one-pole low passes in series give a 12 dB/octave roll-off at
        // bassHighHz, and subtracting a much slower low pass removes DC and
        // sub-sonic rumble -- together a crude but effective band-pass.
        // Filter state persists across windows, so a beat spanning a window
        // boundary is not clipped.
        double sum_squares = 0.0;
        for (int16_t sample : samples) {
            const float value = static_cast<float>(sample);
            lp1 += alpha_high * (value - lp1);
            lp2 += alpha_high * (lp1 - lp2);
            dc += alpha_low * (lp2 - dc);
            const float band = lp2 - dc;
            sum_squares += static_cast<double>(band) * band;
        }
        const float energy = static_cast<float>(sum_squares / static_cast<double>(samples.size()));

        const uint32_t now = GetHAL().millis();

        // Compare against the recent average before folding this window in,
        // so a loud window is measured against its own past, not itself.
        if (history_filled == HISTORY_LEN) {
            const float average   = history_sum / static_cast<float>(HISTORY_LEN);
            const float threshold = average * kBeatDetect.sensitivity;

            // Music presence is judged on the SUSTAINED average, with a
            // little hysteresis so a level sitting near the floor does not
            // chatter in and out.
            //
            // Gating beats on this, rather than on the current window clearing
            // the floor, is what keeps a quiet room quiet: a single keystroke
            // is a sharp low-frequency transient, which is precisely what a
            // kick-drum detector is built to fire on. One key thunk used to be
            // enough to clear the floor on its own window and nod the head in
            // a silent room. The average barely moves for a single transient,
            // so it can tell a room with music in it from a room with a
            // keyboard in it.
            const float stop_level = kBeatDetect.noiseFloor * kBeatDetect.musicStopHysteresis;

            // Entering music mode needs the level held above the floor for
            // musicOnsetHoldMs. Without the hold, a burst of speech or a
            // handful of keystrokes tips the average over the line for a
            // second and the head starts nodding at nothing. Leaving it needs
            // the level held below the hysteresis level for musicStopGraceMs:
            // a quiet passage inside a song dips the bass for a second or two,
            // and stopping on the first dip made the whole show -- LEDs, face,
            // head -- stutter in and out every few seconds.
            // Bass hits are collected first, so the rhythm score is current.
            const bool loud_enough   = energy > threshold;
            const bool past_cooldown = now - last_beat_tick >= kBeatDetect.minBeatIntervalMs;
            const bool rising        = !kBeatDetect.requireRisingEnergy || energy > prev_energy;
            const bool hit           = loud_enough && past_cooldown && rising && average > stop_level;
            if (hit) {
                last_beat_tick     = now;
                onsets[onset_pos]  = now;
                onset_pos          = (onset_pos + 1) % onsets.size();
                onset_count        = std::min(onset_count + 1, onsets.size());
            }
            rhythm = score_rhythm(now);

            // Music = loud enough AND steady. Level alone let loud speech in.
            bool playing = was_playing;
            if (was_playing) {
                if (average > stop_level && rhythm >= kBeatDetect.rhythmStop) {
                    below_stop_since = 0;
                } else {
                    if (below_stop_since == 0) {
                        below_stop_since = now;
                    }
                    playing = now - below_stop_since < kBeatDetect.musicStopGraceMs;
                }
            } else if (average > kBeatDetect.noiseFloor && rhythm >= kBeatDetect.rhythmOnset) {
                if (above_floor_since == 0) {
                    above_floor_since = now;
                }
                playing = now - above_floor_since >= kBeatDetect.musicOnsetHoldMs;
            } else {
                above_floor_since = 0;
            }
            if (playing) {
                above_floor_since = 0;
            } else {
                below_stop_since = 0;
            }

            if (playing && hit) {
                bpm_beat_count++;

                // 0 at the threshold, 1 at twice the threshold or louder.
                float strength = 0.0f;
                if (threshold > 0.0f) {
                    strength = std::clamp(energy / threshold - 1.0f, 0.0f, 1.0f);
                }
                _beat_strength = strength;
                _beat_flag     = true;
            }

            // Deciding when to stop dancing is the app's call -- it waits out
            // kDanceMoves.musicTimeoutMs of no beats before returning to rest.
            _music_playing = playing;

            if (playing != was_playing) {
                mclog::tagInfo(_tag, "music {} (rhythm {:.2f} over {} gaps, period {} ms)",
                               playing ? "detected" : "stopped", rhythm, rhythm_gaps, rhythm_period_ms);
                was_playing      = playing;
                bpm_window_start = now;
                bpm_beat_count   = 0;
            }

            if (!playing && now - quiet_log_tick >= QUIET_LOG_INTERVAL_MS) {
                mclog::tagInfo(_tag, "quiet, energy {:.0f} (noise floor {:.0f}), rhythm {:.2f} over {} gaps, period {} ms",
                               average, kBeatDetect.noiseFloor, rhythm, rhythm_gaps, rhythm_period_ms);
                quiet_log_tick = now;
            }

            if (playing && now - bpm_window_start >= BPM_LOG_INTERVAL_MS) {
                const float minutes = static_cast<float>(now - bpm_window_start) / 60000.0f;
                const int bpm       = minutes > 0.0f ? static_cast<int>(bpm_beat_count / minutes) : 0;
                mclog::tagInfo(_tag, "{} beats in {} ms (~{} bpm), avg energy {:.0f}, rhythm {:.2f}, period {} ms",
                               bpm_beat_count, now - bpm_window_start, bpm, average, rhythm, rhythm_period_ms);
                bpm_window_start = now;
                bpm_beat_count   = 0;
            }
        }

        // Fold the window into the rolling history.
        if (history_filled == HISTORY_LEN) {
            history_sum -= history[history_pos];
        } else {
            history_filled++;
        }
        history[history_pos] = energy;
        history_sum += energy;
        history_pos = (history_pos + 1) % HISTORY_LEN;

        prev_energy = energy;
    }

    GetHAL().micCaptureStop();
    mclog::tagInfo(_tag, "task stopped");
    _running = false;
}

}  // namespace dance
