// Guest address space: one 4 GB reservation, committed range by range.
#include "rt.h"

#include <atomic>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

extern "C" uint8_t* g_mem = nullptr;

namespace rt {
namespace mem {

namespace {
constexpr u32 PAGE = 0x1000;
std::mutex s_lock;
std::map<u32, u32> s_ranges;  // start -> end of committed ranges
}  // namespace

bool Init()
{
#ifdef _WIN32
    g_mem = (uint8_t*)VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* p = mmap(nullptr, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    g_mem = p == MAP_FAILED ? nullptr : (uint8_t*)p;
#endif
    if (!g_mem) Log(LOG_ERROR, "mem", "cannot reserve 4 GB of address space");
    return g_mem != nullptr;
}

bool Map(u32 addr, u32 size)
{
    const u32 a = addr & ~(PAGE - 1);
    const u64 end = ((u64)addr + size + PAGE - 1) & ~(u64)(PAGE - 1);
    if (end > 0x100000000ull || end <= a) return false;
#ifdef _WIN32
    if (!VirtualAlloc(g_mem + a, (size_t)(end - a), MEM_COMMIT, PAGE_READWRITE)) return false;
#else
    if (mprotect(g_mem + a, (size_t)(end - a), PROT_READ | PROT_WRITE) != 0) return false;
#endif
    std::lock_guard<std::mutex> g(s_lock);
    s_ranges[a] = (u32)(end - 1);
    Log(LOG_DEBUG, "mem", "map %08X-%08X", a, (u32)(end - 1));
    return true;
}

void Unmap(u32 addr, u32 size)
{
    const u32 a = addr & ~(PAGE - 1);
    const u64 end = ((u64)addr + size + PAGE - 1) & ~(u64)(PAGE - 1);
#ifdef _WIN32
    VirtualFree(g_mem + a, (size_t)(end - a), MEM_DECOMMIT);
#else
    // Drop the contents and make it inaccessible again.
    mmap(g_mem + a, (size_t)(end - a), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
#endif
    std::lock_guard<std::mutex> g(s_lock);
    s_ranges.erase(a);
}

bool IsMapped(u32 addr, u32 size)
{
    std::lock_guard<std::mutex> g(s_lock);
    auto it = s_ranges.upper_bound(addr);
    if (it == s_ranges.begin()) return false;
    --it;
    return addr >= it->first && (u64)addr + size - 1 <= it->second;
}

std::string ReadString(u32 a, u32 max)
{
    std::string s;
    for (u32 i = 0; i < max && IsMapped(a + i, 1); ++i) {
        const char ch = (char)g_mem[a + i];
        if (!ch) break;
        s += ch;
    }
    return s;
}

}  // namespace mem
}  // namespace rt

namespace rt {

namespace {
std::atomic<u32> s_watchPage{0};
std::atomic<int> s_watchHits{0};
std::atomic<bool> s_watchArmed{false};
u32 s_rangeLo = 0, s_rangeHi = 0;  // DS2RB_WATCH=addr:len: only writes in here are reported
int s_maxHits = 16;
}  // namespace

void WatchArm(u32 addr, const char* why)
{
#ifndef _WIN32
    u32 expected = 0;
    if (!s_watchPage.compare_exchange_strong(expected, (addr & ~0xFFFu) | 1)) return;
    Log(LOG_INFO, "watch", "watching writes to page %08X (%s)", addr & ~0xFFFu, why);
    s_watchArmed = true;
    mprotect(g_mem + (addr & ~0xFFFu), 0x1000, PROT_READ);
#endif
}

void WatchRange(u32 lo, u32 hi)
{
    s_rangeLo = lo;
    s_rangeHi = hi;
    s_maxHits = 1000000;
}

void WatchRearm()
{
#ifndef _WIN32
    const u32 p = s_watchPage;
    if (!p || s_watchArmed || s_watchHits >= s_maxHits) return;
    s_watchArmed = true;
    mprotect(g_mem + (p & ~0xFFFu), 0x1000, PROT_READ);
#endif
}

bool WatchHit(u32 addr, bool* report)
{
#ifndef _WIN32
    const u32 p = s_watchPage & ~0xFFFu;
    if (!(s_watchPage & 1) || (addr & ~0xFFFu) != p || !s_watchArmed) return false;
    mprotect(g_mem + p, 0x1000, PROT_READ | PROT_WRITE);
    s_watchArmed = false;
    *report = s_rangeHi == 0 || (addr >= s_rangeLo && addr < s_rangeHi);
    if (*report) ++s_watchHits;
    return true;
#else
    return false;
#endif
}

}  // namespace rt
