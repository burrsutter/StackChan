/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stackchan/modifiable.h>
#include <stackchan/avatar/decorators/decorators.h>
#include <stackchan/utils/random.h>
#include <smooth_ui_toolkit.hpp>
#include <hal/hal.h>
#include <atomic>
#include <cstdint>
#include <memory>

namespace companion {

/**
 * @brief Directional variant of the factory HeadPetModifier: the head leans
 * into the stroke. A front-to-back stroke raises the head into your hand,
 * a back-to-front stroke lowers it. Repeated strokes accumulate; a few
 * seconds after release the head settles back to where it started.
 * Visual feedback (happy face, heart/shy decorators) matches the factory
 * behavior.
 */
class LeanPetModifier : public stackchan::Modifier {
public:
    LeanPetModifier(uint32_t restoreDelayMs = 3000) : _restore_delay_ms(restoreDelayMs)
    {
        _signal_connection = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture gesture) {
            if (gesture == HeadPetGesture::SwipeForward) {
                _pending_lean += 1;
            } else if (gesture == HeadPetGesture::SwipeBackward) {
                _pending_lean -= 1;
            } else if (gesture == HeadPetGesture::Release) {
                _event_release = true;
            }
        });
    }

    ~LeanPetModifier()
    {
        GetHAL().onHeadPetGesture.disconnect(_signal_connection);
    }

    void _update(stackchan::Modifiable& stackchan) override
    {
        uint32_t now = GetHAL().millis();

        int lean = _pending_lean.exchange(0);
        if (lean != 0) {
            handle_stroke(stackchan, lean);
            _is_waiting_restore = false;
        }

        if (_event_release) {
            _event_release = false;
            if (_in_happy_state) {
                _is_waiting_restore = true;
                _restore_tick       = now + _restore_delay_ms;
            }
        }

        if (_is_waiting_restore && now >= _restore_tick) {
            _is_waiting_restore = false;
            restore_original_state(stackchan);
        }
    }

private:
    // Pitch units (tenths of a degree) the head moves per stroke; the sign
    // constant flips the direction if it feels backwards on real hardware
    // (which physical end of the touch strip is "forward" is undocumented)
    static constexpr int PET_PITCH_STEP     = 120;
    static constexpr int PET_DIRECTION_SIGN = -1;

    void handle_stroke(stackchan::Modifiable& stackchan, int lean)
    {
        auto& avatar = stackchan.avatar();

        if (!_in_happy_state) {
            _in_happy_state = true;
            _prev_emotion   = avatar.getEmotion();
            auto angles     = stackchan.motion().getCurrentAngles();
            _prev_yaw       = angles.x;
            _prev_pitch     = angles.y;
        }

        avatar.setEmotion(stackchan::avatar::Emotion::Happy);

        int duration = stackchan::Random::getInstance().getInt(1500, 2500);
        avatar.removeDecorator(_heart_decorator_id);
        avatar.removeDecorator(_shy_decorator_id);
        _heart_decorator_id = avatar.addDecorator(
            std::make_unique<stackchan::avatar::HeartDecorator>(lv_screen_active(), duration, 500));
        _shy_decorator_id =
            avatar.addDecorator(std::make_unique<stackchan::avatar::ShyDecorator>(lv_screen_active(), duration));

        auto& motion = stackchan.motion();
        if (motion.isModifyLocked()) {
            return;
        }

        auto current           = motion.getCurrentAngles();
        const int target_pitch = uitk::clamp(current.y + lean * PET_DIRECTION_SIGN * PET_PITCH_STEP, 30, 600);
        motion.moveWithSpeed(current.x, target_pitch, 350);
    }

    void restore_original_state(stackchan::Modifiable& stackchan)
    {
        if (!_in_happy_state) {
            return;
        }

        stackchan.avatar().setEmotion(_prev_emotion);
        stackchan.motion().moveWithSpeed(_prev_yaw, _prev_pitch, 200);

        _in_happy_state = false;
    }

    int _signal_connection;
    std::atomic<int> _pending_lean{0};
    volatile bool _event_release = false;

    bool _in_happy_state     = false;
    bool _is_waiting_restore = false;
    uint32_t _restore_tick   = 0;
    uint32_t _restore_delay_ms;

    stackchan::avatar::Emotion _prev_emotion = stackchan::avatar::Emotion::Neutral;
    int32_t _prev_yaw                        = 0;
    int32_t _prev_pitch                      = 0;

    int _heart_decorator_id = -1;
    int _shy_decorator_id   = -1;
};

}  // namespace companion
