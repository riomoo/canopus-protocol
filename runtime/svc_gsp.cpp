// gsp::Gpu: the GPU service. Shared memory holds, per registered thread, the
// interrupt queue (0x000 + 0x40 * thread), framebuffer info for both screens
// (0x200 + 0x80 * thread) and the GX command queue (0x800 + 0x200 * thread).
// A host thread raises VBlank 60 times a second. GX commands are taken off the
// queue when the game triggers it and run on a host thread of their own, as
// the GPU runs beside the CPU; each raises its completion interrupt when done.
#include "display.h"
#include "gpu.h"
#include "pica.h"
#include "input.h"
#include "ipc.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace rt {
namespace ipc {

namespace {

enum Interrupt { PSC0 = 0, PSC1 = 1, VBLANK_TOP = 2, VBLANK_BOTTOM = 3, PPF = 4, P3D = 5, DMA = 6 };

struct Gsp : Service {
    Gsp() : Service("gsp::Gpu")
    {
        shared->size = 0x1000;
        shared->name = "GSP shared memory";
    }
    std::shared_ptr<kernel::SharedMemory> shared = std::make_shared<kernel::SharedMemory>();
    std::shared_ptr<kernel::Event> event;  // the game's interrupt event
    std::thread vblank;
    std::atomic<bool> running{false};
    // GX commands waiting for the GPU thread, in order.
    std::mutex workLock;
    std::condition_variable workCv;
    struct Work {
        std::array<u32, 8> cmd;
        u64 vblank;  // when it was queued
    };
    std::deque<Work> work;
    bool workerStarted = false;
    int pending = 0;  // queued or running (under workLock)
    u64 runningVblank = 0;
    bool gpuRunning = false;
    std::condition_variable idleCv;
    std::atomic<u64> vblanks{0};

    // The VBlank stamp of the oldest command not finished, if any (workLock held).
    bool Oldest(u64* stamp) const
    {
        if (gpuRunning) {
            *stamp = runningVblank;
            return true;
        }
        if (!work.empty()) {
            *stamp = work.front().vblank;
            return true;
        }
        return false;
    }

    u8* Shm(u32 off) { return shared->Ptr(off); }

    // Adds an interrupt to thread 0's queue (lock held).
    void Raise(Interrupt id, std::unique_lock<std::mutex>& lk)
    {
        Log(LOG_TRACE, "gsp", "interrupt %d (event %s, shared %08X)", id, event ? event->name.c_str() : "none", shared->addr);
        if (!event || !shared->addr) return;
        // The header word (index, count, missed) is shared with the game's
        // interrupt thread, which takes entries off with LDREX/STREX: update
        // it with a compare-and-swap so neither side loses the other's change.
        u8* q = Shm(0);
        u32* header = reinterpret_cast<u32*>(q);
        for (;;) {
            u32 old = __atomic_load_n(header, __ATOMIC_SEQ_CST);
            const u32 index = old & 0xFF, count = (old >> 8) & 0xFF;
            u32 now;
            if (count >= 0x34) {
                Tally("interrupt %d missed (queue full)", (int)id);
                now = (old & ~0xFF0000u) | (1u << 16);  // missed
            } else {
                q[0xC + (index + count) % 0x34] = (u8)id;
                now = (old & ~0xFF00u) | ((count + 1) << 8);
            }
            static const bool plain = std::getenv("DS2RB_GSP_PLAIN") != nullptr;
            if (plain) { *header = now; break; }
            if (__atomic_compare_exchange_n(header, &old, now, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
        }
        event->signaled = true;
        kernel::Signal(event.get(), lk);
    }

    void VBlankLoop()
    {
        using clock = std::chrono::steady_clock;
        SetThreadRole(ThreadRole::Timing);
        auto next = clock::now();
        while (running) {
            next += std::chrono::nanoseconds((s64)(16715000 / g_speed));
            SleepUntil(next);
            ++vblanks;
            idleCv.notify_all();
            input::Update(display::PollInput());
            WatchRearm();
            {
                auto lk = kernel::Lock();
                ApplyFramebuffers();
                Raise(VBLANK_TOP, lk);
                Raise(VBLANK_BOTTOM, lk);
            }
            display::Present();
        }
    }

    // The game writes new framebuffer info and sets the update flag; it takes
    // effect at the next VBlank.
    void ApplyFramebuffers()
    {
        if (!shared->addr) return;
        for (int screen = 0; screen < 2; ++screen) {
            u8* info = Shm(0x200 + screen * 0x40);
            if (!info[1]) continue;
            const u32 index = info[0] & 1;
            const u8* fb = info + 4 + index * 0x1C;
            u32 w[7];
            std::memcpy(w, fb, sizeof w);
            display::SetFramebuffer(screen, w[1], w[3], w[4] & 7);
            info[1] = 0;
        }
    }

    // Takes the queued GX commands of thread 0 (kernel lock held) and hands
    // them to the GPU thread.
    void RunQueue(std::unique_lock<std::mutex>& kernelLock)
    {
        u8* h = Shm(0x800);
        if (!h) return;
        {
            std::lock_guard<std::mutex> g(workLock);
            while (h[1] > 0) {
                const u32 index = h[0];
                std::array<u32, 8> c;
                std::memcpy(c.data(), h + 0x20 + index * 0x20, sizeof c);
                work.push_back({c, vblanks.load()});
                ++pending;
                Tally("GX command %02X queued", c[0] & 0xFF);
                h[0] = (u8)((index + 1) % 15);
                h[1] = (u8)(h[1] - 1);
            }
        }
        if (!workerStarted) {
            workerStarted = true;
            std::thread([this] { GpuLoop(); }).detach();
        }
        workCv.notify_one();
        // DS2RB_GPU_SYNC: the game waits until the GPU has caught up (tests).
        // Otherwise the game may run ahead of the GPU by at most a frame: past
        // that it would be rewriting command lists and buffers the GPU has
        // yet to read (on the 3DS the GPU always keeps up).
        static const bool sync = std::getenv("DS2RB_GPU_SYNC") != nullptr;
        auto caughtUp = [&] {
            u64 oldest;
            if (!Oldest(&oldest)) return true;
            return !sync && oldest + 1 >= vblanks.load();
        };
        std::unique_lock<std::mutex> g(workLock);
        if (!caughtUp()) {
            g.unlock();
            kernelLock.unlock();
            g.lock();
            idleCv.wait(g, caughtUp);
            g.unlock();
            kernelLock.lock();
        }
    }

    // The GPU: runs commands without the kernel lock, then raises their interrupts.
    void GpuLoop()
    {
        for (;;) {
            std::array<u32, 8> c;
            {
                std::unique_lock<std::mutex> g(workLock);
                workCv.wait(g, [&] { return !work.empty(); });
                c = work.front().cmd;
                runningVblank = work.front().vblank;
                gpuRunning = true;
                work.pop_front();
            }
            const auto t0 = std::chrono::steady_clock::now();
            Interrupt done[2];
            int n = 0;
            switch (c[0] & 0xFF) {
            case 0x00:  // RequestDma(src, dst, size)
                if (c[1] && c[2]) std::memmove(mem::Ptr(c[2]), mem::Ptr(c[1]), c[3]);
                done[n++] = DMA;
                break;
            case 0x01:  // ProcessCommandList(addr, size)
                { ProfScope p(PROF_GPU_LIST); gpu::ProcessCommandList(c[1], c[2]); }
                done[n++] = P3D;
                break;
            case 0x02:  // MemoryFill: two buffers (start, value, end), control halves
                {
                    ProfScope p(PROF_GPU_FILL);
                    gpu::MemoryFill(c[1], c[3], c[2], c[7] & 0xFFFF);
                    gpu::MemoryFill(c[4], c[6], c[5], c[7] >> 16);
                }
                if (c[1]) done[n++] = PSC0;
                if (c[4]) done[n++] = PSC1;
                break;
            case 0x03:  // DisplayTransfer(in, out, in dims, out dims, flags)
                {
                    static u64 lastDraws = 0, lastPixels = 0;
                    Tally("transfer %08X (out %08X): %llu draws, %llu pixels since the last", c[1], c[4],
                          (unsigned long long)(pica::g_stats.draws - lastDraws), (unsigned long long)(pica::g_stats.pixels - lastPixels) / 1000 * 1000);
                    lastDraws = pica::g_stats.draws;
                    lastPixels = pica::g_stats.pixels;
                }
                { ProfScope p(PROF_GPU_TRANSFER); gpu::DisplayTransfer(c[1], c[2], c[3], c[4], c[5]); }
                done[n++] = PPF;
                break;
            case 0x04:  // TextureCopy(in, out, size, in gap, out gap, flags)
                { ProfScope p(PROF_GPU_COPY); gpu::TextureCopy(c[1], c[2], c[3], c[4], c[5], c[6]); }
                done[n++] = PPF;
                break;
            case 0x05:  // FlushCacheRegions
                break;
            default:
                Log(LOG_WARN, "gsp", "GX command %02X not handled", c[0] & 0xFF);
                break;
            }
            g_gpuBusyNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            Tally("GX command %02X done", c[0] & 0xFF);
            if (n) {
                auto lk = kernel::Lock();
                for (int i = 0; i < n; ++i) {
                    Raise(done[i], lk);
                    Tally("interrupt %d raised", (int)done[i]);
                }
            }
            {
                std::lock_guard<std::mutex> g(workLock);
                gpuRunning = false;
                --pending;
            }
            idleCv.notify_all();
        }
    }

    void Handle(Request& r) override
    {
        auto& c = r.cmd;
        switch (r.id()) {
        case 0x0001:  // WriteHWRegs(reg, size) + buffer
        case 0x0002:  // WriteHWRegsWithMask
            Log(LOG_DEBUG, "gsp", "WriteHWRegs %08X size %X", c[1], c[2]);
            r.Reply(1, 0);
            return;
        case 0x0004: {  // ReadHWRegs(reg, size) -> buffer
            const u32 size = c[2], buf = mem::R32(r.thread->tls + 0x184);
            if (buf) std::memset(mem::Ptr(buf), 0, size);
            r.Reply(1, 2);
            c[2] = size << 14 | 2;
            c[3] = buf;
            return;
        }
        case 0x0005: {  // SetBufferSwap(screen, info[7])
            display::SetFramebuffer((int)c[1], c[3], c[5], c[6] & 7);
            r.Reply(1, 0);
            return;
        }
        case 0x0008:  // FlushDataCache
        case 0x0009:  // InvalidateDataCache
        case 0x000A:  // RegisterInterruptEvents
        case 0x000B:  // SetLcdForceBlack
        case 0x0014:  // UnregisterInterruptRelayQueue
        case 0x0016:  // AcquireRight
        case 0x0017:  // ReleaseRight
        case 0x0019:  // SaveVramSysArea
        case 0x001A:  // RestoreVramSysArea
        case 0x001E:  // SetInternalPriorities
        case 0x001F:  // StoreDataCache
            r.Reply(1, 0);
            return;
        case 0x000C:  // TriggerCmdReqQueue
            RunQueue(*r.lock);
            r.Reply(1, 0);
            return;
        case 0x0013: {  // RegisterInterruptRelayQueue(flags, event) -> thread index, shared memory
            event = kernel::GetAs<kernel::Event>(c[3], kernel::Type::Event);
            r.Reply(2, 2, 0x2A07);
            c[2] = 0;  // thread index
            c[3] = 0x04000000;
            c[4] = kernel::NewHandle(shared);
            if (!running) {
                running = true;
                vblank = std::thread([this] { VBlankLoop(); });
                vblank.detach();
            }
            return;
        }
        case 0x0018: {  // ImportDisplayCaptureInfo
            r.Reply(9, 0);
            std::memset(&c[2], 0, 8 * 4);
            return;
        }
        }
        Unhandled(r);
    }
};

}  // namespace

void RegisterGsp() { Register("gsp::Gpu", std::make_shared<Gsp>()); }

}  // namespace ipc
}  // namespace rt
