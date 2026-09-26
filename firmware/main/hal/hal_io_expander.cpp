/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "drivers/PY32IOExpander_Class/PY32IOExpander_Class.hpp"
#include <mooncake_log.h>
#include <memory>

static const std::string_view _tag = "HAL-IOE";

static std::unique_ptr<m5::PY32IOExpander_Class> _io_expander;

// ---------------------------------------------------------------------------
// TEMPORARY DIAGNOSTIC -- set back to 0 to restore normal boot.
//
// Walks the 12 WS2812C positions one at a time with serial logging, then
// lights the two rows as blocks. The spec says two rows of six; on this unit
// only 0-5 have ever lit. Because WS2812s are daisy-chained, a break at index
// 6 kills everything downstream, so the question this answers is whether ANY
// of 6-11 can be driven individually -- and whether the cut is exactly at the
// row boundary.
// ---------------------------------------------------------------------------
#define DIAG_LED_WALK 0

// ---------------------------------------------------------------------------
// TEMPORARY DIAGNOSTIC -- set back to 0 to restore normal boot.
//
// The PY32 LED driver addresses up to 32 positions (setLedCount caps at 32,
// colours live at REG_LED_RAM_START + index*2). Every test so far only ever
// wrote indices 0-11, so if the second row is addressed anywhere outside that
// range it would have been missed entirely. This lights ALL 32 slots at once,
// then sweeps 12-31 individually. Answers in one look whether row B is an
// addressing problem (firmware-fixable) or physically absent.
// ---------------------------------------------------------------------------
#define DIAG_LED_MAX_SCAN 0

#if DIAG_LED_WALK
static void run_led_walk_test()
{
    constexpr int kCount   = 12;
    constexpr int kPasses  = 2;
    constexpr int kHoldMs  = 700;

    mclog::tagWarn(_tag, "=== LED WALK TEST: {} positions, {} passes ===", kCount, kPasses);

    for (int pass = 0; pass < kPasses; pass++) {
        for (int i = 0; i < kCount; i++) {
            GetHAL().showRgbColor(0, 0, 0);
            GetHAL().setRgbColor(i, 255, 255, 255);
            GetHAL().refreshRgb();
            mclog::tagWarn(_tag, "LED WALK pass {} -> index {:2} ON (white)  [{}]", pass + 1, i,
                           i < 6 ? "row A / known good" : "row B / suspect");
            vTaskDelay(pdMS_TO_TICKS(kHoldMs));
        }
    }

    // Blocks, so a whole row is unmistakable even if single LEDs are dim.
    GetHAL().showRgbColor(0, 0, 0);
    for (int i = 0; i < 6; i++) {
        GetHAL().setRgbColor(i, 0, 255, 0);
    }
    GetHAL().refreshRgb();
    mclog::tagWarn(_tag, "LED WALK: row A (0-5) GREEN -- expect the stage-right bar lit");
    vTaskDelay(pdMS_TO_TICKS(3000));

    GetHAL().showRgbColor(0, 0, 0);
    for (int i = 6; i < 12; i++) {
        GetHAL().setRgbColor(i, 255, 0, 0);
    }
    GetHAL().refreshRgb();
    mclog::tagWarn(_tag, "LED WALK: row B (6-11) RED -- anything lit anywhere?");
    vTaskDelay(pdMS_TO_TICKS(5000));

    GetHAL().showRgbColor(0, 0, 0);
    mclog::tagWarn(_tag, "=== LED WALK TEST complete ===");
}
#endif

#if DIAG_LED_MAX_SCAN
static void run_led_max_scan()
{
    constexpr int kMax = 32;

    mclog::tagWarn(_tag, "=== LED MAX SCAN: driver supports {} positions, we normally use 12 ===", kMax);

    // Tell the controller there are 32 LEDs, then light every slot at once.
    _io_expander->setLedCount(kMax);
    for (int i = 0; i < kMax; i++) {
        _io_expander->setLedColor(i, (uint8_t)255, (uint8_t)255, (uint8_t)255);
    }
    _io_expander->refreshLeds();
    mclog::tagWarn(_tag, "ALL {} SLOTS WHITE -- look now. More than one bar lit?", kMax);
    vTaskDelay(pdMS_TO_TICKS(10000));

    // Now sweep only the slots beyond the normal range, in case the second row
    // is addressed late rather than at 6-11.
    mclog::tagWarn(_tag, "sweeping slots 12-31 individually (red)");
    for (int i = 12; i < kMax; i++) {
        for (int j = 0; j < kMax; j++) {
            _io_expander->setLedColor(j, (uint8_t)0, (uint8_t)0, (uint8_t)0);
        }
        _io_expander->setLedColor(i, (uint8_t)255, (uint8_t)0, (uint8_t)0);
        _io_expander->refreshLeds();
        mclog::tagWarn(_tag, "slot {:2} RED", i);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Restore the real configuration.
    for (int j = 0; j < kMax; j++) {
        _io_expander->setLedColor(j, (uint8_t)0, (uint8_t)0, (uint8_t)0);
    }
    _io_expander->refreshLeds();
    _io_expander->setLedCount(12);
    GetHAL().showRgbColor(0, 0, 0);
    mclog::tagWarn(_tag, "=== LED MAX SCAN complete, count restored to 12 ===");
}
#endif

void Hal::io_expander_init()
{
    mclog::tagInfo(_tag, "init");

    auto i2c_bus        = hal_bridge::board_get_i2c_bus();
    _io_expander        = std::make_unique<m5::PY32IOExpander_Class>(i2c_bus);
    uint32_t start_tick = GetHAL().millis();

    // PY32 IO Expander may boot slowly, wait for it
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));

        if (GetHAL().millis() - start_tick > 1200) {
            mclog::tagError(_tag, "init timeout");
            _io_expander.reset();
            break;
        }

        if (_io_expander->begin()) {
            break;
        }
        mclog::tagInfo(_tag, "init failed, retrying...");
    }

    if (_io_expander) {
        // VM EN
        _io_expander->setDirection(0, true);  // Output
        _io_expander->setPullMode(0, true);   // Pull-up
        GetHAL().setServoPowerEnabled(true);
        vTaskDelay(pdMS_TO_TICKS(200));

        // RGB
        _io_expander->setDirection(13, true);   // Output
        _io_expander->setPullMode(13, true);    // Pull-up
        _io_expander->setDriveMode(13, false);  // Push-pull
        _io_expander->setLedCount(12);
        vTaskDelay(pdMS_TO_TICKS(200));
        GetHAL().showRgbColor(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        GetHAL().showRgbColor(0, 0, 0);

        mclog::tagInfo(_tag, "init done");

#if DIAG_LED_WALK
        run_led_walk_test();
#endif
#if DIAG_LED_MAX_SCAN
        run_led_max_scan();
#endif
    }
}

// ---------------------------------------------------------------------------
// TEMPORARY DIAGNOSTIC -- set back to 0 to restore normal operation.
//
// When 1, the servo rail (VM EN) is never powered: the head stays limp and
// draws nothing. This exists to answer one question -- the board shuts itself
// off at random intervals (117-511s observed) with healthy rails and no
// software path, while the servo bus logs 61-658 errors per run. Running with
// the servos unpowered says whether that subsystem is causing the shutdowns.
// ---------------------------------------------------------------------------
#define DIAG_DISABLE_SERVO_POWER 0

void Hal::setServoPowerEnabled(bool enabled)
{
    if (!_io_expander) {
        return;
    }
#if DIAG_DISABLE_SERVO_POWER
    if (enabled) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            mclog::tagWarn(_tag, "DIAG build: servo power is held OFF (DIAG_DISABLE_SERVO_POWER=1)");
        }
        _io_expander->digitalWrite(0, false);
        return;
    }
#endif
    _io_expander->digitalWrite(0, enabled ? true : false);
}

void Hal::setRgbLedCount(uint8_t count)
{
    if (!_io_expander) {
        return;
    }
    _io_expander->setLedCount(count);
}

void Hal::setRgbColor(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!_io_expander) {
        return;
    }
    _io_expander->setLedColor(index, r, g, b);
}

void Hal::refreshRgb()
{
    if (!_io_expander) {
        return;
    }
    _io_expander->refreshLeds();
}

void Hal::showRgbColor(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 12; i++) {
        setRgbColor(i, r, g, b);
    }
    refreshRgb();
}
