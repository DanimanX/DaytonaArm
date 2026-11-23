// Handheld SDL2 input and arcade steering controller for Anbernic RG34XX and
// compatible PortMaster devices.
//
// Highlights:
//   * Built-in Anbernic RG34XX mapping + gamecontrollerdb.txt loading.
//   * Arcade force-feedback spring steering model for D-pad (docs/gamepad-steering.md)
//     enabled by default so stick-less handhelds (RG34XX) drive smoothly at 300 km/h.
//   * Saturn/Dreamcast-style step steering on menus (circuit/transmission select).
//   * Full analog stick & trigger support for devices with analog sticks.
#pragma once

#include "runtime/drive_board.h"
#include "runtime/m2_board.h"

#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace arm64 {

enum Button : uint32_t {
    BtnA = 1u << 0,      // SDL South (A on Xbox/PortMaster, B on Nintendo)
    BtnB = 1u << 1,      // SDL East
    BtnX = 1u << 2,      // SDL West
    BtnY = 1u << 3,      // SDL North
    Up = 1u << 4,
    Down = 1u << 5,
    Left = 1u << 6,
    Right = 1u << 7,
    L1 = 1u << 8,
    R1 = 1u << 9,
    L2 = 1u << 10,
    R2 = 1u << 11,
    Start = 1u << 12,
    Select = 1u << 13,
    Guide = 1u << 14,    // Menu / Function button
};

struct PadState {
    uint32_t buttons = 0;
    float lx = 0.0f; // -1..+1
    float lt = 0.0f; // 0..1
    float rt = 0.0f; // 0..1
};

enum Action : int {
    Accelerate,
    Brake,
    SteerLeft,
    SteerRight,
    ShiftUp,
    ShiftDown,
    StartButton,
    Coin,
    View1,
    View2,
    View3,
    View4,
    ActionCount
};

inline constexpr uint32_t kBindableMask = 0x3fffu; // BtnA..Select

inline const char *action_name(int a) {
    static const char *names[ActionCount] = {
        "ACCELERATE", "BRAKE", "STEER LEFT", "STEER RIGHT",
        "SHIFT UP", "SHIFT DOWN", "START", "COIN",
        "VIEW 1", "VIEW 2", "VIEW 3", "VIEW 4"
    };
    return (a >= 0 && a < ActionCount) ? names[a] : "?";
}

inline bool single_button(uint32_t b) {
    return b && !(b & ~kBindableMask) && !(b & (b - 1));
}

inline const char *button_name(uint32_t b) {
    static const char *names[] = {
        "A (SOUTH)", "B (EAST)", "X (WEST)", "Y (NORTH)",
        "D-UP", "D-DOWN", "D-LEFT", "D-RIGHT",
        "L1", "R1", "L2", "R2", "START", "SELECT"
    };
    if (!single_button(b)) return "NONE";
    int bit = 0;
    while (!(b & (1u << bit))) ++bit;
    return names[bit];
}

struct Bindings {
    // Ergonomic RG34XX defaults (GBA horizontal layout, no analog sticks):
    //   R1 = Accelerate, L1 = Brake, D-Pad L/R = Steer, D-Pad U/D = Shift Up/Down,
    //   Start = Start, Select = Coin, A/B/X/Y = Views 1-4.
    uint32_t button[ActionCount] = {
        R1, L1, Left, Right, Up, Down, Start, Select, BtnA, BtnB, BtnX, BtnY
    };

    void bind(int action, uint32_t value) {
        if (action < 0 || action >= ActionCount || !single_button(value)) return;
        for (uint32_t &other : button)
            if (other == value) other = button[action];
        button[action] = value;
    }

    void cycle(int action, int dir) {
        if (action < 0 || action >= ActionCount) return;
        int bit = 0;
        while (bit < 13 && !(button[action] & (1u << bit))) ++bit;
        bind(action, 1u << ((bit + (dir < 0 ? 13 : 1)) % 14));
    }

    void sanitize() {
        uint32_t seen = 0;
        for (uint32_t v : button) {
            if (!single_button(v) || (seen & v)) { *this = Bindings{}; return; }
            seen |= v;
        }
    }
};

inline bool menu_chord(uint32_t buttons) {
    return ((buttons & (Start | Select)) == (Start | Select)) || ((buttons & Guide) != 0);
}

class PadManager {
public:
    ~PadManager() { close(); }

    void init(const std::string &data_dir) {
        // Built-in Anbernic RG34XX / H700 controller mapping fallback (from KillerArm).
        SDL_GameControllerAddMapping(
            "19000000010000000100000000010000,Anbernic RG34XX Controller,platform:Linux,"
            "b:b3,a:b4,dpdown:h0.4,leftx:a0,lefty:a1,lefttrigger:b12,dpleft:h0.8,"
            "rightshoulder:b8,leftshoulder:b7,righttrigger:b13,dpright:h0.2,"
            "back:b9,start:b10,dpup:h0.1,y:b6,x:b5,guide:b11,");
        const std::string db = data_dir + "/gamecontrollerdb.txt";
        SDL_GameControllerAddMappingsFromFile(db.c_str());
        for (int i = 0; i < SDL_NumJoysticks(); ++i) open_index(i);
    }

    void handle_event(const SDL_Event &e) {
        if (e.type == SDL_CONTROLLERDEVICEADDED) open_index(e.cdevice.which);
        else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (ctrl_ && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(ctrl_)) == e.cdevice.which) {
                SDL_GameControllerClose(ctrl_);
                ctrl_ = nullptr;
            }
        } else if (e.type == SDL_JOYDEVICEADDED && !ctrl_ && !joy_) {
            open_index(e.jdevice.which);
        } else if (e.type == SDL_JOYDEVICEREMOVED) {
            if (joy_ && SDL_JoystickInstanceID(joy_) == e.jdevice.which) {
                SDL_JoystickClose(joy_);
                joy_ = nullptr;
            }
        }
    }

    PadState poll(const uint8_t *keys) const {
        PadState p{};
        if (ctrl_) {
            auto b = [&](SDL_GameControllerButton gb, uint32_t mask) {
                if (SDL_GameControllerGetButton(ctrl_, gb)) p.buttons |= mask;
            };
            b(SDL_CONTROLLER_BUTTON_A, BtnA);
            b(SDL_CONTROLLER_BUTTON_B, BtnB);
            b(SDL_CONTROLLER_BUTTON_X, BtnX);
            b(SDL_CONTROLLER_BUTTON_Y, BtnY);
            b(SDL_CONTROLLER_BUTTON_DPAD_UP, Up);
            b(SDL_CONTROLLER_BUTTON_DPAD_DOWN, Down);
            b(SDL_CONTROLLER_BUTTON_DPAD_LEFT, Left);
            b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, Right);
            b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, L1);
            b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, R1);
            b(SDL_CONTROLLER_BUTTON_START, Start);
            b(SDL_CONTROLLER_BUTTON_BACK, Select);
            b(SDL_CONTROLLER_BUTTON_GUIDE, Guide);
            p.lx = std::clamp(float(SDL_GameControllerGetAxis(ctrl_, SDL_CONTROLLER_AXIS_LEFTX)) / 32767.0f, -1.0f, 1.0f);
            p.lt = std::clamp(float(SDL_GameControllerGetAxis(ctrl_, SDL_CONTROLLER_AXIS_TRIGGERLEFT)) / 32767.0f, 0.0f, 1.0f);
            p.rt = std::clamp(float(SDL_GameControllerGetAxis(ctrl_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) / 32767.0f, 0.0f, 1.0f);
            if (p.lt > 0.4f) p.buttons |= L2;
            if (p.rt > 0.4f) p.buttons |= R2;
        } else if (joy_) {
            const uint8_t hat = SDL_JoystickNumHats(joy_) > 0 ? SDL_JoystickGetHat(joy_, 0) : 0;
            if (hat & SDL_HAT_UP) p.buttons |= Up;
            if (hat & SDL_HAT_DOWN) p.buttons |= Down;
            if (hat & SDL_HAT_LEFT) p.buttons |= Left;
            if (hat & SDL_HAT_RIGHT) p.buttons |= Right;
            if (SDL_JoystickNumAxes(joy_) >= 1)
                p.lx = std::clamp(float(SDL_JoystickGetAxis(joy_, 0)) / 32767.0f, -1.0f, 1.0f);
            auto jb = [&](int idx, uint32_t mask) {
                if (idx < SDL_JoystickNumButtons(joy_) && SDL_JoystickGetButton(joy_, idx)) p.buttons |= mask;
            };
            jb(0, BtnA); jb(1, BtnB); jb(2, BtnX); jb(3, BtnY);
            jb(4, L1); jb(5, R1); jb(6, Select); jb(7, Start); jb(8, Guide);
        }

        // Keyboard fallback (handy for desktop testing and USB keyboards).
        if (keys) {
            if (keys[SDL_SCANCODE_LEFT]) p.buttons |= Left;
            if (keys[SDL_SCANCODE_RIGHT]) p.buttons |= Right;
            if (keys[SDL_SCANCODE_UP]) p.buttons |= R1;
            if (keys[SDL_SCANCODE_DOWN]) p.buttons |= L1;
            if (keys[SDL_SCANCODE_W]) p.buttons |= Up;
            if (keys[SDL_SCANCODE_Q]) p.buttons |= Down;
            if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_Z]) p.buttons |= BtnA;
            if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_X]) p.buttons |= BtnB;
            if (keys[SDL_SCANCODE_D]) p.buttons |= BtnX;
            if (keys[SDL_SCANCODE_F]) p.buttons |= BtnY;
            if (keys[SDL_SCANCODE_5] || keys[SDL_SCANCODE_BACKSPACE]) p.buttons |= Select;
            if (keys[SDL_SCANCODE_RETURN]) p.buttons |= Start;
            if (keys[SDL_SCANCODE_ESCAPE]) p.buttons |= Guide;
        }
        return p;
    }

    void close() {
        if (ctrl_) { SDL_GameControllerClose(ctrl_); ctrl_ = nullptr; }
        if (joy_) { SDL_JoystickClose(joy_); joy_ = nullptr; }
    }

private:
    void open_index(int index) {
        if (ctrl_) return;
        if (SDL_IsGameController(index)) {
            ctrl_ = SDL_GameControllerOpen(index);
            if (ctrl_ && joy_) { SDL_JoystickClose(joy_); joy_ = nullptr; }
        } else if (!joy_) {
            joy_ = SDL_JoystickOpen(index);
        }
    }

    SDL_GameController *ctrl_ = nullptr;
    SDL_Joystick *joy_ = nullptr;
};

class Controls {
public:
    // Feed force-feedback drive board spring and race state from M2Board each frame.
    void update_board_state(const rt::DriveBoard &db, bool in_menu_screen, float car_speed_norm) {
        const float c1 = db.level(rt::DriveBoard::Effect::Centre);
        const float c2 = db.level(rt::DriveBoard::Effect::CentreDeadZone);
        spring_ = std::max(c1, c2);
        in_menu_ = in_menu_screen;
        speed_ = car_speed_norm;
    }

    rt::Inputs sample(const PadState &pad) {
        rt::Inputs in;
        const auto &b = bindings_.button;
        const uint32_t previous = held_;
        const uint32_t pressed = pad.buttons & ~held_;
        if (pressed & b[Coin]) coin_used_ = false;
        held_ = pad.buttons;

        if (menu_chord(pad.buttons)) {
            coin_used_ = true;
            set_gear(in);
            return in;
        }
        if (pad.buttons & Select) {
            const bool test = (pad.buttons & BtnY) != 0;
            const bool service = (pad.buttons & BtnX) != 0;
            if (test || service) {
                coin_used_ = true;
                if (test) in.in0 &= uint8_t(~0x04);
                if (service) in.in0 &= uint8_t(~0x08);
                set_gear(in);
                return in;
            }
        }

        // Steering: analog stick (if present and genuinely analog) or D-pad with
        // the arcade centring-spring model (docs/gamepad-steering.md).
        float raw_stick = 0.0f;
        const float ax = std::fabs(pad.lx);
        if (ax > deadzone_) raw_stick = std::copysign((ax - deadzone_) / (1.0f - deadzone_), pad.lx);
        if (steer_invert_) raw_stick = -raw_stick;
        if (stick_invert_) raw_stick = -raw_stick; // analog-only correction; pairs with steer_invert_

        const float d_right = (pad.buttons & b[SteerRight]) ? 1.0f : 0.0f;
        const float d_left = (pad.buttons & b[SteerLeft]) ? 1.0f : 0.0f;
        const float push = steer_invert_ ? (d_left - d_right) : (d_right - d_left);

        // Distinguish true analog sticks from digital-only axes reported on some handhelds.
        if (push == 0.0f && ax > 0.03f && ax < 0.97f) {
            if (++run_frames_ == 2) ++mid_frames_;
        } else {
            run_frames_ = 0;
        }
        if (mid_frames_ >= 3) seen_analog_ = true;

        const bool use_menu_step = in_menu_ && gamepad_menu_;
        if (!use_menu_step) {
            menu_sel_ = -1;
            menu_dir_ = 0;
        }
        if (was_menu_ && !use_menu_step) steer_ = 0.0f;
        was_menu_ = use_menu_step;

        if (use_menu_step) {
            const float target = push != 0.0f ? push : raw_stick;
            const int dir = target > 0.5f ? 1 : target < -0.5f ? -1 : 0;
            if (dir != 0 && dir != menu_dir_) menu_sel_ = std::clamp(menu_sel_ + dir, -1, 1);
            menu_dir_ = dir;
            steer_ = float(menu_sel_);
        } else if (!gamepad_fix_) {
            const float target = push != 0.0f ? push : raw_stick;
            if (push == 0.0f && seen_analog_) {
                float s = target;
                if (steer_curve_ == 1) s *= std::fabs(s);
                else if (steer_curve_ == 2) s = s * s * s;
                steer_ = s;
            } else {
                steer_ += std::clamp(target - steer_, -0.14f, 0.14f);
            }
        } else if (push == 0.0f && seen_analog_) {
            constexpr float kSpeedCut = 0.45f, kSmooth = 0.18f;
            const float v = std::clamp(speed_, 0.0f, 1.2f);
            const float speed_fac = 1.0f / (1.0f + kSpeedCut * v * v);
            const float a = std::fabs(raw_stick);
            const float u = std::clamp((a - 0.85f) / 0.15f, 0.0f, 1.0f);
            const float curved = std::copysign(
                0.72f * std::pow(a, 1.6f) + (1.0f - 0.72f) * u * u * (3.0f - 2.0f * u), raw_stick);
            steer_ += (curved * std::sqrt(speed_fac) - steer_) * kSmooth * speed_fac;
        } else {
            // Arcade centring-spring model for RG34XX D-pad!
            constexpr float k0 = 7.0f / 31.0f;
            constexpr int kReleaseHold = 3;
            const float k = std::max(spring_, k0);
            if (push != 0.0f) { dpad_hold_ = push; dpad_release_frames_ = 0; }
            else if (dpad_release_frames_ < kReleaseHold) ++dpad_release_frames_;
            const float active_push = (push != 0.0f || dpad_release_frames_ < kReleaseHold) ? dpad_hold_ : raw_stick;
            const float target = std::clamp(active_push, -1.0f, 1.0f) *
                                 std::clamp(wheel_weight_ * std::sqrt(k0 / k), 0.0f, 1.0f);
            const bool turning_out = target != 0.0f && target * steer_ >= 0.0f && std::fabs(target) > std::fabs(steer_);
            const float rate = turning_out ? wheel_turnin_ * std::sqrt(k0 / k) : wheel_return_ * std::sqrt(k / k0);
            steer_ += std::clamp(target - steer_, -rate, rate);
        }

        const float s = std::clamp(steer_, -1.0f, 1.0f);
        if (use_menu_step) {
            in.steer = uint8_t(std::lround(s >= 0 ? 0x8e + s * (0xe0 - 0x8e) : 0x8e + s * (0x8e - 0x20)));
        } else {
            in.steer = uint8_t(std::lround(0x80 + s * 0x60));
        }

        const float accel = (pad.buttons & b[Accelerate]) ? 1.0f : pad.rt;
        const float brake = (pad.buttons & b[Brake]) ? 1.0f : pad.lt;
        in.accel = uint8_t(std::lround(0x20 + std::clamp(accel, 0.0f, 1.0f) * 0xc0));
        in.brake = uint8_t(std::lround(0x20 + std::clamp(brake, 0.0f, 1.0f) * 0xc0));

        const int shift = int(bool(pressed & b[ShiftUp])) - int(bool(pressed & b[ShiftDown]));
        gear_ = std::clamp(gear_ + shift, 1, 4);
        set_gear(in);

        if ((previous & b[Coin]) && !(pad.buttons & b[Coin]) && !coin_used_) in.in0 &= uint8_t(~0x01);
        if (pad.buttons & b[StartButton]) in.in0 &= uint8_t(~0x10);
        if (pad.buttons & b[View1]) in.in0 &= uint8_t(~0x20);
        if (pad.buttons & b[View2]) in.in0 &= uint8_t(~0x40);
        if (pad.buttons & b[View3]) in.in0 &= uint8_t(~0x80);
        if (pad.buttons & b[View4]) in.in1 &= uint8_t(~0x01);
        return in;
    }

    void latch(uint32_t buttons) {
        held_ = buttons;
        if (buttons & bindings_.button[Coin]) coin_used_ = true;
    }

    void set_deadzone(float v) { deadzone_ = std::clamp(v, 0.0f, 0.4f); }
    void set_steer_curve(int v) { steer_curve_ = std::clamp(v, 0, 2); }
    void set_steer_invert(bool v) { steer_invert_ = v; }
    // Analog stick only. Some handhelds wire the left stick the opposite way to
    // SDL's convention, and inverting the shared flag to fix that would also
    // invert the D-pad.
    bool stick_invert() const { return stick_invert_; }
    void set_stick_invert(bool v) { stick_invert_ = v; }
    void set_gamepad_fix(bool v) { gamepad_fix_ = v; }
    void set_gamepad_menu(bool v) { gamepad_menu_ = v; }
    void set_bindings(const Bindings &b) { bindings_ = b; bindings_.sanitize(); }
    const Bindings &bindings() const { return bindings_; }

private:
    void set_gear(rt::Inputs &in) const {
        constexpr uint8_t codes[] = {0, 2, 1, 6, 5};
        in.in1 = uint8_t((in.in1 & ~0x70) | (codes[gear_] << 4));
    }

    Bindings bindings_;
    uint32_t held_ = 0;
    int gear_ = 1, steer_curve_ = 0;
    float deadzone_ = 0.12f;
    bool steer_invert_ = false, stick_invert_ = false, coin_used_ = false;
    bool gamepad_fix_ = true;  // ON by default for RG34XX D-pad playability
    bool gamepad_menu_ = true; // ON by default for clean menu selection
    float wheel_weight_ = 1.0f, wheel_turnin_ = 0.09f, wheel_return_ = 0.14f;
    float steer_ = 0.0f, spring_ = 0.0f, speed_ = 0.0f, dpad_hold_ = 0.0f;
    int dpad_release_frames_ = 99, run_frames_ = 0, mid_frames_ = 0;
    bool seen_analog_ = false, in_menu_ = false, was_menu_ = false;
    int menu_sel_ = -1, menu_dir_ = 0;
};

class FrameClock {
public:
    explicit FrameClock(double hz, int max_steps = 1)
        : step_(1.0 / hz), max_steps_(std::clamp(max_steps, 1, 4)) {}
    void set_hz(double hz) { step_ = 1.0 / std::clamp(hz, 30.0, 120.0); }
    int advance(double seconds) {
        if (!std::isfinite(seconds) || seconds < 0.0) { reset(); return 0; }
        pending_ = std::min(pending_ + seconds, step_ * 4.0);
        int count = 0;
        while (pending_ >= step_ && count < max_steps_) { pending_ -= step_; ++count; }
        if (pending_ >= step_) pending_ = std::fmod(pending_, step_);
        return count;
    }
    void reset() { pending_ = 0.0; }
private:
    double step_, pending_ = 0.0;
    int max_steps_;
};

} // namespace arm64
