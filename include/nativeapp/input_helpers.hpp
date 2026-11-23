// nativeapp/input_helpers.hpp — Mouse e gamepad sobre eventos Android.

#pragma once

#include <android/input.h>
#include <android/keycodes.h>

#include <array>
#include <cstdint>
#include <unordered_set>

namespace nativeapp {

enum class MouseButton { Left, Right, Middle, Back, Forward };

struct Stick { float x = 0, y = 0; };

class InputHelpers {
public:
    static InputHelpers& instance() {
        static InputHelpers singleton;
        return singleton;
    }

    /// Repasse todo evento recebido no App::on_input (key ou motion).
    void on_event(AInputEvent* event) {
        if (!event) return;
        const int32_t type = AInputEvent_getType(event);
        if (type == AINPUT_EVENT_TYPE_KEY) {
            const int32_t code = AKeyEvent_getKeyCode(event);
            const int32_t action = AKeyEvent_getAction(event);
            const bool down = action == AKEY_EVENT_ACTION_DOWN ||
                              action == AKEY_EVENT_ACTION_MULTIPLE;
            if (down) keys_.insert(code);
            else if (action == AKEY_EVENT_ACTION_UP) keys_.erase(code);
            last_key_ = code;
            // Botões de gamepad também chegam como KEY events (DPAD, A/B…).
            mark_gamepad_source(AInputEvent_getSource(event));
            return;
        }
        if (type != AINPUT_EVENT_TYPE_MOTION) return;

        const int32_t source = AInputEvent_getSource(event);
        const bool is_mouse = (source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE;
        const bool is_gamepad = mark_gamepad_source(source);
        // Eventos de hover/exit podem não ter nenhum ponteiro.
        const bool has_pointer = AMotionEvent_getPointerCount(event) > 0;

        const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        const int32_t buttons = AMotionEvent_getButtonState(event);

        if (is_mouse) {
            mouse_buttons_ = buttons;
            if (has_pointer) {
                mouse_x_ = AMotionEvent_getX(event, 0);
                mouse_y_ = AMotionEvent_getY(event, 0);
            }
            if (action == AMOTION_EVENT_ACTION_SCROLL) {
                // Soma os deltas até consume_scroll() (vários eventos/frame).
                scroll_x_ += AMotionEvent_getXOffset(event);
                scroll_y_ += AMotionEvent_getYOffset(event);
            }
        }

        if (is_gamepad && has_pointer) {
            // Analog sticks (AXIS_X/Y = esquerdo, Z/RZ = direito)
            stick_left_  = {AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0),
                            AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0)};
            stick_right_ = {AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0),
                            AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0)};
            triggers_ = {AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0),
                         AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0)};
        }
    }

    // ----- Teclado / botões de gamepad -------------------------------------
    bool is_down(int32_t keycode) const { return keys_.count(keycode) > 0; }
    int32_t last_key() const { return last_key_; }

    // ----- Gamepad ----------------------------------------------------------
    bool gamepad_connected() const { return gamepad_connected_; }
    Stick left_stick() const { return stick_left_; }
    Stick right_stick() const { return stick_right_; }
    float left_trigger() const { return triggers_[0]; }
    float right_trigger() const { return triggers_[1]; }

    bool button_a() const { return is_down(AKEYCODE_BUTTON_A); }
    bool button_b() const { return is_down(AKEYCODE_BUTTON_B); }
    bool button_x() const { return is_down(AKEYCODE_BUTTON_X); }
    bool button_y() const { return is_down(AKEYCODE_BUTTON_Y); }
    bool button_start() const { return is_down(AKEYCODE_BUTTON_START); }

    // ----- Mouse ------------------------------------------------------------
    float mouse_x() const { return mouse_x_; }
    float mouse_y() const { return mouse_y_; }
    bool mouse_down(MouseButton button) const {
        const int32_t bit = button == MouseButton::Left ? AMOTION_EVENT_BUTTON_PRIMARY :
                            button == MouseButton::Right ? AMOTION_EVENT_BUTTON_SECONDARY :
                            button == MouseButton::Middle ? AMOTION_EVENT_BUTTON_TERTIARY :
                            button == MouseButton::Back ? AMOTION_EVENT_BUTTON_BACK
                                                        : AMOTION_EVENT_BUTTON_FORWARD;
        return (mouse_buttons_ & bit) != 0;
    }
    float scroll_x() const { return scroll_x_; }
    float scroll_y() const { return scroll_y_; }

    void reset() {
        keys_.clear();
        mouse_x_ = mouse_y_ = 0;
        mouse_buttons_ = 0;
        scroll_x_ = scroll_y_ = 0;
        last_key_ = 0;
        stick_left_ = stick_right_ = {};
        triggers_ = {};
        gamepad_connected_ = false;
    }

    /// Consome o delta de scroll acumulado (zera após a leitura).
    void consume_scroll(float& x, float& y) {
        x = scroll_x_;
        y = scroll_y_;
        scroll_x_ = scroll_y_ = 0;
    }

private:
    InputHelpers() = default;

    bool mark_gamepad_source(int32_t source) {
        const bool is_gamepad =
            (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
            (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
        if (is_gamepad) gamepad_connected_ = true;
        return is_gamepad;
    }

    std::unordered_set<int32_t> keys_;
    int32_t last_key_ = 0;
    float mouse_x_ = 0, mouse_y_ = 0;
    int32_t mouse_buttons_ = 0;
    float scroll_x_ = 0, scroll_y_ = 0;
    Stick stick_left_, stick_right_;
    std::array<float, 2> triggers_{};
    bool gamepad_connected_ = false;
};

}  // namespace nativeapp
