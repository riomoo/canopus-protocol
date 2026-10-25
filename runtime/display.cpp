// The screens: shown in the host window when there is one (window.h), and
// optionally written to image files (DS2RB_DUMP=<every n frames>, into
// ./frames).
#include "display.h"
#include "cheats.h"
#include "gpu.h"
#include "kernel.h"
#include "window.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace rt {
namespace display {

namespace {
struct Screen {
    u32 addr = 0, stride = 0, format = 0;
};
Screen s_screens[2];
std::mutex s_lock;
u64 s_frame = 0;
std::atomic<u64> s_swaps{0};  // top screen buffer flips: the game's frame rate

void WritePpm(const std::string& path, const std::vector<u32>& px, int w, int h)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (u32 p : px) {
        const unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}
}  // namespace

void SetFramebuffer(int screen, u32 addr, u32 stride, u32 format)
{
    if (screen < 0 || screen > 1) return;
    std::lock_guard<std::mutex> g(s_lock);
    if (s_screens[screen].addr != addr && screen == TOP) {
        ++s_swaps;
        // Hitches: a long wait for a new picture right after smooth motion.
        static auto last = std::chrono::steady_clock::now();
        static int smooth = 0;
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        if (ms > 120 && smooth >= 10) Log(LOG_INFO, "display", "hitch: no new picture for %.0f ms (frame %llu)", ms, (unsigned long long)s_frame);
        smooth = ms < 70 ? smooth + 1 : 0;
    }
    if (s_screens[screen].addr != addr)
        Log(LOG_DEBUG, "display", "%s screen framebuffer %08X stride %X format %u", screen ? "bottom" : "top", addr, stride, format);
    s_screens[screen] = {addr, stride, format};
}

bool Capture(int screen, u32* out, int* w, int* h)
{
    Screen s;
    {
        std::lock_guard<std::mutex> g(s_lock);
        s = s_screens[screen];
    }
    *w = screen == TOP ? 400 : 320;
    *h = 240;
    if (!s.addr || !mem::IsMapped(s.addr, 1)) return false;
    // Framebuffers are stored rotated: each 240-pixel line is a screen
    // column, from the bottom of the screen up.
    const u32 bpp = gpu::BytesPerPixel(s.format);
    for (int x = 0; x < *w; ++x)
        for (int y = 0; y < 240; ++y)
            out[y * *w + x] = gpu::ReadPixel(mem::Ptr(s.addr + x * s.stride + (239 - y) * bpp), s.format);
    return true;
}

bool s_window = false;

void OpenWindow(const char* title) { s_window = window::Open(title); }

void Present()
{
    ++s_frame;
    // The picture stopped (the game is stuck): log what every thread is doing, once.
    {
        static u64 lastSwaps = 0, still = 0;
        static bool reported = false;
        const u64 swaps = s_swaps;
        if (swaps != lastSwaps) {
            lastSwaps = swaps;
            still = 0;
            reported = false;
        } else if (++still == 300 && swaps > 100 && !reported) {
            reported = true;
            Log(LOG_INFO, "display", "no new picture for 5 s (frame %llu); the threads:", (unsigned long long)s_frame);
            kernel::LogStatus();
        }
    }
    if (s_window) {
        static std::vector<u32> top(400 * 240), bottom(320 * 240);
        int w, h;
        bool t = Capture(TOP, top.data(), &w, &h);
        const bool b = Capture(BOTTOM, bottom.data(), &w, &h);
        if (cheat::Open() || !t) {
            if (!t) std::fill(top.begin(), top.end(), 0xFF000000u);
            cheat::Frame(top.data());
            t = t || cheat::Open();
        } else {
            cheat::Frame(nullptr);  // a pending F1
        }
        window::Show(t ? top.data() : nullptr, b ? bottom.data() : nullptr);
        // Once a second: the game's frame rate and how busy the GPU is.
        static auto last = std::chrono::steady_clock::now();
        static u64 lastSwaps = 0;
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - last).count();
        if (secs >= 1.0) {
            const u64 swaps = s_swaps;
            const s64 busy = g_gpuBusyNs.exchange(0);
            char text[96];
            std::snprintf(text, sizeof text, "%.0f fps, GPU %.0f%%", (swaps - lastSwaps) / secs, busy / (secs * 1e7));
            window::SetStatus(text);
            // Every 10 s, the same in the log: how a player's machine keeps up.
            static int seconds = 0;
            static double fpsSum = 0, gpuSum = 0;
            fpsSum += (swaps - lastSwaps) / secs;
            gpuSum += busy / (secs * 1e7);
            if (++seconds == 10) {
                Log(LOG_INFO, "perf", "%.0f fps, GPU %.0f%% (frame %llu)", fpsSum / 10, gpuSum / 10, (unsigned long long)s_frame);
                seconds = 0;
                fpsSum = gpuSum = 0;
            }
            last = now;
            lastSwaps = swaps;
        }
    }
    if (!s_window) cheat::Frame(nullptr);  // scripted cheat keys (tests)
    static const long every = std::getenv("DS2RB_DUMP") ? std::atol(std::getenv("DS2RB_DUMP")) : 0;
    if (every <= 0 || s_frame % every) return;
    std::filesystem::create_directories("frames");
    for (int screen = 0; screen < 2; ++screen) {
        int w, h;
        std::vector<u32> px(400 * 240);
        if (!Capture(screen, px.data(), &w, &h)) continue;
        px.resize(w * h);
        if (screen == TOP && cheat::Open()) cheat::Frame(px.data());
        char name[64];
        std::snprintf(name, sizeof name, "frames/%s_%06llu.ppm", screen ? "bottom" : "top", (unsigned long long)s_frame);
        WritePpm(name, px, w, h);
    }
}

// DS2RB_INPUT: scripted presses for testing without a player, items split
// by ';': "<frame>[-<last>/<every>]:<BUTTON>[+<BUTTON>...][:<hold frames>]",
// buttons A B X Y L R START SELECT UP DOWN LEFT RIGHT or TOUCH@<x>x<y>.
// Example: "2000:START;2600:UP;2700-40000/90:A".
struct Press {
    u64 first, last, every, hold;
    u32 buttons;
    bool touch;
    u16 tx, ty;
    std::vector<int> cheatKeys;  // F1, CUP, CDOWN, CLEFT, CRIGHT, CENTER, CBACK, CPGUP, CPGDN, "C=text"
};

std::vector<Press> ParseScript(const char* text)
{
    std::vector<Press> out;
    std::string all = text;
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find(';', at);
        if (end == std::string::npos) end = all.size();
        const std::string item = all.substr(at, end - at);
        at = end + 1;
        Press p = {0, 0, 1, 6, 0, false, 0, 0, {}};
        const size_t c1 = item.find(':');
        if (c1 == std::string::npos) continue;
        const std::string when = item.substr(0, c1);
        std::string what = item.substr(c1 + 1);
        const size_t c2 = what.find(':');
        if (c2 != std::string::npos) {
            p.hold = std::strtoull(what.c_str() + c2 + 1, nullptr, 10);
            what = what.substr(0, c2);
        }
        p.first = p.last = std::strtoull(when.c_str(), nullptr, 10);
        const size_t dash = when.find('-'), slash = when.find('/');
        if (dash != std::string::npos) p.last = std::strtoull(when.c_str() + dash + 1, nullptr, 10);
        if (slash != std::string::npos) p.every = std::max<u64>(1, std::strtoull(when.c_str() + slash + 1, nullptr, 10));
        size_t b = 0;
        while (b <= what.size()) {
            size_t plus = what.find('+', b);
            if (plus == std::string::npos) plus = what.size();
            const std::string name = what.substr(b, plus - b);
            b = plus + 1;
            static const struct { const char* n; u32 v; } kNames[] = {
                {"A", input::A}, {"B", input::B}, {"X", input::X}, {"Y", input::Y}, {"L", input::L}, {"R", input::R},
                {"START", input::START}, {"SELECT", input::SELECT}, {"UP", input::UP}, {"DOWN", input::DOWN},
                {"LEFT", input::LEFT}, {"RIGHT", input::RIGHT}};
            for (auto& k : kNames)
                if (name == k.n) p.buttons |= k.v;
            static const struct { const char* n; int k; } kCheat[] = {
                {"F1", cheat::KEY_TOGGLE}, {"CUP", cheat::KEY_UP}, {"CDOWN", cheat::KEY_DOWN}, {"CLEFT", cheat::KEY_LEFT},
                {"CRIGHT", cheat::KEY_RIGHT}, {"CENTER", cheat::KEY_ENTER}, {"CBACK", cheat::KEY_BACK},
                {"CPGUP", cheat::KEY_PAGE_UP}, {"CPGDN", cheat::KEY_PAGE_DOWN}};
            for (auto& k : kCheat)
                if (name == k.n) p.cheatKeys.push_back(k.k);
            if (name.rfind("C=", 0) == 0)
                for (size_t i = 2; i < name.size(); ++i) p.cheatKeys.push_back(name[i] == '_' ? ' ' : name[i]);
            if (name.rfind("TOUCH@", 0) == 0) {
                p.touch = true;
                p.tx = (u16)std::atoi(name.c_str() + 6);
                const size_t x = name.find('x', 6);
                if (x != std::string::npos) p.ty = (u16)std::atoi(name.c_str() + x + 1);
            }
        }
        out.push_back(p);
    }
    return out;
}

input::State PollInput()
{
    input::State st = s_window ? window::Input() : input::State{};
    static const std::vector<Press> script = std::getenv("DS2RB_INPUT") ? ParseScript(std::getenv("DS2RB_INPUT")) : std::vector<Press>{};
    for (const Press& p : script) {
        if (s_frame < p.first || s_frame > p.last + p.hold) continue;
        const u64 phase = (s_frame - p.first) % p.every;
        if (phase == 0 && s_frame <= p.last)
            for (int k : p.cheatKeys) cheat::PushKey(k);
        if (phase >= p.hold) continue;
        st.buttons |= p.buttons;
        if (p.touch) {
            st.touch = true;
            st.touchX = p.tx;
            st.touchY = p.ty;
        }
    }
    return st;
}

}  // namespace display
}  // namespace rt
