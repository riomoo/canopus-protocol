// The host window: shows the two screens and turns the keyboard and mouse
// into 3DS input. One implementation per platform (window_win32.cpp); where
// there is none yet, Open returns false and the game runs without a window.
#pragma once

#include "input.h"

#include <string>

namespace rt {
namespace window {

// How the picture reaches the screen (command line: --no-vsync, --smooth,
// --output). Set before Open.
struct Options {
    bool vsync = true;    // wait for the monitor's refresh: no tearing
    bool smooth = false;  // bilinear scaling instead of sharp pixels
    // "" picks the best: OpenGL on Windows (GDI if it fails), SDL's
    // accelerated renderer on Linux. Or "gl", "gdi" (Windows), "software"
    // (Linux).
    std::string output;
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
