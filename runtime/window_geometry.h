// The window's first size and place, shared by the game and the launcher
// (both raylib, after InitWindow) so one becomes the other seamlessly:
// 16:9, its height the largest whole multiple of 480 (the two 3DS screens'
// height, so the pixels stay even) that fits 90% of the monitor, centred.
#pragma once
#include "raylib.h"
#include <algorithm>

struct WindowRect {
    int x, y, w, h;
};

inline WindowRect DefaultWindowRect()
{
    const int m = GetCurrentMonitor();
    const int mw = GetMonitorWidth(m), mh = GetMonitorHeight(m);
    if (mw <= 0 || mh <= 0) return {-1, -1, 1280, 720};
    int h = 480;
    while (h + 480 <= mh * 9 / 10) h += 480;
    int w = std::min(h * 16 / 9, mw * 95 / 100);
    if (h > mh) h = mh;
    const Vector2 mp = GetMonitorPosition(m);
    return {(int)mp.x + (mw - w) / 2, (int)mp.y + std::max(0, (mh - h) / 2), w, h};
}
