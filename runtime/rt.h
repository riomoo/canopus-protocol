// PC runtime for the recompiled game: what the kernel, the system services
// and the hardware did on the 3DS. Shared declarations.
#pragma once

#include <atomic>
#include <chrono>

#include "recomp.h"

#include <cstdarg>
#include <cstdint>
#include <string>

namespace rt {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

// ---- logging ----
enum LogLevel { LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_TRACE };
extern int g_logLevel;
void Log(LogLevel lv, const char* area, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
[[noreturn]] void Fatal(const char* fmt, ...);

// ---- guest memory (memory.cpp) ----
// The 3DS application address space, one 4 GB reservation; a guest address
// a is at g_mem + a. Only mapped ranges are accessible.
namespace mem {
constexpr u32 CODE = 0x00100000;
constexpr u32 HEAP = 0x08000000;         // application heap (svcControlMemory)
constexpr u32 HEAP_END = 0x0E000000;
constexpr u32 STACK_TOP = 0x10000000;    // main thread stack
constexpr u32 SHARED = 0x10000000;       // mapped memory blocks
constexpr u32 SHARED_END = 0x14000000;
constexpr u32 LINEAR = 0x14000000;       // linear heap (physical FCRAM order)
constexpr u32 LINEAR_END = 0x1C000000;
constexpr u32 VRAM = 0x1F000000;
constexpr u32 VRAM_SIZE = 0x00600000;
constexpr u32 DSP_RAM = 0x1FF00000;
constexpr u32 DSP_RAM_SIZE = 0x00080000;
constexpr u32 CONFIG = 0x1FF80000;       // kernel configuration page
constexpr u32 SHARED_PAGE = 0x1FF81000;  // shared page (time, 3D slider)
constexpr u32 TLS = 0x1FF82000;          // thread-local storage, 0x200 per thread
constexpr u32 TLS_SIZE = 0x200;

bool Init();
bool Map(u32 addr, u32 size);   // commit read/write, zero-filled
void Unmap(u32 addr, u32 size);
bool IsMapped(u32 addr, u32 size);

inline u8* Ptr(u32 a) { return g_mem + a; }
inline u32 R32(u32 a) { return ld32(a); }
inline u16 R16(u32 a) { return (u16)ld16(a); }
inline u8 R8(u32 a) { return g_mem[a]; }
inline void W32(u32 a, u32 v) { st32(a, v); }
inline void W16(u32 a, u16 v) { st16(a, v); }
inline void W8(u32 a, u8 v) { g_mem[a] = v; }
std::string ReadString(u32 a, u32 max = 256);
}  // namespace mem

// ---- the program (main.cpp) ----
struct Exheader {
    u32 textAddr, textSize, roAddr, roSize, dataAddr, dataSize, bssSize, stackSize;
    std::string title;
};
extern Exheader g_exheader;
extern std::string g_dataDir;  // the setup tool's output: code.bin, exheader.bin, romfs/

// Function table of the generated code (table.c).
extern "C" const unsigned rc_func_count;
extern "C" const uint32_t rc_addr[];
extern "C" const RcFunc rc_orig[];
extern "C" RcFunc* const rc_slot[];  // what the code calls at rc_addr[i]
// Replaces the function at a guest address (a ported function, a PC
// replacement). False if there is no function there.
bool Override(u32 addr, RcFunc fn);
// The function at a guest address, or null.
RcFunc FindFunction(u32 addr);
// Game code replaced by the runtime (patches.cpp).
void InstallPatches();

// Where host time goes, for the status line (kernel.cpp prints and resets).
enum Prof { PROF_GPU_LIST, PROF_GPU_FILL, PROF_GPU_TRANSFER, PROF_GPU_COPY, PROF_Y2R, PROF_COUNT };
extern s64 g_profNs[PROF_COUNT];
// How much faster than real time the guest clock runs (DS2RB_SPEED; tests).
extern double g_speed;
// Time the GPU thread spends working, for the window's status (display.cpp).
extern std::atomic<s64> g_gpuBusyNs;
struct ProfScope {
    Prof slot;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit ProfScope(Prof p) : slot(p) {}
    ~ProfScope()
    {
        g_profNs[slot] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }
};

// Counts of notable events by description, printed and cleared with the
// status line (diagnostics; thread-safe).
void Tally(const char* fmt, ...);
std::string TakeTallies();

// Host thread priorities (thread_prio.cpp): Timing for the threads that keep
// time (VBlank, sound), Game for the guest's threads, Background for the
// rasterizer's helpers.
enum class ThreadRole { Timing, Game, Background };
void SetThreadRole(ThreadRole role);
// Sleeps until t, to well under a millisecond on every system.
void SleepUntil(std::chrono::steady_clock::time_point t);

// Write watch (diagnostics, Linux): makes the page holding addr read-only and
// logs which guest function writes it; WatchRearm (each VBlank) protects it
// again after a hit, up to a few times.
void WatchArm(u32 addr, const char* why);
void WatchRearm();
bool WatchHit(u32 addr, bool* report);  // from the fault handler: true if it was the watch
void WatchRange(u32 lo, u32 hi);

}  // namespace rt
