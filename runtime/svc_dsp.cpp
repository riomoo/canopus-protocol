// dsp::DSP: the audio processor, done in C++ (high level): the game's voice
// configurations in DSP memory are read every audio frame, the voices are
// decoded (PCM8, PCM16, ADPCM), resampled and mixed, and the result goes to
// the host's sound output (audio.h).
#include "audio.h"
#include "gpu.h"
#include "ipc.h"

#include <algorithm>
#include <cmath>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace rt {
namespace ipc {

namespace {

// Where the DSP firmware keeps its shared structures (DSP word addresses
// in region 0; region 1 is the same + 0x10000): frame counter, source
// configurations, source statuses, ADPCM coefficients, DSP configuration,
// DSP status, final samples, intermediate mix samples, compressor, debug,
// and five more.
const u16 kStructs[15] = {0xBFFF, 0x9E92, 0x8680, 0xA792, 0x9430, 0x8400, 0x8540, 0x9492,
                          0x8710, 0x8410, 0xA912, 0xAA12, 0xAAD2, 0xAC52, 0xAC5C};

u32 DspToVirt(u32 dspAddr) { return mem::DSP_RAM + 0x40000 + dspAddr * 2; }

// 32-bit values in DSP memory are two 16-bit words, high word first.
u32 R32Dsp(u32 va) { return (u32)mem::R16(va) << 16 | mem::R16(va + 2); }
void W32Dsp(u32 va, u32 v) { mem::W16(va, (u16)(v >> 16)); mem::W16(va + 2, (u16)v); }

constexpr u32 kSources = 24;
constexpr u32 kConfigSize = 192, kStatusSize = 12;
constexpr u32 kSamplesPerFrame = 160;

// A voice: the buffers the game queued for it, decoded one at a time as it
// plays them, resampled to the output rate and mixed in with its gains.
struct Voice {
    enum Format { PCM8 = 0, PCM16 = 1, ADPCM = 2 };
    struct Buffer {
        u32 addr = 0, length = 0, playPosition = 0;  // physical address; length in samples
        u16 id = 0;
        u8 format = PCM16, channels = 1;
        bool looping = false, fromQueue = false, played = false;
        bool adpcmDirty = false;
        u16 adpcmPs = 0;
        s16 yn1 = 0, yn2 = 0;
    };
    bool enabled = false;
    u16 syncCount = 0;
    float rate = 1.0f;
    u8 format = PCM16, channels = 1, interpolation = 0;
    float gain[3][4] = {{1, 1, 0, 0}, {}, {}};
    s16 coeff[16] = {};
    s16 yn1 = 0, yn2 = 0;  // ADPCM history, carried from buffer to buffer
    std::vector<Buffer> queue;  // kept ordered by buffer id
    bool playing = false;       // a buffer is current
    std::vector<s16> samples;   // the current buffer, decoded (interleaved when stereo)
    u8 curChannels = 1;
    u32 length = 0;
    double position = 0;        // samples into the current buffer
    // Resampling: the latest input samples, oldest first, and how many of
    // the current buffer's have gone in. Polyphase (interpolation mode 0,
    // what games ask for) is a windowed-sinc low-pass that follows the rate,
    // so bright sound played at a lower rate does not fold back as noise.
    static constexpr int kTaps = 16, kPhases = 64;
    float hist[2][kTaps] = {};
    u32 fed = 0;
    float tableRate = 0;
    std::vector<float> table;  // (kPhases + 1) rows of kTaps weights
    u16 currentId = 0, lastId = 0;
    bool idChanged = false;
    bool startedSinceEnable = false;  // a buffer has played since the game enabled the voice

    void Reset() { *this = Voice{}; }

    void Push(const Buffer& b)
    {
        auto it = queue.begin();
        while (it != queue.end() && it->id != b.id && (u16)(b.id - it->id) < 0x8000) ++it;  // ids before b's
        queue.insert(it, b);
    }

    static float F32(u32 va)
    {
        const u32 x = mem::R32(va);
        float f;
        std::memcpy(&f, &x, 4);
        return f;
    }

    // Reads the configuration the game wrote (at va; ADPCM coefficients at
    // coeffVa) and clears its dirty flags.
    void Configure(u32 va, u32 coeffVa)
    {
        const u32 dirty = mem::R32(va);
        if (!dirty) return;
        if (dirty >> 29 & 1) Reset();                          // reset
        if (dirty >> 4 & 1) { queue.clear(); playing = false; }  // partial reset
        if (dirty >> 16 & 1) {
            const bool on = mem::R8(va + 160) != 0;
            if (on && !enabled) startedSinceEnable = false;
            enabled = on;
        }
        if (dirty >> 28 & 1) syncCount = mem::R16(va + 162);
        if (dirty >> 18 & 1) {
            const float r = F32(va + 52);
            rate = (r > 0 && r < 64) ? r : 1.0f;
        }
        if (dirty >> 17 & 1) interpolation = mem::R8(va + 56);
        const u16 flags1 = mem::R16(va + 180);
        if (dirty & 1) format = (flags1 >> 2) & 3;
        if (dirty >> 1 & 1) channels = (flags1 & 3) == 2 ? 2 : 1;
        if (dirty >> 2 & 1)
            for (int i = 0; i < 16; ++i) coeff[i] = (s16)mem::R16(coeffVa + 2 * i);
        for (int m = 0; m < 3; ++m)
            if (dirty >> (25 + m) & 1)
                for (int ch = 0; ch < 4; ++ch) gain[m][ch] = F32(va + 4 + m * 16 + ch * 4);
        if (dirty >> 30 & 1) {  // the embedded buffer, which also sets the format
            format = (flags1 >> 2) & 3;
            channels = (flags1 & 3) == 2 ? 2 : 1;
            Buffer b;
            b.addr = R32Dsp(va + 172);
            b.length = R32Dsp(va + 176);
            b.playPosition = R32Dsp(va + 164);
            const u16 flags2 = mem::R16(va + 188);
            b.looping = flags2 >> 1 & 1;
            b.adpcmDirty = flags2 & 1;
            b.adpcmPs = mem::R16(va + 182);
            b.yn1 = (s16)mem::R16(va + 184);
            b.yn2 = (s16)mem::R16(va + 186);
            b.id = mem::R16(va + 190);
            b.format = format;
            b.channels = channels;
            Push(b);
        }
        if (dirty >> 19 & 1) {  // the buffer queue
            const u16 which = mem::R16(va + 74);
            for (u32 i = 0; i < 4; ++i) {
                if (!(which >> i & 1)) continue;
                const u32 q = va + 76 + i * 20;
                Buffer b;
                b.addr = R32Dsp(q);
                b.length = R32Dsp(q + 4);
                b.adpcmPs = mem::R8(q + 8);
                b.yn1 = (s16)mem::R16(q + 10);
                b.yn2 = (s16)mem::R16(q + 12);
                b.adpcmDirty = mem::R8(q + 14) != 0;
                b.looping = mem::R8(q + 15) != 0;
                b.id = mem::R16(q + 16);
                b.format = format;
                b.channels = channels;
                b.fromQueue = true;
                if (std::getenv("DS2RB_DSPLOG")) Log(LOG_INFO, "dsp", "queue id %u len %u fmt %u ch %u at %08X (queued %zu, playing %u pos %.0f/%u)", b.id, b.length, b.format, b.channels, b.addr, queue.size(), currentId, position, length);
                Push(b);
            }
            mem::W16(va + 74, 0);
        }
        mem::W32(va, 0);
    }

    // Decodes a whole buffer into `samples`.
    void Decode(const Buffer& b)
    {
        curChannels = b.channels;
        samples.assign((size_t)b.length * b.channels, 0);
        const u32 va = gpu::PhysToVirt(b.addr);
        if (!va || !b.length) return;
        switch (b.format) {
        case PCM8:
            if (!mem::IsMapped(va, b.length * b.channels)) return;
            for (size_t i = 0; i < samples.size(); ++i) samples[i] = (s16)((int8_t)mem::R8(va + (u32)i) << 8);
            break;
        case PCM16:
            if (!mem::IsMapped(va, b.length * b.channels * 2)) return;
            std::memcpy(samples.data(), mem::Ptr(va), samples.size() * 2);
            break;
        case ADPCM: {  // mono: 8-byte frames of a header and 14 nibbles
            curChannels = 1;
            samples.assign(b.length, 0);
            const u32 frames = (b.length + 13) / 14;
            if (!mem::IsMapped(va, frames * 8)) return;
            if (b.adpcmDirty) {
                yn1 = b.yn1;
                yn2 = b.yn2;
            }
            for (u32 n = 0; n < b.length; ++n) {
                const u32 frame = va + (n / 14) * 8;
                const u8 header = mem::R8(frame);
                const int scale = 1 << (header & 0xF), idx = (header >> 4) & 7;
                const int k = n % 14;
                const u8 byte = mem::R8(frame + 1 + k / 2);
                int nibble = (k & 1) ? (byte & 0xF) : (byte >> 4);
                if (nibble >= 8) nibble -= 16;
                int v = ((nibble * scale) << 11) + 0x400 + coeff[idx * 2] * yn1 + coeff[idx * 2 + 1] * yn2;
                v >>= 11;
                v = std::clamp(v, -32768, 32767);
                yn2 = yn1;
                yn1 = (s16)v;
                samples[n] = (s16)v;
            }
            break;
        }
        default:
            break;
        }
    }

    bool Next()
    {
        if (queue.empty()) return false;
        Buffer b = queue.front();
        queue.erase(queue.begin());
        lastId = currentId;
        position = b.played ? 0 : b.playPosition;
        length = b.length;
        fed = (u32)position;
        currentId = b.id;
        idChanged = b.fromQueue && !b.played;
        playing = true;
        startedSinceEnable = true;
        Decode(b);
        // DS2RB_DSPDUMP=<file>: the stereo buffers as the game queued them (diagnostics).
        static FILE* dump = std::getenv("DS2RB_DSPDUMP") ? std::fopen(std::getenv("DS2RB_DSPDUMP"), "wb") : nullptr;
        if (dump && curChannels == 2 && !b.played) {
            std::fwrite(samples.data(), 2, samples.size(), dump);
            std::fflush(dump);
        }
        if (b.looping) {
            b.played = true;
            Push(b);
        }
        return true;
    }

    s16 SampleAt(u32 i, int ch) const
    {
        if (i >= length) return 0;
        return curChannels == 2 ? samples[2 * i + ch] : samples[i];
    }

    void BuildTable()
    {
        tableRate = rate;
        table.assign((kPhases + 1) * kTaps, 0.0f);
        const double fc = 0.5 * std::min(1.0, 1.0 / rate) * 0.92;  // cutoff, cycles per input sample
        const double pi = 3.14159265358979323846;
        for (int p = 0; p <= kPhases; ++p) {
            const double frac = (double)p / kPhases;
            double sum = 0;
            for (int k = 0; k < kTaps; ++k) {
                const double x = frac - kTaps / 2 + (kTaps - 1 - k);  // distance from sample k
                const double sinc = x == 0 ? 1.0 : std::sin(2 * pi * fc * x) / (2 * pi * fc * x);
                const double u = x / (kTaps / 2);  // Blackman window over the taps
                const double win = std::fabs(u) >= 1 ? 0 : 0.42 + 0.5 * std::cos(pi * u) + 0.08 * std::cos(2 * pi * u);
                const double w = 2 * fc * sinc * win;
                table[p * kTaps + k] = (float)w;
                sum += w;
            }
            for (int k = 0; k < kTaps; ++k) table[p * kTaps + k] = (float)(table[p * kTaps + k] / sum);
        }
    }

    void Feed(float l, float r)
    {
        std::memmove(hist[0], hist[0] + 1, (kTaps - 1) * sizeof(float));
        std::memmove(hist[1], hist[1] + 1, (kTaps - 1) * sizeof(float));
        hist[0][kTaps - 1] = l;
        hist[1][kTaps - 1] = r;
    }

    // One audio frame: 160 output samples, `rate` input samples each, added
    // into mix (stereo).
    void Tick(float (*mix)[2])
    {
        if (!enabled) return;
        // A voice with nothing left to play stops itself: games watch for
        // this to know a sound has ended.
        if (!playing && !startedSinceEnable && queue.empty()) return;  // just enabled, buffers still to come
        if (!playing && !Next()) {
            if (channels == 2)  // streams (movies): the game fed it too late
                Log(LOG_INFO, "dsp", "stereo voice ran dry after buffer %u", currentId);
            enabled = false;
            idChanged = true;
            currentId = 0;
            return;
        }
        if (interpolation == 0 && tableRate != rate) BuildTable();
        const float gl = gain[0][0] + gain[0][2], gr = gain[0][1] + gain[0][3];
        for (u32 n = 0; n < kSamplesPerFrame; ++n) {
            if (!playing && !Next()) break;
            const u32 i = (u32)position;
            while (fed <= i && fed < length) {
                Feed(SampleAt(fed, 0), SampleAt(fed, 1));
                ++fed;
            }
            const float frac = (float)(position - i);
            float l, r;
            if (interpolation == 2) {  // none
                l = hist[0][kTaps - 1];
                r = hist[1][kTaps - 1];
            } else if (interpolation == 1) {  // linear
                l = hist[0][kTaps - 2] + (hist[0][kTaps - 1] - hist[0][kTaps - 2]) * frac;
                r = hist[1][kTaps - 2] + (hist[1][kTaps - 1] - hist[1][kTaps - 2]) * frac;
            } else {  // polyphase
                const float* w = &table[(size_t)(frac * kPhases + 0.5f) * kTaps];
                l = r = 0;
                for (int k = 0; k < kTaps; ++k) {
                    l += hist[0][k] * w[k];
                    r += hist[1][k] * w[k];
                }
            }
            mix[n][0] += l * gl;
            mix[n][1] += r * gr;
            position += rate;
            if (position >= length) {
                const double over = position - length;
                playing = false;
                if (Next()) position += over;
            }
        }
    }

    void Status(u32 va)
    {
        mem::W8(va, enabled);
        mem::W8(va + 1, idChanged);
        idChanged = false;
        mem::W16(va + 2, syncCount);
        W32Dsp(va + 4, (u32)std::min<double>(position, length));
        mem::W16(va + 8, currentId);
        mem::W16(va + 10, lastId);
    }
};

struct Dsp : Service {
    Dsp() : Service("dsp::DSP")
    {
        semaphore->name = "DSP semaphore event";
        semaphore->reset = kernel::ONESHOT;
    }
    std::shared_ptr<kernel::Event> semaphore = std::make_shared<kernel::Event>();
    std::map<u32, std::shared_ptr<kernel::Event>> interrupts;  // (interrupt << 8 | channel)
    std::map<u32, std::deque<u8>> pipes;  // data the DSP has for the game, by channel
    std::atomic<bool> running{false};
    Voice voices[kSources];

    // The handshake, from the game's sound frame handler (0x2A6E44): the
    // game fills one region with voice configurations and stamps it with
    // frame counter c. The DSP takes the region with the higher counter,
    // writes the voice statuses into the other one and copies the counter
    // there, which is where the game looks for it before starting on c+1.
    static u32 ReadRegion()
    {
        const u16 a = mem::R16(DspToVirt(kStructs[0])), b = mem::R16(DspToVirt(kStructs[0] + 0x10000));
        if (a == 0xFFFF && b != 0xFFFE) return 0x10000;
        if (b == 0xFFFF && a != 0xFFFE) return 0;
        return a > b ? 0 : 0x10000;
    }

    void AudioFrame()
    {
        const u32 rd = ReadRegion(), wr = rd ^ 0x10000;
        static u32 n = 0;
        if (n++ % 2000 == 0)
            Log(LOG_DEBUG, "dsp", "audio frame %u: counters %04X %04X", n, mem::R16(DspToVirt(kStructs[0])),
                mem::R16(DspToVirt(kStructs[0] + 0x10000)));
        float mix[kSamplesPerFrame][2] = {};
        for (u32 i = 0; i < kSources; ++i) {
            Voice& v = voices[i];
            v.Configure(DspToVirt(kStructs[1] + rd) + i * kConfigSize, DspToVirt(kStructs[3] + rd) + i * 32);
            v.Tick(mix);
            v.Status(DspToVirt(kStructs[2] + wr) + i * kStatusSize);
        }
        s16 out[kSamplesPerFrame * 2];
        for (u32 n = 0; n < kSamplesPerFrame; ++n)
            for (int ch = 0; ch < 2; ++ch) out[2 * n + ch] = (s16)std::clamp(mix[n][ch], -32768.0f, 32767.0f);
        audio::Push(out, kSamplesPerFrame);
        mem::W16(DspToVirt(kStructs[0] + wr), mem::R16(DspToVirt(kStructs[0] + rd)));
    }

    // Signals the events registered for a pipe channel (interrupt type 2).
    void RaisePipe(u32 channel, std::unique_lock<std::mutex>& lk)
    {
        for (auto& [key, e] : interrupts)
            if ((key & 0xFF) == channel && e) {
                e->signaled = true;
                kernel::Signal(e.get(), lk);
            }
    }

    // An audio frame every 160 samples at 32728 Hz (4.89 ms): the voices
    // take the game's latest configuration, play on, report where they are,
    // and the audio pipe interrupt tells the game.
    //
    // The real DSP never runs frames back to back. When this thread is late
    // (the host was busy), it catches up one frame at a time, giving the
    // game a moment to answer each one first: frames in a burst would play
    // through a movie's queued sound before the game could add more, and
    // its player, which follows the sound, would stall. Far behind, the
    // lost time is dropped instead.
    u16 GameCounter() const
    {
        const u16 a = mem::R16(DspToVirt(kStructs[0])), b = mem::R16(DspToVirt(kStructs[0] + 0x10000));
        return (u16)(a == 0xFFFF ? b : b == 0xFFFF ? a : std::max<u16>(a, b));
    }

    void StartFrames()
    {
        if (running.exchange(true)) return;
        std::thread([this] {
            using clock = std::chrono::steady_clock;
            SetThreadRole(ThreadRole::Timing);
            auto next = clock::now();
            u16 answered = GameCounter();
            u32 late = 0, dropped = 0, frames = 0;
            while (running) {
                const auto period = std::chrono::nanoseconds((s64)(4889000 / g_speed));
                next += period;
                auto now = clock::now();
                if (now < next) {
                    SleepUntil(next);
                } else if (now > next + 20 * period) {
                    dropped += (u32)((now - next) / period);
                    next = now;  // too far behind: skip, do not burst
                } else {
                    ++late;
                    // Give the game up to half a frame to answer the last one.
                    const auto until = now + period / 2;
                    while (GameCounter() == answered && clock::now() < until) std::this_thread::yield();
                }
                {
                    auto lk = kernel::Lock();
                    AudioFrame();
                    RaisePipe(2, lk);
                }
                answered = GameCounter();
                if (++frames >= 2045) {  // about every 10 s
                    if (late > 20 || dropped)
                        Log(LOG_INFO, "dsp", "in the last 10 s %u sound frames ran late and %u were skipped", late, dropped);
                    frames = late = dropped = 0;
                }
            }
        }).detach();
    }

    void Handle(Request& r) override
    {
        auto& c = r.cmd;
        switch (r.id()) {
        case 0x0001:  // RecvData(register) -> 1: the DSP is running
            r.Reply(2, 0);
            c[2] = 1;
            return;
        case 0x0002:  // RecvDataIsReady
            r.Reply(2, 0);
            c[2] = 1;
            return;
        case 0x0007:  // SetSemaphore
        case 0x0013:  // FlushDataCache
        case 0x0014:  // InvalidateDataCache
        case 0x0017:  // SetSemaphoreMask
        case 0x0021:  // ForceHeadphoneOut
        case 0x0012:  // UnloadComponent
            r.Reply(1, 0);
            return;
        case 0x000C: {  // ConvertProcessAddressFromDspDram(address) -> virtual address
            const u32 addr = c[1];  // read before the reply overwrites it
            r.Reply(2, 0);
            c[2] = DspToVirt(addr);
            return;
        }
        case 0x000D: {  // WriteProcessPipe(channel, size) + buffer
            const u32 ch = c[1], size = c[2], buf = c[4];
            Log(LOG_DEBUG, "dsp", "pipe %u: game writes %u bytes", ch, size);
            if (ch == 2 && size >= 4) {
                const u16 cmd = mem::R16(buf);
                if (cmd == 0 || cmd == 2) {  // initialise / wake up
                    // The DSP answers with the addresses of its 15 shared
                    // structures and starts producing audio frames.
                    auto& p = pipes[2];
                    p.clear();
                    p.push_back(15);
                    p.push_back(0);
                    for (u16 a : kStructs) {
                        p.push_back((u8)a);
                        p.push_back((u8)(a >> 8));
                    }
                    RaisePipe(2, *r.lock);
                    StartFrames();
                } else if (cmd == 1) {  // shut down
                    running = false;
                }
            }
            (void)buf;
            r.Reply(1, 0);
            return;
        }
        case 0x000E:    // ReadPipe
        case 0x0010: {  // ReadPipeIfPossible(channel, peer, size) -> bytes read
            const u32 ch = c[1], size = c[3] & 0xFFFF;
            const u32 buf = mem::R32(r.thread->tls + 0x184);
            auto& p = pipes[ch];
            Log(LOG_DEBUG, "dsp", "pipe %u: game reads %u bytes (%zu ready) into %08X (desc %08X; cmd %08X %08X %08X %08X)", ch, size,
                p.size(), buf, mem::R32(r.thread->tls + 0x180), c[0], c[1], c[2], c[3]);
            u32 n = 0;
            for (; n < size && !p.empty(); ++n) {
                if (buf) mem::W8(buf + n, p.front());
                p.pop_front();
            }
            r.Reply(2, 2);
            c[2] = n;
            c[3] = size << 14 | 2;
            c[4] = buf;
            return;
        }
        case 0x000F: {  // GetPipeReadableSize
            const u32 ch = c[1];
            r.Reply(2, 0);
            c[2] = (u32)pipes[ch].size();
            return;
        }
        case 0x0011:  // LoadComponent(size, program mask, data mask) -> loaded
            Log(LOG_INFO, "dsp", "DSP program loaded (%u bytes)", c[1]);
            r.Reply(2, 2);
            c[2] = 1;
            c[3] = c[4];
            c[4] = c[5];
            return;
        case 0x0015: {  // RegisterInterruptEvents(interrupt, channel, event)
            auto e = kernel::GetAs<kernel::Event>(c[4], kernel::Type::Event);
            interrupts[c[1] << 8 | c[2]] = e;
            Log(LOG_INFO, "dsp", "interrupt %u channel %u -> handle %X (%s)", c[1], c[2], c[4], e ? e->name.c_str() : "none");
            if (e) e->name += " (dsp interrupt)";
            r.Reply(1, 0);
            return;
        }
        case 0x0016:  // GetSemaphoreEventHandle
            r.Reply(1, 2);
            c[2] = 0;
            c[3] = kernel::NewHandle(semaphore);
            return;
        case 0x001F:  // GetHeadphoneStatus
            r.Reply(2, 0);
            c[2] = 0;
            return;
        }
        Unhandled(r);
    }
};

}  // namespace

void RegisterDsp() { Register("dsp::DSP", std::make_shared<Dsp>()); }

}  // namespace ipc
}  // namespace rt
