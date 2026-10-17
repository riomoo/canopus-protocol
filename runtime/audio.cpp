// The ring buffer between the DSP and the sound backend, and the
// resampling from the DSP's rate to the output's.
#include "audio.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace rt {
namespace audio {

namespace {

std::mutex s_lock;
// Stereo frames at kDspRate waiting to be played.
std::vector<s16> s_ring(2 * kDspRate);  // 1 s
size_t s_head = 0, s_count = 0;         // in frames
double s_phase = 0;                     // position between the two frames being blended
s16 s_prev[2] = {0, 0};
bool s_open = false;
// Problems since the last report (diagnostics): output frames played with
// nothing queued, and queued frames dropped because the output fell behind.
u64 s_starved = 0, s_dropped = 0, s_pulled = 0;
bool s_started = false;

// The latency we aim for: enough to ride out scheduling hiccups.
constexpr size_t kTarget = kDspRate * 60 / 1000;  // 60 ms
constexpr size_t kMax = kDspRate * 200 / 1000;    // drop beyond 200 ms

}  // namespace

bool Open()
{
    s_open = OpenBackend();
    Log(LOG_INFO, "audio", s_open ? "sound on (%d Hz)" : "no sound output on this system", kOutRate);
    return s_open;
}

// DS2RB_WAV=<file>: also writes the mixed sound to a WAV file (diagnostics).
FILE* s_wav = nullptr;
u32 s_wavFrames = 0;

void WavHeader()
{
    const u32 bytes = s_wavFrames * 4;
    u8 h[44];
    auto w32 = [&](int o, u32 v) { std::memcpy(h + o, &v, 4); };
    auto w16 = [&](int o, u16 v) { std::memcpy(h + o, &v, 2); };
    std::memcpy(h, "RIFF", 4);
    w32(4, 36 + bytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, 1);
    w16(22, 2);
    w32(24, kDspRate);
    w32(28, kDspRate * 4);
    w16(32, 4);
    w16(34, 16);
    std::memcpy(h + 36, "data", 4);
    w32(40, bytes);
    std::fseek(s_wav, 0, SEEK_SET);
    std::fwrite(h, 1, 44, s_wav);
    std::fseek(s_wav, 0, SEEK_END);
}

void Push(const s16* stereo, int frames)
{
    static const char* wavPath = std::getenv("DS2RB_WAV");
    if (wavPath) {
        std::lock_guard<std::mutex> g(s_lock);
        if (!s_wav && (s_wav = std::fopen(wavPath, "wb"))) WavHeader();
        if (s_wav) {
            std::fwrite(stereo, 4, frames, s_wav);
            s_wavFrames += frames;
            if (s_wavFrames % (kDspRate / 2) < (u32)frames) {  // keep the header current
                WavHeader();
                std::fflush(s_wav);
            }
        }
    }
    if (!s_open) return;
    std::lock_guard<std::mutex> g(s_lock);
    s_started = true;
    const size_t cap = s_ring.size() / 2;
    for (int i = 0; i < frames; ++i) {
        if (s_count >= kMax) {  // the output fell behind: drop the oldest
            s_head = (s_head + 1) % cap;
            --s_count;
            ++s_dropped;
        }
        const size_t at = (s_head + s_count) % cap;
        s_ring[2 * at] = stereo[2 * i];
        s_ring[2 * at + 1] = stereo[2 * i + 1];
        ++s_count;
    }
}

void Pull(s16* out, int frames)
{
    std::unique_lock<std::mutex> g(s_lock);
    s_pulled += frames;
    if (s_pulled >= (u64)kOutRate * 5) {  // every 5 s of sound: report trouble, if any
        const u64 starved = s_starved, dropped = s_dropped;
        s_pulled = s_starved = s_dropped = 0;
        if (starved || dropped) {
            g.unlock();
            Log(LOG_INFO, "audio", "in the last 5 s the sound ran out for %.0f ms and %.0f ms of it was dropped",
                starved * 1000.0 / kDspRate, dropped * 1000.0 / kDspRate);
            g.lock();
        }
    }
    const size_t cap = s_ring.size() / 2;
    // Nudge the rate by up to 0.5% to keep the queue near its target.
    const double drift = std::clamp(((double)s_count - (double)kTarget) / (double)kTarget, -1.0, 1.0) * 0.005;
    const double step = (double)kDspRate / kOutRate * (1.0 + drift);
    for (int i = 0; i < frames; ++i) {
        s16 next[2] = {s_prev[0], s_prev[1]};
        if (s_count) {
            next[0] = s_ring[2 * s_head];
            next[1] = s_ring[2 * s_head + 1];
        }
        out[2 * i] = (s16)(s_prev[0] + (next[0] - s_prev[0]) * s_phase);
        out[2 * i + 1] = (s16)(s_prev[1] + (next[1] - s_prev[1]) * s_phase);
        s_phase += step;
        while (s_phase >= 1.0) {
            s_phase -= 1.0;
            s_prev[0] = next[0];
            s_prev[1] = next[1];
            if (s_count) {
                s_head = (s_head + 1) % cap;
                --s_count;
                next[0] = s_count ? s_ring[2 * s_head] : s_prev[0];
                next[1] = s_count ? s_ring[2 * s_head + 1] : s_prev[1];
            } else {
                if (s_started) ++s_starved;
                // Ran dry: fade towards silence instead of clicking.
                s_prev[0] = (s16)(s_prev[0] * 15 / 16);
                s_prev[1] = (s16)(s_prev[1] * 15 / 16);
            }
        }
    }
}

}  // namespace audio
}  // namespace rt
