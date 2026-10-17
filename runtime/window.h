// The host window: shows the two screens and turns the keyboard and mouse
// into 3DS input. One implementation per platform (window_win32.cpp); where
// there is none yet, Open returns false and the game runs without a window.
#pragma once

#include "input.h"

namespace rt {
namespace window {

// Opens the window on a thread of its own. False if there is no window here.
bool Open(const char* title);
// Hands over the latest frame: top 400x240 and bottom 320x240, 0xAARRGGBB,
// row by row as seen. Either may be null (screen not set up yet).
void Show(const u32* top, const u32* bottom);
// Shows a line of status (speed) in the title bar.
void SetStatus(const char* text);
// The player's input as of now.
input::State Input();

}  // namespace window
}  // namespace rt
