// Buttons and the touch screen: the HID shared memory the game reads and the
// events that tell it new input arrived.
#pragma once

#include "kernel.h"

namespace rt {
namespace input {

// 3DS buttons (HID bits).
enum Button : u32 {
    A = 1 << 0, B = 1 << 1, SELECT = 1 << 2, START = 1 << 3, RIGHT = 1 << 4, LEFT = 1 << 5, UP = 1 << 6, DOWN = 1 << 7,
    R = 1 << 8, L = 1 << 9, X = 1 << 10, Y = 1 << 11,
};

struct State {
    u32 buttons = 0;
    bool touch = false;
    u16 touchX = 0, touchY = 0;  // bottom screen pixels
    s16 circleX = 0, circleY = 0;
};

std::shared_ptr<kernel::SharedMemory> SharedMemory();
std::shared_ptr<kernel::Event> Event(int i);  // PAD0, PAD1, accel, gyro, debug pad
// Called by the platform layer each frame with the current state.
void Update(const State& s);

}  // namespace input
}  // namespace rt
