/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "drivers/Si12T/Si12T.h"
#include "board/hal_bridge.h"
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const std::string_view _tag = "HAL-HeadTouch";

// 触摸状态
enum class TouchState { IDLE, TOUCHED, SWIPING };

// 配置参数
struct TouchConfig {
    // 空闲时中间通道常驻 1 count 的噪声，阈值必须高于该噪声地板。
    // 实测：真实触摸各通道 2~3 counts
    uint8_t touch_threshold = 2;
    int16_t swipe_threshold = 40;  // 使用百分比，范围-100到100
    // 位置是比值，总强度很低时 1 count 噪声就能让位置跳 50~100，
    // 远超 swipe_threshold。低于该总强度不信任位置
    uint16_t min_total_for_position = 4;
    // 滑动需要连续两帧确认，过滤单帧噪声尖峰
    uint8_t swipe_confirm_samples = 2;
};

// 触摸数据
struct TouchData {
    uint8_t intensity[3];
    uint32_t timestamp;

    // 计算位置（返回-100到100的整数）
    int16_t get_position() const
    {
        uint16_t total = intensity[0] + intensity[1] + intensity[2];
        if (total == 0) return 0;

        int32_t weighted = intensity[0] * (-100) + intensity[1] * 0 + intensity[2] * 100;
        return static_cast<int16_t>(weighted / total);
    }

    uint16_t get_total_intensity() const
    {
        return intensity[0] + intensity[1] + intensity[2];
    }

    uint8_t get_max_intensity() const
    {
        uint8_t max_val = intensity[0];
        if (intensity[1] > max_val) max_val = intensity[1];
        if (intensity[2] > max_val) max_val = intensity[2];
        return max_val;
    }

    bool is_touched(uint8_t threshold) const
    {
        return get_max_intensity() >= threshold;
    }
};

// 手势识别器类
class GestureRecognizer {
public:
    GestureRecognizer() : current_state(TouchState::IDLE), initial_position(0)
    {
    }

    // 更新状态机，返回识别到的手势
    HeadPetGesture update(const TouchData& data)
    {
        HeadPetGesture gesture = HeadPetGesture::None;
        const bool touched     = data.is_touched(config.touch_threshold);
        // 位置只在信号足够强时可信，否则保持上一次的有效值
        const bool pos_valid = data.get_total_intensity() >= config.min_total_for_position;

        switch (current_state) {
            case TouchState::IDLE:
                if (touched) {
                    current_state = TouchState::TOUCHED;
                    // 信号太弱就先不锁定起点，等到可信的一帧再记录
                    has_initial_position = pos_valid;
                    initial_position     = pos_valid ? data.get_position() : 0;
                    pending_direction    = 0;
                    pending_count        = 0;
                    gesture              = HeadPetGesture::Press;
                }
                break;

            case TouchState::TOUCHED:
                if (!touched) {
                    current_state = TouchState::IDLE;
                    gesture       = HeadPetGesture::Release;
                    break;
                }
                if (!pos_valid) {
                    // 弱信号帧不参与判定，也不累积确认
                    pending_direction = 0;
                    pending_count     = 0;
                    break;
                }
                if (!has_initial_position) {
                    has_initial_position = true;
                    initial_position     = data.get_position();
                    break;
                }
                {
                    const int16_t delta = data.get_position() - initial_position;
                    const int direction = delta > config.swipe_threshold    ? 1
                                          : delta < -config.swipe_threshold ? -1
                                                                            : 0;
                    if (direction == 0) {
                        pending_direction = 0;
                        pending_count     = 0;
                        break;
                    }
                    // 同方向连续多帧才认可，单帧噪声尖峰会被清零
                    if (direction == pending_direction) {
                        pending_count++;
                    } else {
                        pending_direction = direction;
                        pending_count     = 1;
                    }
                    if (pending_count >= config.swipe_confirm_samples) {
                        current_state     = TouchState::SWIPING;
                        gesture = direction > 0 ? HeadPetGesture::SwipeForward : HeadPetGesture::SwipeBackward;
                        pending_direction = 0;
                        pending_count     = 0;
                    }
                }
                break;

            case TouchState::SWIPING:
                if (!touched) {
                    current_state = TouchState::IDLE;
                    gesture       = HeadPetGesture::Release;
                }
                break;
        }

        return gesture;
    }

    void set_config(const TouchConfig& cfg)
    {
        config = cfg;
    }

private:
    TouchConfig config;
    TouchState current_state;
    int16_t initial_position;
    bool has_initial_position = false;
    int pending_direction     = 0;
    uint8_t pending_count     = 0;
};

static void _head_touch_update_task(void* param)
{
    mclog::tagInfo(_tag, "start update task");

    si12t_handle_t si12t = (si12t_handle_t)param;
    uint8_t touch_result = 0;
    TouchData data;

    GestureRecognizer recognizer;
    HeadPetGesture gesture;

    vTaskDelay(pdMS_TO_TICKS(200));

    while (1) {
        // Read data
        si12t_read_touch_result(si12t, &touch_result);
        si12t_parse_touch_result_to(touch_result, data.intensity);
        data.timestamp = xTaskGetTickCount();

        // Update and fire event
        gesture = recognizer.update(data);
        if (gesture != HeadPetGesture::None) {
            GetHAL().onHeadPetGesture.emit(gesture);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void Hal::head_touch_init()
{
    mclog::tagInfo(_tag, "init");

    auto i2c_bus = hal_bridge::board_get_i2c_bus();

    si12t_config_t si12t_cfg = {
        .i2c_bus  = i2c_bus,
        .dev_addr = SI12T_GND_ADDRESS,
    };
    static si12t_handle_t si12t;
    si12t_init(&si12t_cfg, &si12t);
    si12t_setup(si12t, SI12T_TYPE_LOW, SI12T_SENSITIVITY_LEVEL_3);

    // xTaskCreateWithCaps(_head_touch_update_task, "headtouch", 1024 * 6, si12t, 2, NULL, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(_head_touch_update_task, "headtouch", 1024 * 6, si12t, 2, NULL, 1,
                                    MALLOC_CAP_SPIRAM);
}
