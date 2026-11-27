// Host thread priorities. The timing threads (VBlank, sound frames, the
// kernel's timers, the sound output) must run on time even when the
// rasterizer's helpers keep every core busy: late sound frames are heard as
// crackle and starve the game's movie player, which follows the sound.
#include "rt.h"

#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace rt {

void SetThreadRole(ThreadRole role)
{
#ifdef _WIN32
    int p = THREAD_PRIORITY_NORMAL;
    switch (role) {
    case ThreadRole::Timing: p = THREAD_PRIORITY_HIGHEST; break;
    case ThreadRole::Game: p = THREAD_PRIORITY_ABOVE_NORMAL; break;
    case ThreadRole::Background: p = THREAD_PRIORITY_BELOW_NORMAL; break;
    }
    SetThreadPriority(GetCurrentThread(), p);
#else
    // Without privileges a thread can only lower itself: the helpers step back.
    if (role == ThreadRole::Background) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
#endif
}

void SleepUntil(std::chrono::steady_clock::time_point t)
{
#ifdef _WIN32
    // Windows sleeps in whole milliseconds (at best): sleep most of the way,
    // then wait out the rest by yielding.
    const auto spin = std::chrono::microseconds(1500);
    if (t - std::chrono::steady_clock::now() > spin) std::this_thread::sleep_until(t - spin);
    while (std::chrono::steady_clock::now() < t) std::this_thread::yield();
#else
    std::this_thread::sleep_until(t);
#endif
}

}  // namespace rt
