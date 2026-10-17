// ds2rb: runs the recompiled game. Usage: ds2rb [--data <setup output>] [-v|-vv]
#include "ipc.h"
#include "kernel.h"
#include "audio.h"
#include "display.h"
#include "rt.h"

#include <csignal>
#ifndef _WIN32
#include <execinfo.h>
#include <ucontext.h>
#include <sys/time.h>
#include <signal.h>
#endif
#include <algorithm>
#include <cstdio>
#include <thread>
#include <chrono>
#include <atomic>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#endif
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

namespace rt {

int g_logLevel = LOG_INFO;
Exheader g_exheader;
std::string g_dataDir = "data";

namespace {
std::mutex s_logLock;
const char* kLevel[] = {"error", "warn", "info", "debug", "trace"};
}

// The log also goes to ds2rb.log in the working folder (for bug reports).
FILE* s_logFile = nullptr;

void Log(LogLevel lv, const char* area, const char* fmt, ...)
{
    if (lv > g_logLevel) return;
    std::lock_guard<std::mutex> g(s_logLock);
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[%s] %s: %s\n", kLevel[lv], area, line);
    if (s_logFile) {
        std::fprintf(s_logFile, "[%s] %s: %s\n", kLevel[lv], area, line);
        std::fflush(s_logFile);
    }
}

void Fatal(const char* fmt, ...)
{
    {
        std::lock_guard<std::mutex> g(s_logLock);
        char line[2048];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line, sizeof line, fmt, ap);
        va_end(ap);
        std::fprintf(stderr, "[fatal] %s", line);
        if (s_logFile) {
            std::fprintf(s_logFile, "[fatal] %s\n", line);
            std::fflush(s_logFile);
        }
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }
    std::_Exit(1);
}

// ---- the function table ----
namespace {
int FindIndexSlow(u32 a)
{
    unsigned lo = 0, hi = rc_func_count;
    while (lo < hi) {
        const unsigned m = (lo + hi) / 2;
        if (rc_addr[m] < a) lo = m + 1;
        else hi = m;
    }
    return lo < rc_func_count && rc_addr[lo] == a ? (int)lo : -1;
}

// Computed calls are frequent: a flat table from (address - first) / 2 to
// the function's index + 1 answers them at once.
struct FuncTable {
    u32 first = 0, last = 0;
    std::vector<u32> index;
    FuncTable()
    {
        if (!rc_func_count) return;
        first = rc_addr[0];
        last = rc_addr[rc_func_count - 1];
        index.assign((last - first) / 2 + 1, 0);
        for (unsigned i = 0; i < rc_func_count; ++i) index[(rc_addr[i] - first) / 2] = i + 1;
    }
};

inline int FindIndex(u32 a)
{
    static const FuncTable t;
    if (a < t.first || a > t.last || (a & 1)) return FindIndexSlow(a);
    return (int)t.index[(a - t.first) / 2] - 1;
}
}  // namespace

RcFunc FindFunction(u32 addr)
{
    const int i = FindIndex(addr & ~1u);
    return i < 0 ? nullptr : *rc_slot[i];
}

bool Override(u32 addr, RcFunc fn)
{
    const int i = FindIndex(addr);
    if (i < 0) return false;
    *rc_slot[i] = fn;
    return true;
}

namespace {

bool ReadFile(const std::string& path, std::vector<u8>* out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

u32 U32(const std::vector<u8>& d, size_t o) { return d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (u32)d[o + 3] << 24; }

bool LoadProgram()
{
    std::vector<u8> exh, code;
    if (!ReadFile(g_dataDir + "/exheader.bin", &exh) || exh.size() < 0x400) {
        Log(LOG_ERROR, "load", "cannot read %s/exheader.bin (run ds2rb_setup first)", g_dataDir.c_str());
        return false;
    }
    if (!ReadFile(g_dataDir + "/code.bin", &code)) {
        Log(LOG_ERROR, "load", "cannot read %s/code.bin", g_dataDir.c_str());
        return false;
    }
    Exheader& e = g_exheader;
    e.title.assign((const char*)exh.data(), strnlen((const char*)exh.data(), 8));
    e.textAddr = U32(exh, 0x10);
    e.textSize = U32(exh, 0x18);
    e.roAddr = U32(exh, 0x20);
    e.roSize = U32(exh, 0x28);
    e.dataAddr = U32(exh, 0x30);
    e.dataSize = U32(exh, 0x38);
    e.bssSize = U32(exh, 0x3C);
    e.stackSize = U32(exh, 0x1C);
    const u32 end = e.dataAddr + e.dataSize + e.bssSize;
    if (!mem::Map(e.textAddr, end - e.textAddr)) return false;
    std::memcpy(mem::Ptr(e.textAddr), code.data(), std::min<size_t>(code.size(), end - e.textAddr));
    // .bss: zero (code.bin may carry padding past the data).
    std::memset(mem::Ptr(e.dataAddr + e.dataSize), 0, e.bssSize);
    // Main thread stack, TLS pages, VRAM, the DSP windows, config and shared pages.
    mem::Map(mem::STACK_TOP - e.stackSize, e.stackSize);
    mem::Map(mem::TLS, 0x10000);
    mem::Map(mem::VRAM, mem::VRAM_SIZE);
    mem::Map(mem::DSP_RAM, mem::DSP_RAM_SIZE);
    mem::Map(mem::CONFIG, 0x1000);
    mem::Map(mem::SHARED_PAGE, 0x1000);
    // Kernel configuration page.
    mem::W32(mem::CONFIG + 0x00, 0x02220000);  // kernel version 2.34
    mem::W32(mem::CONFIG + 0x30, 0);           // APPMEMTYPE: 64 MB
    mem::W32(mem::CONFIG + 0x40, 0x04000000);  // APPMEMALLOC
    mem::W32(mem::CONFIG + 0x44, 0x02C00000);  // SYSMEMALLOC
    mem::W32(mem::CONFIG + 0x48, 0x01400000);  // BASEMEMALLOC
    mem::W32(mem::CONFIG + 0x60, 0x022E0000);  // FIRM version
    // Shared page: running on a retail unit, 3D slider off.
    mem::W8(mem::SHARED_PAGE + 0x04, 1);
    Log(LOG_INFO, "load", "%s: code %08X-%08X, stack %X", e.title.c_str(), e.textAddr, end, e.stackSize);
    return true;
}

#ifdef _WIN32
LONG WINAPI OnWinCrash(EXCEPTION_POINTERS* e)
{
    kernel::Thread* t = kernel::Current();
    const uintptr_t fault = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1], base = (uintptr_t)g_mem;
    char where[160] = "";
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && fault >= base && fault < base + (1ull << 32))
        std::snprintf(where, sizeof where, " at guest address %08X", (u32)(fault - base));
    char who[160] = "";
    if (t) std::snprintf(who, sizeof who, " (thread %u, in function %08X, lr %08X, sp %08X)", t->id, t->cpu.r[15], t->cpu.r[14], t->cpu.r[13]);
    Fatal("exception %08lX at host %p%s%s", (unsigned long)e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress, where, who);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#ifndef _WIN32
// DS2RB_PROFILE=1: samples where the process is (host program counter) 1000
// times a second of CPU time; the counts go to profile.txt every 10 s, for
// addr2line against this binary.
std::atomic<u64> s_samples[1 << 16];
std::atomic<uintptr_t> s_sampleKey[1 << 16];

void OnProfile(int, siginfo_t*, void* ctx)
{
    const uintptr_t pc = (uintptr_t)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
    u32 h = (u32)((pc >> 2) * 2654435761u) >> 16;
    for (int i = 0; i < 16; ++i, h = (h + 1) & 0xFFFF) {
        uintptr_t k = s_sampleKey[h].load(std::memory_order_relaxed);
        if (k == pc || (k == 0 && s_sampleKey[h].compare_exchange_strong(k, pc))) {
            ++s_samples[h];
            return;
        }
        if (k == pc) { ++s_samples[h]; return; }
    }
}

void StartProfile()
{
    struct sigaction sa = {};
    sa.sa_sigaction = OnProfile;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    itimerval it = {};
    it.it_interval.tv_usec = 1000;
    it.it_value.tv_usec = 1000;
    setitimer(ITIMER_PROF, &it, nullptr);
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            FILE* f = std::fopen("profile.txt", "w");
            if (!f) continue;
            for (int i = 0; i < (1 << 16); ++i)
                if (const u64 n = s_samples[i]) std::fprintf(f, "%llu %lx\n", (unsigned long long)n, (unsigned long)s_sampleKey[i].load());
            std::fclose(f);
        }
    }).detach();
}

void OnCrash(int sig, siginfo_t* info, void*)
{
    kernel::Thread* t = kernel::Current();
    const uintptr_t fault = (uintptr_t)info->si_addr, base = (uintptr_t)g_mem;
    bool report = true;
    if (fault >= base && fault < base + (1ull << 32) && WatchHit((u32)(fault - base), &report)) {
        if (!report) return;
        std::fprintf(stderr, "[watch] write to %08X", (u32)(fault - base));
        if (t) std::fprintf(stderr, " by thread %u in function %08X (lr %08X, sp %08X)", t->id, t->cpu.r[15], t->cpu.r[14], t->cpu.r[13]);
        std::fprintf(stderr, "; host:\n");
        void* frames[8];
        const int n = backtrace(frames, 8);
        backtrace_symbols_fd(frames, n, 2);
        return;
    }
    std::fprintf(stderr, "[fatal] signal %d", sig);
    if (fault >= base && fault < base + (1ull << 32)) std::fprintf(stderr, " at guest address %08X", (u32)(fault - base));
    else std::fprintf(stderr, " at host address %p", info->si_addr);
    if (t) std::fprintf(stderr, " (thread %u, in function %08X, lr %08X, sp %08X)", t->id, t->cpu.r[15], t->cpu.r[14], t->cpu.r[13]);
    std::fprintf(stderr, "\n");
    if (t) {
        std::fprintf(stderr, "[fatal] registers:");
        for (int i = 0; i < 13; ++i) std::fprintf(stderr, " r%d=%08X", i, t->cpu.r[i]);
        std::fprintf(stderr, "\n");
        // What the registers point at, and the stack: enough to tell what was being worked on.
        for (int i = 0; i <= 13; ++i) {
            const u32 a = t->cpu.r[i] & ~3u;
            if (!mem::IsMapped(a, 64)) continue;
            std::fprintf(stderr, "[fatal] %s %08X:", i == 13 ? "sp" : ("r" + std::to_string(i)).c_str(), a);
            for (u32 k = 0; k < (i == 13 ? 64u : 16u); ++k) std::fprintf(stderr, "%s%08X", k % 8 ? " " : "\n    ", mem::R32(a + 4 * k));
            std::fprintf(stderr, "\n");
        }
    }
    void* frames[32];
    const int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, 2);
    std::_Exit(1);
}
#endif

}  // namespace
}  // namespace rt

using namespace rt;

// ---- what the generated code calls ----
extern "C" const unsigned rc_dyn_count;
extern "C" const RcDyn rc_dyn[];

extern "C" void rc_call(RcCpu* c, uint32_t addr)
{
    RcFunc f = FindFunction(addr);
    if (!f) {
        // Inside code with computed jumps: enter it there.
        const u32 a = addr & ~1u;
        for (unsigned i = 0; i < rc_dyn_count; ++i)
            if (a >= rc_dyn[i].start && a < rc_dyn[i].end) {
                c->r[15] = a;
                rc_dyn[i].fn(c, a);
                return;
            }
    }
    if (!f)
        Fatal("call to %08X: no recompiled function there (lr %08X; r0-r7 %08X %08X %08X %08X %08X %08X %08X %08X)", addr, c->r[14],
              c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
    c->r[15] = addr & ~1u;
    f(c);
}
extern "C" int rc_excl_cas = 1;
extern "C" void rc_svc(RcCpu* c, uint32_t imm) { kernel::Svc(c, imm); }
extern "C" void rc_bkpt(RcCpu* c, uint32_t pc, uint32_t imm) { Fatal("breakpoint %X at %08X (lr %08X)", imm, pc, c->r[14]); }
extern "C" void rc_trap(RcCpu* c, uint32_t pc, uint32_t raw) { Fatal("untranslated instruction %08X at %08X (lr %08X)", raw, pc, c->r[14]); }
extern "C" void rc_unreachable(RcCpu* c, uint32_t pc) { Fatal("ran past the decoded code at %08X (lr %08X)", pc, c->r[14]); }
extern "C" void rc_badreturn(RcCpu* c, uint32_t pc, uint32_t to, uint32_t lr)
{
    static int n = 0;
    if (n++ < 20) Log(LOG_WARN, "recomp", "return at %08X goes to %08X, not to the caller (%08X); sp %08X", pc, to, lr, c->r[13]);
}
extern "C" void rc_badsp(RcCpu* c, uint32_t entry, uint32_t pc, uint32_t sp0)
{
    static int n = 0;
    if (n++ < 20)
        Log(LOG_WARN, "recomp", "function %08X returns at %08X with sp %08X, called with %08X (lr %08X)", entry, pc, c->r[13], sp0, c->r[14]);
}
extern "C" void rc_cp15_write(RcCpu*, uint32_t pc, uint32_t raw, uint32_t) { Log(LOG_DEBUG, "cp15", "write %08X at %08X ignored", raw, pc); }
extern "C" uint32_t rc_cp15_read(RcCpu*, uint32_t pc, uint32_t raw)
{
    Log(LOG_WARN, "cp15", "read %08X at %08X: 0", raw, pc);
    return 0;
}

int main(int argc, char** argv)
{
    bool headless = false;
#ifdef _WIN32
    SetUnhandledExceptionFilter(OnWinCrash);
    // Windows sleeps in 15.6 ms steps unless asked for 1 ms: the game's
    // short waits (0.5 ms polls in the movie player) would crawl.
    timeBeginPeriod(1);
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) g_dataDir = argv[++i];
        else if (a == "-v") g_logLevel = LOG_DEBUG;
        else if (a == "-vv") g_logLevel = LOG_TRACE;
        else if (a == "-q") g_logLevel = LOG_WARN;
        else if (a == "--headless") headless = true;
    }
#ifndef _WIN32
    struct sigaction sa = {};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
#endif
#ifndef _WIN32
    if (std::getenv("DS2RB_PROFILE")) StartProfile();
#endif
    if (std::getenv("DS2RB_STREX_SIMPLE")) rc_excl_cas = 0;
    if (const char* sp = std::getenv("DS2RB_SPEED")) g_speed = std::max(0.1, std::atof(sp));
    if (!std::getenv("DS2RB_NO_LOGFILE")) s_logFile = std::fopen("ds2rb.log", "w");
    if (!mem::Init() || !LoadProgram()) return 1;
    InstallPatches();
    if (!headless) audio::Open();
    if (!headless) display::OpenWindow(("Devil Survivor 2 Record Breaker - " + g_exheader.title).c_str());
    ipc::RegisterAll();
    const int code = kernel::RunProcess(g_exheader.textAddr, mem::STACK_TOP, 0x30);
    Log(LOG_INFO, "main", "the game exited (%d)", code);
    return code;
}
