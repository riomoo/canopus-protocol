// The window on Windows: plain Win32, drawing with OpenGL (vsync: no
// tearing) or, if that is not available, GDI. A thread of its own owns the
// window and its message loop; frames arrive from the VBlank thread through
// Show.
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
#include <GL/gl.h>

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
std::atomic<bool> s_newFrame{false};

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// OpenGL output: the frame as a texture (512x512: OpenGL 1.1 wants powers
// of two), drawn scaled to the window; SwapBuffers waits for the monitor.
struct Gl {
    bool on = false;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    GLuint tex = 0;
} s_gl;
constexpr int TEX = 512;

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

bool InitGl(HWND hwnd)
{
    const Options& o = g_options;
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int format = ChoosePixelFormat(dc, &pfd);
    HGLRC rc = format && SetPixelFormat(dc, format, &pfd) ? wglCreateContext(dc) : nullptr;
    if (!rc || !wglMakeCurrent(dc, rc)) {
        Log(LOG_WARN, "window", "OpenGL is not available (error %lu): drawing with GDI", GetLastError());
        if (rc) wglDeleteContext(rc);
        ReleaseDC(hwnd, dc);
        return false;
    }
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    const char* vendor = (const char*)glGetString(GL_VENDOR);
    if (!renderer || std::strstr(renderer, "GDI Generic")) {
        // Windows' own software OpenGL: no graphics driver; GDI does better.
        Log(LOG_WARN, "window", "no OpenGL driver (only Windows' software one): drawing with GDI");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(rc);
        ReleaseDC(hwnd, dc);
        return false;
    }
    typedef BOOL(WINAPI * SwapInterval)(int);
    const auto swap = (SwapInterval)wglGetProcAddress("wglSwapIntervalEXT");
    const bool vsync = swap && swap(o.vsync ? 1 : 0) && o.vsync;
    glGenTextures(1, &s_gl.tex);
    glBindTexture(GL_TEXTURE_2D, s_gl.tex);
    const GLint filter = o.smooth ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    std::vector<u32> black(TEX * TEX, 0xFF000000u);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TEX, TEX, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, black.data());
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    s_gl.dc = dc;
    s_gl.rc = rc;
    s_gl.on = true;
    Log(LOG_INFO, "window", "output: OpenGL (%s, %s), vsync %s", renderer, vendor ? vendor : "?", vsync ? "on" : "off");
    return true;
}

void DrawGl(HWND hwnd)
{
    RECT c;
    GetClientRect(hwnd, &c);
    if (c.right <= 0 || c.bottom <= 0) return;
    if (s_newFrame.exchange(false)) {
        std::lock_guard<std::mutex> g(s_frameLock);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, s_frame.data());
    }
    glViewport(0, 0, c.right, c.bottom);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, c.right, c.bottom, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    const RECT r = Fit(c.right, c.bottom);
    const float u = (float)W / TEX, v = (float)H / TEX;
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex2i(r.left, r.top);
    glTexCoord2f(u, 0);
    glVertex2i(r.right, r.top);
    glTexCoord2f(u, v);
    glVertex2i(r.right, r.bottom);
    glTexCoord2f(0, v);
    glVertex2i(r.left, r.bottom);
    glEnd();
    SwapBuffers(s_gl.dc);  // waits for the monitor's refresh (vsync)
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP:  // a new frame
        if (s_gl.on) {
            if (s_newFrame) DrawGl(hwnd);  // frames queued while waiting are skipped
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SIZE:
        if (s_gl.on) DrawGl(hwnd);
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
        if (s_gl.on) {
            ValidateRect(hwnd, nullptr);
            DrawGl(hwnd);
        } else {
            Paint(hwnd);
        }
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
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_OWNDC;  // OpenGL keeps the window's device context
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // The icon from res/canopus.rc (resource 1), large and small.
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"ds2rb";
    RegisterClassExW(&wc);
    RECT r = {0, 0, W * 2, H * 2};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    std::wstring wtitle(title.begin(), title.end());
    s_hwnd = CreateWindowW(L"ds2rb", wtitle.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                           r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    *ready = s_hwnd ? 1 : -1;
    if (!s_hwnd) return;
    if (g_options.output != "gdi" && !InitGl(s_hwnd)) g_options.output = "gdi";
    if (!s_gl.on) Log(LOG_INFO, "window", "output: GDI (no vsync)");
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
    s_newFrame = true;
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
