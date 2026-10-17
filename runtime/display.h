// The two screens: what GSP shows (framebuffer address, stride, format) and
// the host side that presents them and reads the player's input.
#pragma once

#include "input.h"
#include "rt.h"

namespace rt {
namespace display {

constexpr int TOP = 0, BOTTOM = 1;

void SetFramebuffer(int screen, u32 addr, u32 stride, u32 format);
// Converts a screen to 0xAARRGGBB pixels, row by row as seen (top: 400x240,
// bottom: 320x240). False if the game has not set that screen yet.
bool Capture(int screen, u32* out, int* w, int* h);
// Opens the host window if this platform has one (else runs headless).
void OpenWindow(const char* title);
// Called at every VBlank.
void Present();
input::State PollInput();

}  // namespace display
}  // namespace rt
