/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "drivers/FTServo_Arduino/src/SCSCL.h"
#include <stackchan/stackchan.h>
#include <smooth_ui_toolkit.hpp>
#include <mooncake_log.h>
#include <settings.h>
#include <string>

using namespace smooth_ui_toolkit;
using namespace stackchan::motion;

static SCSCL _scs_bus;

/*
 * Servo bus watchdog.
 *
 * Both servos share one half-duplex UART bus. On the K151 the servos have
 * been seen to stop answering entirely (every read returns -1, the head
 * freezes) in a way that survives an ESP32 reset but clears with a power
 * cycle -- so the fault sits on the servo/VM-rail side, not in the UART
 * driver. The servo rail is switched by the PY32 IO expander (VM EN), so
 * when the bus has been silent for a while we pulse that rail off and on
 * to give the servos a real power cycle, with exponential backoff so a
 * genuinely broken bus is not hammered forever.
 *
 * Everything here is non-blocking: read paths report their outcome, the
 * write path ticks the state machine (reads stop during an animation, so
 * timing cannot depend on them alone), and it all runs on the motion
 * update loop against millis().
 */
class ServoBusWatchdog {
public:
    static inline const std::string _tag = "ServoBus";

    // Called after every bus read. `ok` is whether the servo answered with
    // a valid frame; `err` is the driver's last error code (SCS_ERR_LIST).
    void noteRead(bool ok, uint8_t err)
    {
        const uint32_t now = GetHAL().millis();

        if (ok) {
            if (_failing) {
                mclog::tagInfo(_tag, "bus recovered after {} ms, {} power pulse(s), battery {}", now - _fail_since,
                               _pulse_count, battery_info());
            }
            _failing     = false;
            _pulse_count = 0;
            _state       = State::Idle;
            return;
        }

        if (!_failing) {
            _failing    = true;
            _fail_since = now;
            _next_pulse = now + kFirstPulseDelayMs;
            _last_err   = err;
            mclog::tagWarn(_tag, "bus reads failing ({}), battery {}; will pulse servo power in {} ms if it stays silent",
                           err_name(err), battery_info(), kFirstPulseDelayMs);
            return;
        }

        _last_err = err;
        tick();
    }

    // Advance the power-pulse state machine. Called from every bus access,
    // read or write, so the rail is not left off for a whole animation.
    void tick()
    {
        if (!_failing) {
            return;
        }
        const uint32_t now = GetHAL().millis();
        const uint8_t err  = _last_err;

        switch (_state) {
            case State::Idle:
                if (static_cast<int32_t>(now - _next_pulse) >= 0) {
                    _pulse_count++;
                    mclog::tagWarn(_tag, "bus silent for {} ms ({}), battery {}; pulsing servo power, attempt {}",
                                   now - _fail_since, err_name(err), battery_info(), _pulse_count);
                    GetHAL().setServoPowerEnabled(false);
                    _power_off_tick = now;
                    _state          = State::PowerOff;
                }
                break;

            case State::PowerOff:
                if (now - _power_off_tick >= kPowerOffMs) {
                    GetHAL().setServoPowerEnabled(true);
                    // Back off: 3s, 6s, 12s ... capped, so a dead bus is not thrashed.
                    uint32_t delay = kFirstPulseDelayMs << (_pulse_count < 6 ? _pulse_count : 6);
                    if (delay > kMaxPulseDelayMs) {
                        delay = kMaxPulseDelayMs;
                    }
                    _next_pulse = now + delay;
                    _state      = State::Idle;
                    mclog::tagInfo(_tag, "servo power back on; next pulse in {} ms if still silent", delay);
                }
                break;
        }
    }

private:
    enum class State { Idle, PowerOff };

    static constexpr uint32_t kFirstPulseDelayMs = 3000;   // silence tolerated before the first pulse
    static constexpr uint32_t kMaxPulseDelayMs   = 60000;  // backoff ceiling between pulses
    static constexpr uint32_t kPowerOffMs        = 300;    // how long VM EN is held low per pulse

    // Battery state at the moment of a bus event, so a future freeze's log
    // shows whether it coincided with low charge or with charging draw.
    static std::string battery_info()
    {
        return std::to_string(GetHAL().getBatteryLevel()) + "% " +
               (GetHAL().isBatteryCharging() ? "charging" : "discharging");
    }

    static const char* err_name(uint8_t err)
    {
        switch (err) {
            case ERR_NO_REPLY:
                return "no reply";
            case ERR_CRC_CMP:
                return "bad checksum";
            case ERR_SLAVE_ID:
                return "wrong servo id";
            case ERR_BUFF_LEN:
                return "bad length";
            default:
                return "unknown";
        }
    }

    bool _failing            = false;
    State _state             = State::Idle;
    uint32_t _fail_since     = 0;
    uint32_t _next_pulse     = 0;
    uint32_t _power_off_tick = 0;
    uint32_t _pulse_count    = 0;
    uint8_t _last_err        = 0;
};

static ServoBusWatchdog _bus_watchdog;

struct ServoConfig_t {
    int id             = -1;
    int defaultZeroPos = 0;
    Vector2i angleLimit;
    Vector2i rawPosLimit;
    std::string settingNs;
    std::string settingZeroPositionKey;
    bool enablePwmMode = false;
    bool enableStallProtection = false;
};

class ScsServo : public Servo {
public:
    static inline const std::string _tag = "ScsServo";

    ScsServo(const ServoConfig_t& config) : _config(config), _runtime_raw_pos_limit(config.rawPosLimit)
    {
    }

    void init() override
    {
        reset_runtime_limits();
        set_angle_limit(_config.angleLimit);
        get_zero_pos_from_nvs();
        Servo::init();
    }

    void get_zero_pos_from_nvs()
    {
        _zero_pos     = _config.defaultZeroPos;
        bool is_valid = false;

        {
            Settings settings(_config.settingNs, false);
            int nvs_zero_pos = settings.GetInt(_config.settingZeroPositionKey, -1);

            // Limit check
            if (nvs_zero_pos >= _config.rawPosLimit.x && nvs_zero_pos <= _config.rawPosLimit.y) {
                _zero_pos = nvs_zero_pos;
                is_valid  = true;
                mclog::tagInfo(_tag, "id: {} get zero pos: {} from settings", _config.id, _zero_pos);
            } else {
                is_valid = false;
                mclog::tagWarn(_tag, "id: {} get invalid zero pos: {} from settings", _config.id, nvs_zero_pos);
            }
        }

        if (!is_valid) {
            _zero_pos = _config.defaultZeroPos;
            mclog::tagInfo(_tag, "id: {} override zero pos to default: {}", _config.id, _zero_pos);

            Settings settings(_config.settingNs, true);
            settings.SetInt(_config.settingZeroPositionKey, _zero_pos);
        }
    }

    void set_angle_impl(int angle) override
    {
        int mapped_angle = _zero_pos + angle * 16 / 5 / 10;  // 一步对应 0.3125度, 0.3125 = 5/16
        mapped_angle     = uitk::clamp(mapped_angle, _runtime_raw_pos_limit.x, _runtime_raw_pos_limit.y);

        // mclog::tagInfo(_tag, "id: {} mapped angle: {}", _id, mapped_angle);

        _bus_watchdog.tick();

        if (update_stall_protection(mapped_angle)) {
            return;
        }

        check_mode(Mode::Position);
        _scs_bus.WritePos(_config.id, mapped_angle, 20, 0);
    }

    int getCurrentAngle() override
    {
        int current_pos = _scs_bus.ReadPos(_config.id);
        _bus_watchdog.noteRead(current_pos >= 0, _scs_bus.getLastError());
        if (!is_raw_pos_valid(current_pos)) {
            int fallback_angle = uitk::clamp(Servo::getCurrentAngle(), getAngleLimit().x, getAngleLimit().y);
            mclog::tagWarn(_tag, "id: {} ignore invalid current pos: {}, fallback angle: {}", _config.id, current_pos,
                           fallback_angle);
            return fallback_angle;
        }

        int angle = raw_pos_to_angle(current_pos);
        angle     = uitk::clamp(angle, getAngleLimit().x, getAngleLimit().y);
        // mclog::tagInfo(_tag, "id: {} current pos: {} angle: {}", _id, current_pos, angle);
        return angle;
    }

    bool is_moving_impl() override
    {
        int moving = _scs_bus.ReadMove(_config.id);
        // mclog::tagInfo(_tag, "id: {} moving: {}", _id, moving);
        _bus_watchdog.noteRead(moving >= 0, _scs_bus.getLastError());

        // ReadMove returns -1 when the servo does not answer on the bus.
        // Passing that through as "moving" (it is != 0) deadlocks anything
        // that waits for the head to be still -- idle motion stops animating
        // and never recovers. A failed read is not evidence of movement, so
        // report "not moving" and let callers proceed.
        if (moving < 0) {
            if (!_move_read_failing) {
                _move_read_failing = true;
                mclog::tagWarn(_tag, "id: {} move-state read failing; treating as not moving", _config.id);
            }
            return false;
        }
        if (_move_read_failing) {
            _move_read_failing = false;
            mclog::tagInfo(_tag, "id: {} move-state read recovered", _config.id);
        }
        return moving != 0;
    }

    void setTorqueEnabled(bool enabled) override
    {
        Servo::setTorqueEnabled(enabled);
        _scs_bus.EnableTorque(_config.id, enabled ? 1 : 0);
        // mclog::tagInfo(_tag, "id: {} set torque: {}", _id, enabled);
    }

    bool getTorqueEnabled() override
    {
        int torque_enable = _scs_bus.ReadToqueEnable(_config.id);
        // mclog::tagInfo(_tag, "id: {} torque enable: {}", _id, torque_enable);
        return torque_enable > 0;
    }

    void setCurrentAngleAsZero() override
    {
        int current_pos = _scs_bus.ReadPos(_config.id);
        if (!is_raw_pos_valid(current_pos)) {
            mclog::tagWarn(_tag, "id: {} ignore invalid zero calibration pos: {}, keep zero pos: {}", _config.id,
                           current_pos, _zero_pos);
            return;
        }

        _zero_pos = current_pos;
        reset_runtime_limits();

        Settings settings(_config.settingNs, true);
        settings.SetInt(_config.settingZeroPositionKey, _zero_pos);

        mclog::tagInfo(_tag, "id: {} set zero pos: {} to settings", _config.id, _zero_pos);
    }

    void resetZeroCalibration() override
    {
        _zero_pos = _config.defaultZeroPos;
        reset_runtime_limits();

        Settings settings(_config.settingNs, true);
        settings.SetInt(_config.settingZeroPositionKey, _zero_pos);

        mclog::tagInfo(_tag, "id: {} set zero pos: {} to settings", _config.id, _zero_pos);
    }

    void rotate(int velocity) override
    {
        velocity = uitk::clamp(velocity, -1000, 1000);

        if (!_config.enablePwmMode) {
            return;
        }

        int mapped_velocity = map_range(velocity, 0, 1000, 0, 1023);

        check_mode(Mode::PWM);
        _scs_bus.WritePWM(_config.id, mapped_velocity);
    }

private:
    enum class Mode { Position = 0, PWM = 1 };

    ServoConfig_t _config;
    Vector2i _runtime_raw_pos_limit;
    int _zero_pos           = 0;
    Mode _current_mode      = Mode::Position;
    bool _move_read_failing = false;  // logs bus read failure once, not every poll

    static constexpr uint32_t kStallFeedbackIntervalMs = 50;
    static constexpr int kStallMinTargetDeltaRaw       = 8;
    static constexpr int kStallMaxPositionDeltaRaw     = 1;
    static constexpr int kStallCurrentRiseThreshold    = 80;
    static constexpr int kStallLoadRiseThreshold       = 150;
    static constexpr int kStallCurrentAbsThreshold     = 350;
    static constexpr int kStallLoadAbsThreshold        = 650;
    static constexpr int kStallConfirmSamples          = 2;

    uint32_t _last_stall_check_tick = 0;
    int _last_stall_raw_pos         = 0;
    int _last_stall_current_abs     = 0;
    int _last_stall_load_abs        = 0;
    int _last_stall_direction       = 0;
    int _stall_confirm_count        = 0;
    bool _last_stall_feedback_valid = false;

    static int abs_int(int value)
    {
        return value < 0 ? -value : value;
    }

    bool is_raw_pos_valid(int raw_pos) const
    {
        return raw_pos >= _config.rawPosLimit.x && raw_pos <= _config.rawPosLimit.y;
    }

    int raw_pos_to_angle(int raw_pos) const
    {
        return (raw_pos - _zero_pos) * 5 * 10 / 16;
    }

    void reset_runtime_limits()
    {
        _runtime_raw_pos_limit = _config.rawPosLimit;
        set_angle_limit(_config.angleLimit);
        reset_stall_detection();
    }

    void reset_stall_detection()
    {
        _last_stall_feedback_valid = false;
        _last_stall_direction      = 0;
        _stall_confirm_count       = 0;
    }

    bool update_stall_protection(int target_raw_pos)
    {
        if (!_config.enableStallProtection) {
            return false;
        }

        const uint32_t now = GetHAL().millis();
        if (now - _last_stall_check_tick < kStallFeedbackIntervalMs) {
            return false;
        }
        _last_stall_check_tick = now;

        if (_scs_bus.FeedBack(_config.id) < 0) {
            reset_stall_detection();
            return false;
        }

        const int current_raw_pos = _scs_bus.ReadPos(-1);
        const int current_abs     = abs_int(_scs_bus.ReadCurrent(-1));
        const int load_abs        = abs_int(_scs_bus.ReadLoad(-1));

        if (!is_raw_pos_valid(current_raw_pos)) {
            reset_stall_detection();
            return false;
        }

        const int target_delta = target_raw_pos - current_raw_pos;
        if (abs_int(target_delta) < kStallMinTargetDeltaRaw) {
            reset_stall_detection();
            return false;
        }

        const int direction = target_delta > 0 ? 1 : -1;
        if (_last_stall_feedback_valid && direction == _last_stall_direction) {
            const int pos_delta = abs_int(current_raw_pos - _last_stall_raw_pos);
            const bool position_stuck = pos_delta <= kStallMaxPositionDeltaRaw;
            const bool current_spike  = current_abs >= kStallCurrentAbsThreshold ||
                                       current_abs - _last_stall_current_abs >= kStallCurrentRiseThreshold;
            const bool load_spike = load_abs >= kStallLoadAbsThreshold ||
                                    load_abs - _last_stall_load_abs >= kStallLoadRiseThreshold;

            if (position_stuck && (current_spike || load_spike)) {
                _stall_confirm_count++;
            } else if (pos_delta > kStallMaxPositionDeltaRaw) {
                _stall_confirm_count = 0; 
            }
        } else {
            _stall_confirm_count = 0;
        }

        _last_stall_raw_pos         = current_raw_pos;
        _last_stall_current_abs     = current_abs;
        _last_stall_load_abs        = load_abs;
        _last_stall_direction       = direction;
        _last_stall_feedback_valid = true;

        if (_stall_confirm_count < kStallConfirmSamples) {
            return false;
        }

        handle_stall(current_raw_pos, direction, current_abs, load_abs);
        return true;
    }

    void handle_stall(int raw_pos, int direction, int current_abs, int load_abs)
    {
        int angle = raw_pos_to_angle(raw_pos);
        angle     = uitk::clamp(angle, _config.angleLimit.x, _config.angleLimit.y);

        auto angle_limit = getAngleLimit();
        if (direction > 0) {
            if (raw_pos < _runtime_raw_pos_limit.y) {
                _runtime_raw_pos_limit.y = raw_pos;
            }
            if (angle < angle_limit.y) {
                angle_limit.y = angle;
            }
        } else {
            if (raw_pos > _runtime_raw_pos_limit.x) {
                _runtime_raw_pos_limit.x = raw_pos;
            }
            if (angle > angle_limit.x) {
                angle_limit.x = angle;
            }
        }
        set_angle_limit(angle_limit);
        stop_motion_at_angle(angle);
        reset_stall_detection();

        check_mode(Mode::Position);
        _scs_bus.WritePos(_config.id, raw_pos, 20, 0);

        mclog::tagWarn(_tag,
                       "id: {} stall detected, raw: {}, angle: {}, dir: {}, current: {}, load: {}, limit: [{}, {}]",
                       _config.id, raw_pos, angle, direction, current_abs, load_abs, angle_limit.x, angle_limit.y);
    }

    void check_mode(Mode targetMode)
    {
        if (targetMode == _current_mode) {
            return;
        }

        _scs_bus.SwitchMode(_config.id, static_cast<uint8_t>(targetMode));
        _current_mode = targetMode;
    }
};

void Hal::servo_init()
{
    mclog::tagInfo("HAL-Servo", "init");

    _scs_bus.begin(UART_NUM_1, 1000000, 6, 7);

    ServoConfig_t yaw_servo_config;
    yaw_servo_config.id                     = 1;
    yaw_servo_config.defaultZeroPos         = 460;
    yaw_servo_config.angleLimit             = Vector2i(-1280, 1280);
    yaw_servo_config.rawPosLimit            = Vector2i(0, 1000);
    yaw_servo_config.settingNs              = "servo";
    yaw_servo_config.settingZeroPositionKey = "zero_pos_1";
    yaw_servo_config.enablePwmMode          = true;

    ServoConfig_t pitch_servo_config;
    pitch_servo_config.id                     = 2;
    pitch_servo_config.defaultZeroPos         = 620;
    pitch_servo_config.angleLimit             = Vector2i(30, 870);
    pitch_servo_config.rawPosLimit            = Vector2i(0, 1000);
    pitch_servo_config.settingNs              = "servo";
    pitch_servo_config.settingZeroPositionKey = "zero_pos_2";
    pitch_servo_config.enableStallProtection  = true;

    auto yaw_servo   = std::make_unique<ScsServo>(yaw_servo_config);
    auto pitch_servo = std::make_unique<ScsServo>(pitch_servo_config);
    auto motion      = std::make_unique<Motion>(std::move(yaw_servo), std::move(pitch_servo));
    motion->init();

    GetStackChan().attachMotion(std::move(motion));
}
