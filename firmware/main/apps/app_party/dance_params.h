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
    int nodDownSpeed = 650;
    int nodUpSpeed   = 420;

    /* ------------------------ Optional yaw sway --------------------------- */
    // Off by default: the nod is the motion. Set a value (try 150-250) to add
    // a slow side-to-side sway that advances one step every 4 beats.
    int yawCenter = 0;
    int yawSwing  = 0;

    // Mechanical yaw trim, in tenth-degrees. Commanding yaw 0 does not point
    // this unit's head straight ahead: the servo horn's mounting position sets
    // where zero physically lands, and at 0 the head sits slightly toward the
    // robot's right shoulder.
    //
    // VERIFIED ON HARDWARE 2026-09-27, by driving yaw directly from the
    // dashboard and watching the robot: +40 (4 degrees) is physically centred,
    // so at a commanded 0 the head sits a little toward its LEFT.
    //
    // The same session settled the sign convention, which is documented
    // nowhere else in this firmware:
    //
    //     NEGATIVE yaw -> robot's LEFT shoulder
    //     POSITIVE yaw -> robot's RIGHT shoulder
    //
    // (The remote's joystick mapping at joystick_handle.c:174 spans +-1280 but
    // never says which end is which, which is why this had to be measured.)
    int yawNeutralOffset = 40;

    // The resting pose is pitchCenter with yaw centred: the head parks there
    // when the app opens and returns there when the music stops, so a nod
    // always reads against the same still starting position.

    /* ----------------------- Safety clamps (hard stops) --------------------- */
    // Nothing the choreography computes is allowed outside this window,
    // whatever the amplitudes above say.
    int yawSafeMin   = -700;
    int yawSafeMax   = 700;
    int pitchSafeMin = 150;
    int pitchSafeMax = 600;

    /* -------------------------- Beat responsiveness ------------------------ */
    // Scale the dip with how hard the beat hit. 0.0 = every beat dips fully;
    // 1.0 = a beat only just over the threshold barely moves. Raised from 0.40
    // so he grooves -- big on strong beats, a bob on weak ones -- instead of
    // headbanging at full depth on every hit.
    float beatStrengthInfluence = 0.65f;
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

    // ...and it has to stay down there this long before the music counts as
    // stopped. Measured 2026-10-28: a 150-190 bpm track flickered stopped and
    // detected again every 2-5 s through its quieter passages without this.
    uint32_t musicStopGraceMs = 4000;

    // Require the energy to be RISING to call a beat, so the attack of a kick
    // triggers once instead of the whole sustained note re-triggering.
    bool requireRisingEnergy = true;

    // Refractory period. Caps the beat rate and stops one hit registering
    // across consecutive windows. 250 ms => at most 240 BPM.
    uint32_t minBeatIntervalMs = 250;
};

/*
 * Head LEDs while music is playing. All 12 are driven, both bars together.
 */
struct DanceLedParams_t {
    // A fresh random hue on every beat, rather than one fixed colour. Set
    // false to hold fixedColor instead.
    bool randomHue = true;

    // Consecutive hues are forced at least this far apart on the colour wheel
    // (degrees), so two beats running never look like the same colour.
    int minHueStepDeg = 70;

    // Used when randomHue is false. Defaults to the dance magenta that the
    // face uses in this mode.
    uint8_t fixedR = 0xC2;
    uint8_t fixedG = 0x15;
    uint8_t fixedB = 0x8A;

    // Resting brightness between beats, 0-1. The beat flashes to full and
    // fades back to this.
    float glowLevel = 0.18f;

    // How long the flash takes to fade back to the glow. Matched to the nod
    // so the light tracks the head rather than outrunning it.
    uint32_t fadeMs = 220;
};

/*
 * Party hard without injury: the face and LEDs carry the energy (they do not
 * wear), and the servos pace themselves and back off before damage.
 */
struct PartyShowParams_t {
    /* --------------------------- Face on the beat --------------------------- */
    // Happy eyes while music plays; the mouth pops open on each beat and
    // closes with the chin. Screen only -- no servo cost.
    int beatMouthOpen = 70;  // speaking uses 40-80
    int beatMouthRest = 0;

    /* ------------------------------ LED show -------------------------------- */
    // Alternate which bar flashes on each beat (right 0-5, left 6-11); the
    // other bar holds the glow. A beat at least this strong flashes both bars
    // at once, in complementary colours.
    float strongBeat = 0.75f;
};

struct PartySafetyParams_t {
    /* ------------------------------- Stamina -------------------------------- */
    // After this long dancing without a real break, rest: the face and LEDs
    // keep going, the head only bobs. A quiet gap this long resets the stint.
    uint32_t stintMs        = 5 * 60 * 1000;
    uint32_t staminaRestMs  = 30 * 1000;
    float staminaRestScale  = 0.30f;
    uint32_t stintResetGapMs = 20 * 1000;

    /* ---------------------------- Servo health ------------------------------ */
    // Each servo's status block is read this often (one bus transaction per
    // servo) and logged, so thresholds can be set from real numbers.
    uint32_t telemetryIntervalMs = 5000;

    // PROVISIONAL -- set before any long dance had been measured. Re-tune from
    // the "[PartySafety]" log lines once a few songs' worth exist.
    int warmC         = 50;    // nods shrink above this
    float warmScale   = 0.60f;
    int hotC          = 60;    // nodding stops above this...
    int cooledC       = 48;    // ...until every servo is back under this
    uint32_t minHeatRestMs = 30 * 1000;

    // Strain: high load on a head that is supposed to be still means it is
    // blocked or pushing against something. Readings are only taken while the
    // head is not animating, so an ordinary nod never counts.
    int strainLoad          = 650;  // out of 1000
    int strainSamples       = 2;    // consecutive polls before acting
    uint32_t strainRestMs   = 30 * 1000;
};

// The live values used by the PARTY app. Edit the struct defaults above.
inline constexpr DanceMoveParams_t kDanceMoves{};
inline constexpr BeatDetectParams_t kBeatDetect{};
inline constexpr DanceLedParams_t kDanceLeds{};
inline constexpr PartyShowParams_t kPartyShow{};
inline constexpr PartySafetyParams_t kPartySafety{};

}  // namespace dance
