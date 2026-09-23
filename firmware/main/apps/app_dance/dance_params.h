/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>

namespace dance {

/*
 * ============================================================================
 * TUNING BLOCK -- edit these, rebuild, reflash. Nothing else needs changing.
 * ============================================================================
 *
 * Angles are in TENTHS OF A DEGREE, the unit the servo layer uses
 * (`firmware/main/hal/hal_servo.cpp`), so 200 = 20 degrees.
 *
 * Hardware travel limits, for reference:
 *     yaw   [-1280, 1280]
 *     pitch [   30,  870]
 *
 * The defaults sit well inside those, and inside what the idle-motion
 * behaviour already exercises daily (yaw +-800, pitch 0-600). The servos are
 * small plastic-geared SCS units and the pitch axis holds the head against
 * gravity, so the aim is a nod that reads clearly without ever parking the
 * head at an extreme or leaning on a hard stop.
 *
 * Every target is clamped to the safety window below before it reaches a
 * servo, so raising an amplitude can never overextend anything -- widen the
 * safety window too if you really want more travel.
 */
struct DanceMoveParams_t {
    /* ------------------------------ The nod ------------------------------- */

    // Resting pitch the head returns to between beats. Each beat dips the
    // chin from here and comes back.
    int pitchCenter = 330;

    // How far the chin dips on a full-strength beat.
    int nodDip = 190;  // 19 deg

    // Which way is "chin down". Physical pitch polarity is not documented
    // anywhere in the firmware (the head-pet and camera-wave code carry their
    // own sign constants for the same reason). -1 dips the chin: verified on
    // hardware 2026-09-23. Flip to +1 if a future build nods the wrong way --
    // the safety window keeps travel safe either way.
    int chinDipSign = -1;

    // How long the chin stays down before coming back up. Short and snappy
    // reads as a nod; too long and it just looks like the head sagging.
    uint32_t nodHoldMs = 90;

    // Drive speeds, 0-1000. The dip is sharp, the recovery gentler -- that
    // asymmetry is what makes it read as a nod rather than a twitch.
    //
    // The dip speed also sets how late the nod lands: the servo runs a spring
    // animation, and a soft spring takes ~200 ms to reach the bottom, which
    // at 138 BPM is half a beat behind the music. Driving it hard cuts that
    // roughly in half. Lower it if the head starts to sound harsh.
    int nodDownSpeed = 900;
    int nodUpSpeed   = 420;

    /* ------------------------ Optional yaw sway --------------------------- */
    // Off by default: the nod is the motion. Set a value (try 150-250) to add
    // a slow side-to-side sway that advances one step every 4 beats.
    int yawCenter = 0;
    int yawSwing  = 0;

    // Note: there is deliberately no resting-pose setting here. When the music
    // stops the head is handed back to the Companion-style idle look-around,
    // which owns its own posture.

    /* ----------------------- Safety clamps (hard stops) --------------------- */
    // Nothing the choreography computes is allowed outside this window,
    // whatever the amplitudes above say.
    int yawSafeMin   = -700;
    int yawSafeMax   = 700;
    int pitchSafeMin = 150;
    int pitchSafeMax = 600;

    /* -------------------------- Beat responsiveness ------------------------ */
    // Scale the dip with how hard the beat hit. 0.0 = every beat dips fully;
    // 1.0 = a beat only just over the threshold barely moves.
    float beatStrengthInfluence = 0.40f;
};

/*
 * Beat detection tuning.
 *
 * The detector listens to the BASS only -- a band-pass roughly covering kick
 * drum and bass line -- and calls a beat when that band's energy jumps above
 * its own recent average. Listening to the full spectrum picks up vocals,
 * cymbals and room noise and lands all over the place; the low band is where
 * the "hard deep beat" actually lives.
 */
struct BeatDetectParams_t {
    // Band-pass edges in Hz. Kick drums sit around 50-100 Hz; the upper edge
    // keeps snares and vocals out, the lower edge removes DC and rumble.
    float bassLowHz  = 30.0f;
    float bassHighHz = 160.0f;

    // A beat needs the current window to be this many times louder than the
    // recent average of the same band. Lower = more sensitive, higher = only
    // the hardest hits. 1.2-1.8 is the useful range.
    float sensitivity = 1.40f;

    // Absolute floor for the bass band, in mean-square units of int16
    // samples. Below this the room counts as quiet and nothing is reported,
    // so hum and handling noise cannot drive the servos.
    //
    // To tune: the detector logs "avg energy" every 10 s while music plays.
    // Put this well under that figure but above what it reads in silence.
    // Measured on hardware (2026-09-23): the bass band swings hugely with the
    // arrangement -- ~60k in a sparse passage against ~870k once the bass
    // drops in, while a quiet room sits near 40k. That leaves a narrow gap,
    // so this is the one value worth re-measuring in your actual room: the
    // detector logs "quiet, energy N" every 5 s whenever it is not hearing
    // music. Set this comfortably above that N, and below the sparse-passage
    // figure in the BPM lines.
    float noiseFloor = 50000.0f;

    // Level alone cannot separate a voice from a track: measured in this room
    // the bass band reads ~8-14k for quiet typing, ~21-28k while actively
    // typing or talking with bursts past 40k, and ~60k upward once music is
    // on. The bands nearly touch, so time is the better discriminator --
    // speech and keystrokes are bursty, music is continuous. The level has to
    // stay above the floor for this long before the head starts nodding.
    uint32_t musicOnsetHoldMs = 1200;

    // Once music is detected, it has to fall to this fraction of the floor
    // before counting as stopped. Stops a level hovering near the threshold
    // from chattering in and out and stuttering the nod.
    float musicStopHysteresis = 0.7f;

    // Require the energy to be RISING to call a beat, so the attack of a kick
    // triggers once instead of the whole sustained note re-triggering.
    bool requireRisingEnergy = true;

    // Refractory period. Caps the beat rate and stops one hit registering
    // across consecutive windows. 250 ms => at most 240 BPM.
    uint32_t minBeatIntervalMs = 250;
};

// The live values used by the DANCE app. Edit the struct defaults above.
inline constexpr DanceMoveParams_t kDanceMoves{};
inline constexpr BeatDetectParams_t kBeatDetect{};

}  // namespace dance
