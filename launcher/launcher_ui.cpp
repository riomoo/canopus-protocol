// The launcher's window: raylib, the backdrop with the logo, and a panel
// along the bottom with the progress.
#include "launcher_ui.h"

#include "raylib.h"
#include "res/font_ttf.h"
#include "../runtime/icon_pixels.h"
#include "../runtime/res/backdrop_png.h"
#include "../runtime/window_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace ui {

namespace {

const Color kPink = {226, 30, 108, 255};   // the PROTOCOL panel
const Color kBlue = {86, 172, 236, 255};   // the wave
const Color kPanel = {14, 14, 20, 228};
const Color kText = {244, 244, 248, 255};
const Color kDim = {170, 172, 184, 255};

Texture2D g_backdrop;
Font g_big = {}, g_small = {};
int g_fontFor = 0;  // window height the fonts were made for

void LoadFonts(int h)
{
    if (g_fontFor && std::abs(h - g_fontFor) < g_fontFor / 8) return;
    if (g_fontFor) {
        UnloadFont(g_big);
        UnloadFont(g_small);
    }
    int cps[224];
    for (int i = 0; i < 224; ++i) cps[i] = 32 + i;  // Latin-1
    g_big = LoadFontFromMemory(".ttf", kFontTtf, (int)sizeof kFontTtf, std::max(14, h * 34 / 1000), cps, 224);
    g_small = LoadFontFromMemory(".ttf", kFontTtf, (int)sizeof kFontTtf, std::max(12, h * 22 / 1000), cps, 224);
    SetTextureFilter(g_big.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(g_small.texture, TEXTURE_FILTER_BILINEAR);
    g_fontFor = h;
}

// Text cut to fit a width, with an ellipsis.
std::string Fit(const Font& f, std::string s, float width)
{
    const float size = (float)f.baseSize;
    if (MeasureTextEx(f, s.c_str(), size, 0).x <= width) return s;
    while (!s.empty() && MeasureTextEx(f, (s + "...").c_str(), size, 0).x > width) s.pop_back();
    return s + "...";
}

void Text(const Font& f, const std::string& s, float x, float y, Color c)
{
    DrawTextEx(f, s.c_str(), {std::round(x), std::round(y)}, (float)f.baseSize, 0, c);
}

void DrawCover(const Texture2D& t)
{
    if (!IsTextureValid(t)) return;
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    const float s = std::max(sw / t.width, sh / t.height);
    const float w = sw / s, h = sh / s;
    DrawTexturePro(t, {(t.width - w) / 2, (t.height - h) / 2, w, h}, {0, 0, sw, sh}, {0, 0}, 0, WHITE);
}

struct Snapshot {
    std::string stage, detail;
    float fraction;
    std::vector<std::string> lines;
    Progress::State state;
    bool needGames;
};

Snapshot Take(Progress& p)
{
    std::lock_guard<std::mutex> g(p.lock);
    return {p.stage, p.detail, p.fraction, p.state == Progress::Failed ? p.lines : std::vector<std::string>{}, p.state, p.needGames};
}

}  // namespace

bool Open(const char* title)
{
    SetTraceLogLevel(LOG_ERROR);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIDDEN | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, title);
    if (!IsWindowReady()) return false;
    SetExitKey(KEY_NULL);
    const WindowRect r = DefaultWindowRect();
    SetWindowSize(r.w, r.h);
    if (r.x != -1 || r.y != -1) SetWindowPosition(r.x, r.y);
    static uint32_t icon[64 * 64];
    for (int i = 0; i < 64 * 64; ++i) {
        const uint32_t p = kIconPixels[i];
        icon[i] = (p & 0xFF00FF00u) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
    }
    SetWindowIcon(Image{icon, 64, 64, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8});
    Image img = LoadImageFromMemory(".png", kBackdropLogoPng, (int)sizeof kBackdropLogoPng);
    if (img.data) {
        g_backdrop = LoadTextureFromImage(img);
        UnloadImage(img);
        GenTextureMipmaps(&g_backdrop);
        SetTextureFilter(g_backdrop, TEXTURE_FILTER_TRILINEAR);
    }
    return true;
}

bool Run(Progress& p, const std::function<bool(int, int, int, int)>& start, const std::function<bool()>& handedOver,
         const std::function<void()>& openGamesFolder)
{
    bool shown = false, started = false;
    double startedAt = 0;
    for (;;) {
        if (WindowShouldClose()) return false;
        const Snapshot s = Take(p);
        const double now = GetTime();
        if (s.state == Progress::Ready && !started && shown) {
            started = true;
            startedAt = now;
            const Vector2 pos = GetWindowPosition();
            if (!start((int)pos.x, (int)pos.y, GetScreenWidth(), GetScreenHeight())) {
                std::lock_guard<std::mutex> g(p.lock);
                p.state = Progress::Failed;
                p.stage = "The game did not start.";
            }
        }
        if (started && s.state == Progress::Ready && (handedOver() || now - startedAt > 20)) return true;

        const int sw = GetScreenWidth(), sh = GetScreenHeight();
        LoadFonts(sh);
        const float mx = sw * 0.05f;
        const float lineBig = g_big.baseSize * 1.25f, lineSmall = g_small.baseSize * 1.35f;
        const bool failed = s.state == Progress::Failed;

        // The panel: a band along the bottom, taller to list what went wrong.
        int listed = 0;
        float panelH = sh * 0.19f;
        if (failed) {
            listed = (int)std::min<size_t>(s.lines.size(), 14);
            panelH = std::max(panelH, mx * 0.5f + lineBig + lineSmall * (listed + 2) + mx * 0.4f);
            panelH = std::min(panelH, sh * 0.72f);
            listed = std::max(0, std::min(listed, (int)((panelH - mx * 0.9f - lineBig) / lineSmall) - 2));
        }
        const float py = sh - panelH;
        Rectangle button = {};
        if (s.needGames) {
            const char* label = "Open the games folder";
            const Vector2 m = MeasureTextEx(g_small, label, (float)g_small.baseSize, 0);
            button = {sw - mx - m.x - g_small.baseSize * 1.6f, py + mx * 0.32f, m.x + g_small.baseSize * 1.6f, m.y + g_small.baseSize * 0.9f};
            const bool hover = CheckCollisionPointRec(GetMousePosition(), button);
            SetMouseCursor(hover ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_DEFAULT);
            if ((hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) || IsKeyPressed(KEY_G)) openGamesFolder();
        }

        BeginDrawing();
        ClearBackground(BLACK);
        DrawCover(g_backdrop);
        DrawRectangleRec({0, py, (float)sw, panelH}, kPanel);
        DrawRectangleRec({0, py, (float)sw, std::max(2.0f, sh * 0.004f)}, failed ? kPink : kBlue);
        float y = py + mx * 0.32f;
        const float textW = (s.needGames ? button.x - mx * 0.5f : sw - mx) - mx;
        Text(g_big, Fit(g_big, s.stage, textW), mx, y, failed ? Color{255, 120, 160, 255} : kText);
        y += lineBig;
        if (!failed) {
            Text(g_small, Fit(g_small, s.detail, sw - 2 * mx), mx, y, kDim);
            // The bar: filled as far as it is known, else a moving stripe.
            const float by = sh - mx * 0.55f - sh * 0.012f, bh = std::max(4.0f, sh * 0.012f), bw = sw - 2 * mx;
            DrawRectangleRounded({mx, by, bw, bh}, 1, 8, Color{255, 255, 255, 38});
            if (s.state == Progress::Ready) {
                DrawRectangleRounded({mx, by, bw, bh}, 1, 8, kPink);
            } else if (s.fraction >= 0) {
                DrawRectangleRounded({mx, by, std::max(bh, bw * std::min(1.0f, s.fraction)), bh}, 1, 8, kPink);
            } else {
                const float seg = bw * 0.22f;
                const float t = (float)std::fmod(now * 0.55, 1.0);
                const float x0 = mx + (bw + seg) * t - seg;
                const float a = std::max(mx, x0), b = std::min(mx + bw, x0 + seg);
                if (b > a) DrawRectangleRounded({a, by, b - a, bh}, 1, 8, kBlue);
            }
        } else {
            const size_t from = s.lines.size() - listed;
            for (size_t i = from; i < s.lines.size(); ++i) {
                Text(g_small, Fit(g_small, s.lines[i], sw - 2 * mx), mx, y, kDim);
                y += lineSmall;
            }
            y += lineSmall * 0.4f;
            if (!s.needGames)
                Text(g_small, "The details are in canopus-setup.log, next to the program. Close this window when you are done.", mx, y, kText);
        }
        if (s.needGames) {
            const bool hover = CheckCollisionPointRec(GetMousePosition(), button);
            DrawRectangleRounded(button, 0.3f, 8, hover ? Color{246, 60, 132, 255} : kPink);
            const char* label = "Open the games folder";
            const Vector2 m = MeasureTextEx(g_small, label, (float)g_small.baseSize, 0);
            Text(g_small, label, button.x + (button.width - m.x) / 2, button.y + (button.height - m.y) / 2, WHITE);
        }
        EndDrawing();
        if (!shown) {
            ClearWindowState(FLAG_WINDOW_HIDDEN);
            shown = true;
        }
    }
}

}  // namespace ui
