// The window on Linux: SDL2 (loaded at run time), on a thread of its own
// that owns the window and its events. Same keys as on Windows:
// arrows = +Control Pad, X = A, Z = B, S = X, A = Y, Q = L, W = R,
// Enter = START, Backspace = SELECT, I/J/K/L = Circle Pad; the left mouse
// button on the bottom screen touches it.
#include "window.h"
#include "cheats.h"

#ifndef _WIN32

#include "sdl.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rt {
namespace window {

namespace {

constexpr int W = 400, H = 480, BOTTOM_X = 40, BOTTOM_Y = 240;

std::mutex s_frameLock;
std::vector<u32> s_frame(W * H, 0);
std::atomic<bool> s_newFrame{false};
std::atomic<u32> s_buttons{0};
std::atomic<bool> s_touch{false};
std::atomic<int> s_touchX{0}, s_touchY{0};
std::atomic<int> s_circle{0};
std::mutex s_statusLock;
std::string s_status;
std::atomic<bool> s_newStatus{false};

// SDL scancodes.
u32 KeyButton(int sc)
{
    switch (sc) {
    case 82: return input::UP;
    case 81: return input::DOWN;
    case 80: return input::LEFT;
    case 79: return input::RIGHT;
    case 27: return input::A;       // X
    case 29: return input::B;       // Z
    case 22: return input::X;       // S
    case 4: return input::Y;        // A
    case 20: return input::L;       // Q
    case 26: return input::R;       // W
    case 40: case 88: return input::START;  // Enter, keypad Enter
    case 42: return input::SELECT;  // Backspace
    default: return 0;
    }
}

int KeyCircle(int sc)
{
    switch (sc) {
    case 13: return 1;  // J
    case 15: return 2;  // L
    case 12: return 4;  // I
    case 14: return 8;  // K
    default: return 0;
    }
}

void Touch(const sdl::Api* s, void* renderer, int wx, int wy)
{
    float lx = (float)wx, ly = (float)wy;
    if (s->RenderWindowToLogical) s->RenderWindowToLogical(renderer, wx, wy, &lx, &ly);
    const int x = (int)lx - BOTTOM_X, y = (int)ly - BOTTOM_Y;
    if (x < 0 || x >= 320 || y < 0 || y >= 240) {
        s_touch = false;
        return;
    }
    s_touchX = x;
    s_touchY = y;
    s_touch = true;
}

void Run(std::string title, std::atomic<int>* ready)
{
    const sdl::Api* s = sdl::Get();
    if (!s || s->InitSubSystem(sdl::INIT_VIDEO) != 0) {
        if (s) Log(LOG_WARN, "window", "SDL video: %s", s->GetError());
        *ready = -1;
        return;
    }
    void* win = s->CreateWindow(title.c_str(), sdl::WINDOWPOS_UNDEFINED, sdl::WINDOWPOS_UNDEFINED, W * 2, H * 2, sdl::WINDOW_RESIZABLE);
    void* ren = win ? s->CreateRenderer(win, -1, sdl::RENDERER_ACCELERATED) : nullptr;
    if (win && !ren) ren = s->CreateRenderer(win, -1, 0);
    void* tex = ren ? s->CreateTexture(ren, sdl::PIXELFORMAT_ARGB8888, sdl::TEXTUREACCESS_STREAMING, W, H) : nullptr;
    if (!tex) {
        Log(LOG_WARN, "window", "SDL window: %s", s->GetError());
        *ready = -1;
        return;
    }
    s->RenderSetLogicalSize(ren, W, H);  // scaled to the window, aspect kept
    *ready = 1;
    std::vector<u32> local(W * H);
    for (;;) {
        sdl::Event e;
        if (s->WaitEventTimeout(&e, 4)) {
            do {
                switch (e.type) {
                case sdl::QUIT:
                    Log(LOG_INFO, "window", "closed");
                    std::_Exit(0);
                case sdl::KEYDOWN:
                case sdl::KEYUP: {
                    const bool down = e.type == sdl::KEYDOWN;
                    const int sc = e.key.keysym.scancode;
                    // F1 and, while it is open, the keys that drive the cheat menu.
                    if (down && sc == 58) { cheat::PushKey(cheat::KEY_TOGGLE); break; }
                    if (cheat::Open()) {
                        int k = 0;
                        switch (sc) {
                        case 82: k = cheat::KEY_UP; break;
                        case 81: k = cheat::KEY_DOWN; break;
                        case 80: k = cheat::KEY_LEFT; break;
                        case 79: k = cheat::KEY_RIGHT; break;
                        case 40: case 88: k = cheat::KEY_ENTER; break;
                        case 41: k = cheat::KEY_BACK; break;
                        case 42: k = cheat::KEY_ERASE; break;
                        case 75: k = cheat::KEY_PAGE_UP; break;
                        case 78: k = cheat::KEY_PAGE_DOWN; break;
                        }
                        if (down && k) cheat::PushKey(k);
                        s_buttons = 0;
                        s_circle = 0;
                        break;
                    }
                    if (const u32 b = KeyButton(e.key.keysym.scancode)) {
                        if (down) s_buttons |= b;
                        else s_buttons &= ~b;
                    }
                    if (const int k = KeyCircle(e.key.keysym.scancode)) {
                        if (down) s_circle |= k;
                        else s_circle &= ~k;
                    }
                    break;
                }
                case sdl::TEXTINPUT:  // typing searches the cheat menu's lists
                    if (cheat::Open())
                        for (const char* t = e.text.text; *t; ++t)
                            if (*t >= 32 && *t < 127) cheat::PushKey(*t);
                    break;
                case sdl::MOUSEBUTTONDOWN:
                    if (e.button.button == 1) Touch(s, ren, e.button.x, e.button.y);
                    break;
                case sdl::MOUSEBUTTONUP:
                    if (e.button.button == 1) s_touch = false;
                    break;
                case sdl::MOUSEMOTION:
                    if (e.motion.state & 1) Touch(s, ren, e.motion.x, e.motion.y);
                    break;
                default:
                    break;
                }
            } while (s->PollEvent(&e));
        }
        if (s_newStatus.exchange(false)) {
            std::string t;
            {
                std::lock_guard<std::mutex> g(s_statusLock);
                t = title + "  -  " + s_status;
            }
            s->SetWindowTitle(win, t.c_str());
        }
        if (s_newFrame.exchange(false)) {
            {
                std::lock_guard<std::mutex> g(s_frameLock);
                std::memcpy(local.data(), s_frame.data(), local.size() * 4);
            }
            s->UpdateTexture(tex, nullptr, local.data(), W * 4);
            s->SetRenderDrawColor(ren, 0, 0, 0, 255);
            s->RenderClear(ren);
            s->RenderCopy(ren, tex, nullptr, nullptr);
            s->RenderPresent(ren);
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
    std::lock_guard<std::mutex> g(s_frameLock);
    for (int y = 0; y < 240; ++y) {
        if (top) std::memcpy(&s_frame[y * W], top + y * 400, 400 * 4);
        else std::memset(&s_frame[y * W], 0, 400 * 4);
        u32* row = &s_frame[(BOTTOM_Y + y) * W];
        if (bottom) std::memcpy(row + BOTTOM_X, bottom + y * 320, 320 * 4);
        else std::memset(row + BOTTOM_X, 0, 320 * 4);
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
    const int c = s_circle;
    st.circleX = (s16)((c & 2 ? 156 : 0) - (c & 1 ? 156 : 0));
    st.circleY = (s16)((c & 4 ? 156 : 0) - (c & 8 ? 156 : 0));
    return st;
}

}  // namespace window
}  // namespace rt

#endif
