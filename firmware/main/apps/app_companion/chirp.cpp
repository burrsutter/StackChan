/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "chirp.h"
#include <hal/board/config.h>
#include <board.h>
#include <audio/audio_codec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace companion {

namespace {

std::atomic<bool> _playing{false};

// Append a sine sweep from freqStart to freqEnd with a 10ms attack and
// exponential decay envelope
void append_note(std::vector<int16_t>& out, float freqStart, float freqEnd, uint32_t durationMs, float amplitude)
{
    constexpr float rate    = AUDIO_OUTPUT_SAMPLE_RATE;
    const uint32_t samples  = static_cast<uint32_t>(rate * durationMs / 1000);
    const uint32_t attack   = static_cast<uint32_t>(rate * 10 / 1000);
    float phase             = 0.0f;

    for (uint32_t i = 0; i < samples; i++) {
        const float t    = static_cast<float>(i) / samples;
        const float freq = freqStart + (freqEnd - freqStart) * t;
        phase += 2.0f * static_cast<float>(M_PI) * freq / rate;

        float env = std::exp(-3.0f * t);
        if (i < attack) {
            env *= static_cast<float>(i) / attack;
        }

        out.push_back(static_cast<int16_t>(std::sin(phase) * env * amplitude * 32767.0f));
    }
}

void append_silence(std::vector<int16_t>& out, uint32_t durationMs)
{
    out.insert(out.end(), AUDIO_OUTPUT_SAMPLE_RATE * durationMs / 1000, 0);
}

void chirp_task(void*)
{
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        std::vector<int16_t> pcm;
        append_note(pcm, 1100.0f, 1650.0f, 110, 0.32f);
        append_silence(pcm, 45);
        append_note(pcm, 1450.0f, 2100.0f, 150, 0.32f);
        append_silence(pcm, 30);

        codec->EnableOutput(true);
        constexpr size_t chunk_frames = 512;
        std::vector<int16_t> chunk;
        for (size_t offset = 0; offset < pcm.size(); offset += chunk_frames) {
            const size_t frames = std::min(chunk_frames, pcm.size() - offset);
            chunk.assign(pcm.begin() + offset, pcm.begin() + offset + frames);
            codec->OutputData(chunk);
        }
        codec->EnableOutput(false);
    }

    _playing = false;
    vTaskDelete(nullptr);
}

}  // namespace

void play_happy_chirp()
{
    bool expected = false;
    if (!_playing.compare_exchange_strong(expected, true)) {
        return;
    }
    if (xTaskCreate(chirp_task, "chirp", 4096, nullptr, 3, nullptr) != pdPASS) {
        _playing = false;
    }
}

}  // namespace companion
