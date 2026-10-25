// The window on Windows: plain Win32 and GDI, no other libraries. A thread
// of its own owns the window and its message loop; frames arrive from the
// VBlank thread through Show.
//
// Keys: arrows = +Control Pad, X = A, Z = B, S = X, A = Y, Q = L, W = R,
// Enter = START, Backspace = SELECT, I/J/K/L = Circle Pad. The left mouse
// button on the bottom screen touches it.
#include "window.h"
#include "cheats.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rt {
namespace window {

namespace {

// The picture: top screen over the bottom one, centred.
constexpr int W = 400, H = 480, BOTTOM_X = 40, BOTTOM_Y = 240;

HWND s_hwnd = nullptr;
std::string s_title;
std::mutex s_statusLock;
std::string s_status;
std::mutex s_frameLock;
std::vector<u32> s_frame(W * H, 0);
std::atomic<u32> s_buttons{0};
std::atomic<bool> s_touch{false};
std::atomic<int> s_touchX{0}, s_touchY{0};
std::atomic<int> s_circle{0};  // bits: 1 left, 2 right, 4 up, 8 down

// Where the picture goes in the client area (scaled, aspect kept).
RECT Fit(int cw, int ch)
{
    const double scale = std::min((double)cw / W, (double)ch / H);
    const int w = (int)(W * scale), h = (int)(H * scale);
    RECT r;
    r.left = (cw - w) / 2;
    r.top = (ch - h) / 2;
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

u32 KeyButton(WPARAM vk)
{
    switch (vk) {
    case VK_UP: return input::UP;
    case VK_DOWN: return input::DOWN;
    case VK_LEFT: return input::LEFT;
    case VK_RIGHT: return input::RIGHT;
    case 'X': return input::A;
    case 'Z': return input::B;
    case 'S': return input::X;
    case 'A': return input::Y;
    case 'Q': return input::L;
    case 'W': return input::R;
    case VK_RETURN: return input::START;
    case VK_BACK: return input::SELECT;
    default: return 0;
    }
}

int KeyCircle(WPARAM vk)
{
    switch (vk) {
    case 'J': return 1;
    case 'L': return 2;
    case 'I': return 4;
    case 'K': return 8;
    default: return 0;
    }
}

// Mouse position to bottom-screen pixels; false if outside it.
bool ToTouch(HWND hwnd, int mx, int my, int* tx, int* ty)
{
    RECT c;
    GetClientRect(hwnd, &c);
    const RECT r = Fit(c.right, c.bottom);
    if (r.right <= r.left || r.bottom <= r.top) return false;
    const int px = (mx - r.left) * W / (r.right - r.left), py = (my - r.top) * H / (r.bottom - r.top);
    const int x = px - BOTTOM_X, y = py - BOTTOM_Y;
    if (x < 0 || x >= 320 || y < 0 || y >= 240) return false;
    *tx = x;
    *ty = y;
    return true;
}

void Paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT c;
    GetClientRect(hwnd, &c);
    // Draw off screen, then copy: no flicker.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    FillRect(mem, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
    const RECT r = Fit(c.right, c.bottom);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(mem, COLORONCOLOR);
    {
        std::lock_guard<std::mutex> g(s_frameLock);
        StretchDIBits(mem, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, W, H, s_frame.data(), &bi, DIB_RGB_COLORS,
                      SRCCOPY);
    }
    BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP:  // a new frame
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_APP + 1: {  // a new status line
        std::string t;
        {
            std::lock_guard<std::mutex> g(s_statusLock);
            t = s_title + "  -  " + s_status;
        }
        SetWindowTextA(hwnd, t.c_str());
        return 0;
    }
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        // F1 and, while it is open, the keys that drive the cheat menu.
        if (wp == VK_F1) { cheat::PushKey(cheat::KEY_TOGGLE); return 0; }
        if (cheat::Open()) {
            int k = 0;
            switch (wp) {
            case VK_UP: k = cheat::KEY_UP; break;
            case VK_DOWN: k = cheat::KEY_DOWN; break;
            case VK_LEFT: k = cheat::KEY_LEFT; break;
            case VK_RIGHT: k = cheat::KEY_RIGHT; break;
            case VK_RETURN: k = cheat::KEY_ENTER; break;
            case VK_ESCAPE: k = cheat::KEY_BACK; break;
            case VK_BACK: k = cheat::KEY_ERASE; break;
            case VK_PRIOR: k = cheat::KEY_PAGE_UP; break;
            case VK_NEXT: k = cheat::KEY_PAGE_DOWN; break;
            }
            if (k) cheat::PushKey(k);
            s_buttons = 0;
            s_circle = 0;
            return 0;
        }
        [[fallthrough]];
    case WM_KEYUP: {
        const bool down = msg == WM_KEYDOWN;
        if (const u32 b = KeyButton(wp)) {
            if (down) s_buttons |= b;
            else s_buttons &= ~b;
        }
        if (const int k = KeyCircle(wp)) {
            if (down) s_circle |= k;
            else s_circle &= ~k;
        }
        return 0;
    }
    case WM_CHAR:  // typing searches the cheat menu's lists
        if (cheat::Open() && wp >= 32 && wp < 127) cheat::PushKey((int)wp);
        return 0;
    case WM_KILLFOCUS:  // keys released elsewhere would stay held
        s_buttons = 0;
        s_circle = 0;
        s_touch = false;
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE: {
        if (msg == WM_MOUSEMOVE && !(wp & MK_LBUTTON)) return 0;
        int x, y;
        if (ToTouch(hwnd, (short)LOWORD(lp), (short)HIWORD(lp), &x, &y)) {
            s_touchX = x;
            s_touchY = y;
            s_touch = true;
            if (msg == WM_LBUTTONDOWN) SetCapture(hwnd);
        } else if (msg == WM_LBUTTONDOWN) {
            s_touch = false;
        }
        return 0;
    }
    case WM_LBUTTONUP:
        s_touch = false;
        ReleaseCapture();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Run(std::string title, std::atomic<int>* ready)
{
    s_title = title;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ds2rb";
    RegisterClassW(&wc);
    RECT r = {0, 0, W * 2, H * 2};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    std::wstring wtitle(title.begin(), title.end());
    s_hwnd = CreateWindowW(L"ds2rb", wtitle.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                           r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    *ready = s_hwnd ? 1 : -1;
    if (!s_hwnd) return;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // The window was closed: so is the game.
    Log(LOG_INFO, "window", "closed");
    std::_Exit(0);
}

}  // namespace

bool Open(const char* title)
{
    std::atomic<int> ready{0};
    std::thread(Run, std::string(title), &ready).detach();
    while (ready == 0) Sleep(1);
    return ready > 0;
}

void Show(const u32* top, const u32* bottom)
{
    {
        std::lock_guard<std::mutex> g(s_frameLock);
        for (int y = 0; y < 240; ++y) {
            if (top) std::memcpy(&s_frame[y * W], top + y * 400, 400 * 4);
            else std::memset(&s_frame[y * W], 0, 400 * 4);
            u32* row = &s_frame[(BOTTOM_Y + y) * W];
            if (bottom) std::memcpy(row + BOTTOM_X, bottom + y * 320, 320 * 4);
            else std::memset(row + BOTTOM_X, 0, 320 * 4);
        }
    }
    if (s_hwnd) PostMessageW(s_hwnd, WM_APP, 0, 0);
}

void SetStatus(const char* text)
{
    {
        std::lock_guard<std::mutex> g(s_statusLock);
        s_status = text;
    }
    if (s_hwnd) PostMessageW(s_hwnd, WM_APP + 1, 0, 0);
}

input::State Input()
{
    input::State s;
    if (cheat::Open()) return s;  // the menu has the keys
    s.buttons = s_buttons;
    s.touch = s_touch;
    s.touchX = (u16)s_touchX.load();
    s.touchY = (u16)s_touchY.load();
    const int c = s_circle;
    s.circleX = (s16)((c & 2 ? 156 : 0) - (c & 1 ? 156 : 0));
    s.circleY = (s16)((c & 4 ? 156 : 0) - (c & 8 ? 156 : 0));
    return s;
}

}  // namespace window
}  // namespace rt

#endif
