// The window, keyboard, mouse and controllers: raylib (GLFW + OpenGL), on a
// thread of its own that owns the window and its events.
//
// Keyboard: arrows = +Control Pad, X = A, Z = B, S = X, A = Y, Q = L, W = R,
// Enter = START, Backspace = SELECT, I/J/K/L = Circle Pad, F1 = cheat menu.
// Mouse: the left button on the bottom screen touches it.
//
// Controllers (any XInput / DirectInput / Linux pad GLFW knows, and Steam
// Input's virtual pad): buttons by their label on an Xbox pad (A = 3DS A,
// B = B, X = X, Y = Y; --nintendo-buttons swaps them for Nintendo-style pads),
// d-pad = +Control Pad, left stick = Circle Pad, LB/RB = L/R, Start / Back =
// START / SELECT. The right stick moves a pointer over the bottom screen;
// the right trigger or R3 touches there. L3 + R3 opens the cheat menu, which
// the d-pad, A (enter), B (back), Y (erase) and LB/RB (page) then drive.
#include "window.h"
#include "cheats.h"
#include "icon_pixels.h"
#include "res/backdrop_png.h"
#include "window_geometry.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rt {

// raylib's own messages go to the game's log.
void RaylibLog(int level, const char* fmt, va_list args)
{
    char line[512];
    std::vsnprintf(line, sizeof line, fmt, args);
    const LogLevel lv = level >= ::LOG_ERROR ? LOG_ERROR : level >= ::LOG_WARNING ? LOG_WARN : level >= ::LOG_INFO ? LOG_INFO : LOG_DEBUG;
    Log(lv, "raylib", "%s", line);
}

void InstallRaylibLog()
{
    static std::once_flag once;
    std::call_once(once, [] {
        ::SetTraceLogCallback(RaylibLog);
        ::SetTraceLogLevel(::LOG_INFO);
    });
}

namespace window {

namespace {

constexpr int W = 400, H = 480, BOTTOM_X = 40, BOTTOM_Y = 240;
constexpr int kCircle = 156;  // full deflection of the Circle Pad

std::mutex s_frameLock;
std::vector<u32> s_frame(W * H, 0xFF000000u);  // RGBA bytes, as the texture wants
std::atomic<bool> s_newFrame{false};
std::atomic<u32> s_buttons{0};
std::atomic<bool> s_touch{false};
std::atomic<int> s_touchX{0}, s_touchY{0};
std::atomic<int> s_circleX{0}, s_circleY{0};
std::mutex s_statusLock;
std::string s_status;
std::atomic<bool> s_newStatus{false};

struct KeyMap {
    int key;
    u32 button;
};
const KeyMap kKeys[] = {
    {KEY_UP, input::UP},       {KEY_DOWN, input::DOWN}, {KEY_LEFT, input::LEFT}, {KEY_RIGHT, input::RIGHT},
    {KEY_X, input::A},         {KEY_Z, input::B},       {KEY_S, input::X},       {KEY_A, input::Y},
    {KEY_Q, input::L},         {KEY_W, input::R},       {KEY_ENTER, input::START}, {KEY_KP_ENTER, input::START},
    {KEY_BACKSPACE, input::SELECT},
};

// A controller's state, merged over every pad that is connected.
struct Pad {
    bool any = false;
    u32 buttons = 0;     // 3DS buttons
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    bool l3 = false, r3 = false;
    bool confirm = false, back = false, erase = false, pageUp = false, pageDown = false;  // for the cheat menu
};

float Dead(float v, float dz)
{
    if (std::fabs(v) < dz) return 0;
    return (v - std::copysign(dz, v)) / (1 - dz);
}

Pad ReadPads(bool nintendo)
{
    Pad p;
    for (int i = 0; i < 4; ++i) {
        if (!IsGamepadAvailable(i)) continue;
        p.any = true;
        auto b = [&](int btn) { return IsGamepadButtonDown(i, btn); };
        // Face buttons: by label on an Xbox pad (bottom = A), or by label on
        // a Nintendo pad (right = A) with --nintendo-buttons.
        const int faceA = nintendo ? GAMEPAD_BUTTON_RIGHT_FACE_RIGHT : GAMEPAD_BUTTON_RIGHT_FACE_DOWN;
        const int faceB = nintendo ? GAMEPAD_BUTTON_RIGHT_FACE_DOWN : GAMEPAD_BUTTON_RIGHT_FACE_RIGHT;
        const int faceX = nintendo ? GAMEPAD_BUTTON_RIGHT_FACE_UP : GAMEPAD_BUTTON_RIGHT_FACE_LEFT;
        const int faceY = nintendo ? GAMEPAD_BUTTON_RIGHT_FACE_LEFT : GAMEPAD_BUTTON_RIGHT_FACE_UP;
        if (b(faceA)) p.buttons |= input::A, p.confirm = true;
        if (b(faceB)) p.buttons |= input::B, p.back = true;
        if (b(faceX)) p.buttons |= input::X;
        if (b(faceY)) p.buttons |= input::Y, p.erase = true;
        if (b(GAMEPAD_BUTTON_LEFT_FACE_UP)) p.buttons |= input::UP;
        if (b(GAMEPAD_BUTTON_LEFT_FACE_DOWN)) p.buttons |= input::DOWN;
        if (b(GAMEPAD_BUTTON_LEFT_FACE_LEFT)) p.buttons |= input::LEFT;
        if (b(GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) p.buttons |= input::RIGHT;
        if (b(GAMEPAD_BUTTON_LEFT_TRIGGER_1)) p.buttons |= input::L, p.pageUp = true;
        if (b(GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) p.buttons |= input::R, p.pageDown = true;
        if (b(GAMEPAD_BUTTON_MIDDLE_RIGHT)) p.buttons |= input::START;
        if (b(GAMEPAD_BUTTON_MIDDLE_LEFT)) p.buttons |= input::SELECT;
        if (b(GAMEPAD_BUTTON_LEFT_THUMB)) p.l3 = true;
        if (b(GAMEPAD_BUTTON_RIGHT_THUMB)) p.r3 = true;
        auto axis = [&](int a) { return GetGamepadAxisMovement(i, a); };
        const float lx = axis(GAMEPAD_AXIS_LEFT_X), ly = axis(GAMEPAD_AXIS_LEFT_Y);
        const float rx = axis(GAMEPAD_AXIS_RIGHT_X), ry = axis(GAMEPAD_AXIS_RIGHT_Y);
        if (std::hypot(lx, ly) > std::hypot(p.lx, p.ly)) p.lx = lx, p.ly = ly;
        if (std::hypot(rx, ry) > std::hypot(p.rx, p.ry)) p.rx = rx, p.ry = ry;
        // Triggers read -1 at rest, 1 fully pressed.
        p.lt = std::max(p.lt, (axis(GAMEPAD_AXIS_LEFT_TRIGGER) + 1) / 2);
        p.rt = std::max(p.rt, (axis(GAMEPAD_AXIS_RIGHT_TRIGGER) + 1) / 2);
    }
    return p;
}

// Repeats a held direction for the cheat menu: once at once, then after a
// pause, then steadily.
struct Repeat {
    double since = -1, next = 0;
    bool Fire(bool down, double now)
    {
        if (!down) {
            since = -1;
            return false;
        }
        if (since < 0) {
            since = now;
            next = now + 0.35;
            return true;
        }
        if (now >= next) {
            next = now + 0.07;
            return true;
        }
        return false;
    }
};

// Where the picture goes in the window: scaled to fit, aspect kept.
struct Fit {
    float x, y, scale;
};
Fit FitWindow()
{
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    const float s = std::max(0.01f, std::min(sw / W, sh / H));
    return {(sw - W * s) / 2, (sh - H * s) / 2, s};
}

// Draws a backdrop to cover the whole window (cropped, aspect kept).
void DrawCover(const Texture2D& t, Color tint)
{
    if (!IsTextureValid(t)) return;
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    const float s = std::max(sw / t.width, sh / t.height);
    const float w = sw / s, h = sh / s;
    DrawTexturePro(t, {(t.width - w) / 2, (t.height - h) / 2, w, h}, {0, 0, sw, sh}, {0, 0}, 0, tint);
}

void DrawFrame(const Texture2D& tex, const Texture2D& logo, const Texture2D& plain, float fade, bool pointer, float ptrX, float ptrY)
{
    const Fit f = FitWindow();
    auto alpha = [](float a) { return (unsigned char)std::lround(std::clamp(a, 0.0f, 1.0f) * 255); };
    if (fade < 1) DrawCover(logo, WHITE);
    if (fade > 0) {
        // The game: the backdrop dimmed behind the screens (or black), the
        // screens with a soft shadow.
        if (IsTextureValid(plain)) DrawCover(plain, Color{88, 92, 104, alpha(fade)});
        else DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, alpha(fade)});
        // The two screens, each with a soft shadow and a thin blue edge.
        const float pad = std::max(2.0f, f.scale * 3);
        const Rectangle src[2] = {{0, 0, 400, 240}, {(float)BOTTOM_X, (float)BOTTOM_Y, 320, 240}};
        for (const Rectangle& r : src) {
            const Rectangle d = {f.x + r.x * f.scale, f.y + r.y * f.scale, r.width * f.scale, r.height * f.scale};
            if (IsTextureValid(plain)) {
                DrawRectangleRec({d.x - pad, d.y - pad, d.width + 2 * pad, d.height + 2 * pad}, Color{0, 0, 0, alpha(fade * 0.45f)});
                DrawRectangleLinesEx({d.x - 1, d.y - 1, d.width + 2, d.height + 2}, 1, Color{90, 170, 235, alpha(fade * 0.7f)});
            }
            DrawTexturePro(tex, r, d, {0, 0}, 0, Color{255, 255, 255, alpha(fade)});
        }
    }
    if (pointer && fade >= 1) {
        const float px = f.x + (BOTTOM_X + ptrX + 0.5f) * f.scale, py = f.y + (BOTTOM_Y + ptrY + 0.5f) * f.scale;
        const float r = std::max(4.0f, 3.5f * f.scale);
        const Color c = s_touch ? Color{255, 61, 139, 255} : Color{255, 255, 255, 230};
        DrawCircleLinesV({px, py}, r + 1, Color{0, 0, 0, 200});
        DrawCircleLinesV({px, py}, r, c);
        DrawCircleV({px, py}, std::max(1.5f, f.scale * 0.75f), c);
    }
}

void Run(std::string title, std::atomic<int>* ready)
{
    InstallRaylibLog();
    const Options& o = g_options;
    unsigned flags = FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIDDEN;  // shown once it has its size, place and picture
    if (o.vsync) flags |= FLAG_VSYNC_HINT;  // frames reach the screen between refreshes: no tearing
    SetConfigFlags(flags);
    InitWindow(1280, 720, title.c_str());
    if (!IsWindowReady()) {
        Log(LOG_WARN, "window", "no window: OpenGL 2.1 or newer is needed");
        *ready = -1;
        return;
    }
    SetExitKey(KEY_NULL);  // Esc is for the cheat menu, not for quitting
    SetTraceLogLevel(LOG_WARNING);
    // Where the launcher's window was (it hands over to this one), else the
    // default size and place.
    {
        WindowRect r = DefaultWindowRect();
        if (o.rect[2] > 0 && o.rect[3] > 0) r = {o.rect[0], o.rect[1], o.rect[2], o.rect[3]};
        SetWindowSize(r.w, r.h);
        if (r.x != -1 || r.y != -1) SetWindowPosition(r.x, r.y);
    }
    // The window icon (taskbar, title bar, Alt-Tab): RGBA bytes.
    {
        static u32 icon[64 * 64];
        for (int i = 0; i < 64 * 64; ++i) {
            const u32 p = kIconPixels[i];
            icon[i] = (p & 0xFF00FF00u) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
        }
        Image img = {icon, 64, 64, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        SetWindowIcon(img);
    }
    std::vector<u32> local(W * H, 0xFF000000u);
    Image frame = {local.data(), W, H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D tex = LoadTextureFromImage(frame);
    if (!IsTextureValid(tex)) {
        Log(LOG_WARN, "window", "could not make the screen texture");
        *ready = -1;
        return;
    }
    SetTextureFilter(tex, o.smooth ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
    // The backdrops: with the logo while the game starts (the launcher's
    // picture), without it around the screens once the game runs.
    auto loadPng = [](const unsigned char* data, int size) {
        Texture2D t = {};
        Image img = LoadImageFromMemory(".png", data, size);
        if (img.data) {
            t = LoadTextureFromImage(img);
            UnloadImage(img);
            if (IsTextureValid(t)) {
                GenTextureMipmaps(&t);
                SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
            }
        }
        return t;
    };
    const Texture2D logo = loadPng(kBackdropLogoPng, sizeof kBackdropLogoPng);
    const Texture2D plain = o.backdrop ? loadPng(kBackdropPng, sizeof kBackdropPng) : Texture2D{};
    Log(LOG_INFO, "window", "output: raylib OpenGL, %dx%d, vsync %s, %s scaling", GetScreenWidth(), GetScreenHeight(),
        o.vsync ? "on" : "off", o.smooth ? "smooth" : "sharp");
    *ready = 1;

    u32 keyButtons = 0;
    int keyCircle = 0;
    bool mouseTouch = false;
    float ptrX = 160, ptrY = 120;  // the controller's pointer, bottom screen pixels
    double ptrShown = -100;        // last time it moved or touched
    bool chord = false;            // L3 + R3 held
    bool padSeen = false;
    Repeat rUp, rDown, rLeft, rRight;
    double last = GetTime();
    double fadeStart = -1, shownAt = 0;  // the game's first picture fades in over the starting backdrop
    float fade = 0;
    bool shown = false;
    std::string readyFile = o.readyFile;
    for (;;) {
        if (WindowShouldClose()) {
            Log(LOG_INFO, "window", "closed");
            std::_Exit(0);
        }
        const double now = GetTime();
        const float dt = (float)std::min(0.1, now - last);
        last = now;

        // ---- keyboard ----
        if (IsKeyPressed(KEY_F1)) cheat::PushKey(cheat::KEY_TOGGLE);
        const Pad pad = ReadPads(o.nintendoButtons);
        if (pad.any != padSeen) {
            padSeen = pad.any;
            if (pad.any)
                for (int i = 0; i < 4; ++i)
                    if (IsGamepadAvailable(i)) Log(LOG_INFO, "window", "controller %d: %s", i, GetGamepadName(i));
        }
        // L3 + R3: the cheat menu.
        const bool chordNow = pad.l3 && pad.r3;
        if (chordNow && !chord) cheat::PushKey(cheat::KEY_TOGGLE);
        chord = chordNow;

        if (cheat::Open()) {
            auto key = [](int k) { return IsKeyPressed(k) || IsKeyPressedRepeat(k); };
            const bool up = pad.buttons & input::UP || pad.ly < -0.6f, down = pad.buttons & input::DOWN || pad.ly > 0.6f;
            const bool left = pad.buttons & input::LEFT || pad.lx < -0.6f, right = pad.buttons & input::RIGHT || pad.lx > 0.6f;
            if (rUp.Fire(up, now) || key(KEY_UP)) cheat::PushKey(cheat::KEY_UP);
            if (rDown.Fire(down, now) || key(KEY_DOWN)) cheat::PushKey(cheat::KEY_DOWN);
            if (rLeft.Fire(left, now) || key(KEY_LEFT)) cheat::PushKey(cheat::KEY_LEFT);
            if (rRight.Fire(right, now) || key(KEY_RIGHT)) cheat::PushKey(cheat::KEY_RIGHT);
            if (key(KEY_ENTER) || key(KEY_KP_ENTER)) cheat::PushKey(cheat::KEY_ENTER);
            if (key(KEY_ESCAPE)) cheat::PushKey(cheat::KEY_BACK);
            if (key(KEY_BACKSPACE)) cheat::PushKey(cheat::KEY_ERASE);
            if (key(KEY_PAGE_UP)) cheat::PushKey(cheat::KEY_PAGE_UP);
            if (key(KEY_PAGE_DOWN)) cheat::PushKey(cheat::KEY_PAGE_DOWN);
            static Pad prev;
            if (pad.confirm && !prev.confirm) cheat::PushKey(cheat::KEY_ENTER);
            if (pad.back && !prev.back) cheat::PushKey(cheat::KEY_BACK);
            if (pad.erase && !prev.erase) cheat::PushKey(cheat::KEY_ERASE);
            if (pad.pageUp && !prev.pageUp) cheat::PushKey(cheat::KEY_PAGE_UP);
            if (pad.pageDown && !prev.pageDown) cheat::PushKey(cheat::KEY_PAGE_DOWN);
            prev = pad;
            // Typing searches the menu's lists.
            for (int c = GetCharPressed(); c; c = GetCharPressed())
                if (c >= 32 && c < 127) cheat::PushKey(c);
            keyButtons = 0;
            keyCircle = 0;
            s_buttons = 0;
            s_circleX = s_circleY = 0;
            s_touch = false;
        } else {
            while (GetCharPressed()) {}
            rUp = rDown = rLeft = rRight = Repeat();
            keyButtons = 0;
            const bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
            for (const KeyMap& k : kKeys)
                if (IsKeyDown(k.key) && !(alt && k.button == input::START)) keyButtons |= k.button;
            keyCircle = (IsKeyDown(KEY_J) ? 1 : 0) | (IsKeyDown(KEY_L) ? 2 : 0) | (IsKeyDown(KEY_I) ? 4 : 0) | (IsKeyDown(KEY_K) ? 8 : 0);
            s_buttons = keyButtons | pad.buttons;
            // Circle Pad: the keys, else the left stick (radial dead zone).
            int cx = (keyCircle & 2 ? kCircle : 0) - (keyCircle & 1 ? kCircle : 0);
            int cy = (keyCircle & 4 ? kCircle : 0) - (keyCircle & 8 ? kCircle : 0);
            if (!keyCircle) {
                const float mag = std::hypot(pad.lx, pad.ly);
                if (mag > 0.2f) {
                    const float k = std::min(1.0f, (mag - 0.2f) / 0.75f) / mag;
                    cx = (int)std::lround(pad.lx * k * kCircle);
                    cy = (int)std::lround(-pad.ly * k * kCircle);  // the 3DS's up is positive
                }
            }
            s_circleX = cx;
            s_circleY = cy;

            // ---- touch: the mouse, else the controller's pointer ----
            const Fit f = FitWindow();
            const Vector2 m = GetMousePosition();
            const int mx = (int)std::floor((m.x - f.x) / f.scale) - BOTTOM_X;
            const int my = (int)std::floor((m.y - f.y) / f.scale) - BOTTOM_Y;
            const bool onBottom = mx >= 0 && mx < 320 && my >= 0 && my < 240;
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) mouseTouch = onBottom;
            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) mouseTouch = false;
            const float sx = Dead(pad.rx, 0.15f), sy = Dead(pad.ry, 0.15f);
            if (sx || sy) {
                const float speed = 360.0f * dt;  // pixels per second at full tilt
                ptrX = std::clamp(ptrX + sx * std::fabs(sx) * speed, 0.0f, 319.0f);
                ptrY = std::clamp(ptrY + sy * std::fabs(sy) * speed, 0.0f, 239.0f);
                ptrShown = now;
            }
            const bool padTouch = !pad.l3 && (pad.r3 || pad.rt > 0.5f);
            if (padTouch) ptrShown = now;
            if (mouseTouch && onBottom) {
                s_touchX = mx;
                s_touchY = my;
                s_touch = true;
            } else if (mouseTouch) {
                s_touch = false;  // dragged off the screen
            } else if (padTouch) {
                s_touchX = (int)ptrX;
                s_touchY = (int)ptrY;
                s_touch = true;
            } else {
                s_touch = false;
            }
        }

        if (s_newStatus.exchange(false)) {
            std::string t;
            {
                std::lock_guard<std::mutex> g(s_statusLock);
                t = title + "  -  " + s_status;
            }
            SetWindowTitle(t.c_str());
        }

        const bool fresh = s_newFrame.exchange(false);
        if (fresh && fadeStart < 0) fadeStart = now + 0.15;
        fade = fadeStart < 0 ? 0 : (float)std::clamp((now - fadeStart) / 0.6, 0.0, 1.0);
        if (IsKeyPressed(KEY_F11) || ((IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)) && IsKeyPressed(KEY_ENTER)))
            ToggleBorderlessWindowed();
        if (fresh) {
            {
                std::lock_guard<std::mutex> g(s_frameLock);
                std::memcpy(local.data(), s_frame.data(), local.size() * 4);
            }
            UpdateTexture(tex, local.data());
        }
        const bool pointer = now - ptrShown < 3.0 && !cheat::Open();
        if (IsWindowMinimized()) {
            PollInputEvents();
            WaitTime(0.02);
        } else if (o.vsync || fresh || pointer || IsWindowResized() || fade < 1) {
            // With vsync this waits for the monitor's refresh.
            BeginDrawing();
            ClearBackground(BLACK);
            DrawFrame(tex, logo, plain, fade, pointer, ptrX, ptrY);
            EndDrawing();
            if (!shown) {
                // The first picture is in place: show the window, then tell
                // the launcher, whose window goes away (the same picture, at
                // the same place).
                ClearWindowState(FLAG_WINDOW_HIDDEN);
                shown = true;
                shownAt = now;
            } else if (!readyFile.empty() && now - shownAt > 0.1) {
                std::ofstream(readyFile) << "ready\n";
                readyFile.clear();
            }
            if (!o.vsync && !fresh) WaitTime(0.004);
        } else {
            PollInputEvents();
            WaitTime(0.002);
        }
    }
}

}  // namespace

bool Open(const char* title)
{
    std::atomic<int> ready{0};
    std::thread(Run, std::string(title), &ready).detach();
    while (ready == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return ready > 0;
}

void Show(const u32* top, const u32* bottom)
{
    // 0xAARRGGBB to the RGBA bytes the texture takes, opaque.
    auto row = [](u32* dst, const u32* src, int n) {
        for (int i = 0; i < n; ++i) {
            const u32 p = src[i];
            dst[i] = 0xFF000000u | (p & 0x0000FF00u) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
        }
    };
    std::lock_guard<std::mutex> g(s_frameLock);
    for (int y = 0; y < 240; ++y) {
        u32* t = &s_frame[y * W];
        if (top) row(t, top + y * 400, 400);
        else std::fill(t, t + 400, 0xFF000000u);
        u32* b = &s_frame[(BOTTOM_Y + y) * W] + BOTTOM_X;
        if (bottom) row(b, bottom + y * 320, 320);
        else std::fill(b, b + 320, 0xFF000000u);
    }
    s_newFrame = true;
}

void SetStatus(const char* text)
{
    std::lock_guard<std::mutex> g(s_statusLock);
    s_status = text;
    s_newStatus = true;
}

input::State Input()
{
    input::State st;
    if (cheat::Open()) return st;  // the menu has the keys
    st.buttons = s_buttons;
    st.touch = s_touch;
    st.touchX = (u16)s_touchX.load();
    st.touchY = (u16)s_touchY.load();
    st.circleX = (s16)s_circleX.load();
    st.circleY = (s16)s_circleY.load();
    return st;
}

}  // namespace window
}  // namespace rt
