/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace companion {

// Play a short synthesized happy chirp on the speaker.
// Non-blocking (runs in its own one-shot task); overlapping requests are dropped.
void play_happy_chirp();

}  // namespace companion
