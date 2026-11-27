// The host window: shows the two screens and turns the keyboard, mouse and
// controllers into 3DS input (window_raylib.cpp). If no window can be opened,
// Open returns false and the game runs without one.
#pragma once

#include "input.h"

#include <string>

namespace rt {
namespace window {

// How the picture reaches the screen and how controllers map (command
// line: --no-vsync, --smooth, --nintendo-buttons). Set before Open.
struct Options {
    bool vsync = true;             // wait for the monitor's refresh: no tearing
    bool smooth = false;           // bilinear scaling instead of sharp pixels
    bool nintendoButtons = false;  // A/B/X/Y by a Nintendo pad's labels (A on the right)
};
inline Options g_options;

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
