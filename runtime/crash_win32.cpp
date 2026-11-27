// Windows: catching crashes that the plain unhandled-exception filter
// misses, so a player's ds2rb.log always says how the game ended:
//   - another DLL (a graphics driver, an overlay) replacing our filter:
//     it is put back every second, and the replacement is logged;
//   - stack overflows (the report runs on a thread of its own, with stack);
//   - illegal instructions and heap corruption noted when they happen;
//   - C++ terminate, abort(), pure virtual calls, bad CRT parameters.
// Each writes ds2rb-crash.dmp (a minidump) next to the log, and names the
// module the crash was in (for example nvoglv64.dll+0x1234).
#ifdef _WIN32

#include "rt.h"
#include "display.h"
#include "kernel.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>

namespace rt {

extern FILE* s_logFile;

namespace crash {
namespace {

std::atomic<bool> s_reported{false};
LPTOP_LEVEL_EXCEPTION_FILTER s_filter = nullptr;

// "nvoglv64.dll+0x1A2B" for a code address, or the address alone.
std::string ModuleOf(const void* addr)
{
    HMODULE m = nullptr;
    char buf[64];
    if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &m)) {
        std::snprintf(buf, sizeof buf, "%p", addr);
        return buf;
    }
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
    std::wstring w(path, n);
    const size_t slash = w.find_last_of(L"\\/");
    if (slash != std::wstring::npos) w = w.substr(slash + 1);
    std::string name;
    for (wchar_t c : w) name += c < 128 ? (char)c : '?';
    std::snprintf(buf, sizeof buf, "+0x%llX", (unsigned long long)((const char*)addr - (const char*)m));
    return name + buf;
}

const char* CodeName(DWORD c)
{
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "stack buffer overrun / fail-fast";
    case 0xE06D7363: return "C++ exception";
    default: return "exception";
    }
}

void WriteDump(EXCEPTION_POINTERS* e, DWORD threadId)
{
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    if (!dbg) return;
    typedef BOOL(WINAPI * Fn)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    const Fn fn = (Fn)(void*)GetProcAddress(dbg, "MiniDumpWriteDump");
    if (!fn) return;
    HANDLE f = CreateFileW(L"ds2rb-crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    struct {
        DWORD ThreadId;
        EXCEPTION_POINTERS* ExceptionPointers;
        BOOL ClientPointers;
    } mei = {threadId, e, FALSE};
    // MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules
    fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0x40 | 0x1000 | 0x20, e ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
}

struct Job {
    EXCEPTION_POINTERS* e;
    DWORD threadId;
    const char* how;
    char guest[160];
};

DWORD WINAPI ReportThread(void* p)
{
    Job* j = (Job*)p;
    char line[1024];
    if (j->e) {
        const EXCEPTION_RECORD* r = j->e->ExceptionRecord;
        char extra[96] = "";
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
            std::snprintf(extra, sizeof extra, ", %s address %p", r->ExceptionInformation[0] == 8 ? "executing" : r->ExceptionInformation[0] ? "writing" : "reading",
                          (void*)r->ExceptionInformation[1]);
        std::snprintf(line, sizeof line, "[fatal] crash (%s): %s %08lX in %s%s%s, host thread %lu, frame %llu\n", j->how, CodeName(r->ExceptionCode),
                      (unsigned long)r->ExceptionCode, ModuleOf(r->ExceptionAddress).c_str(), extra, j->guest, (unsigned long)j->threadId,
                      (unsigned long long)display::FrameCount());
    } else {
        std::snprintf(line, sizeof line, "[fatal] crash (%s), host thread %lu, frame %llu\n", j->how, (unsigned long)j->threadId,
                      (unsigned long long)display::FrameCount());
    }
    std::fputs(line, stderr);
    if (s_logFile) {
        std::fputs(line, s_logFile);
        std::fputs("[fatal] writing ds2rb-crash.dmp (include it with ds2rb.log in a report)\n", s_logFile);
        std::fflush(s_logFile);
    }
    WriteDump(j->e, j->threadId);
    if (s_logFile) {
        std::fputs("[fatal] done\n", s_logFile);
        std::fflush(s_logFile);
    }
    return 0;
}

// Reports once, from a fresh thread (the crashed one may have no stack left).
void Report(EXCEPTION_POINTERS* e, const char* how, bool guestInfo)
{
    if (s_reported.exchange(true)) return;
    static Job j;
    j.e = e;
    j.threadId = GetCurrentThreadId();
    j.how = how;
    j.guest[0] = 0;
    if (guestInfo)
        if (kernel::Thread* t = kernel::Current())
            std::snprintf(j.guest, sizeof j.guest, ", guest thread %u in %08X (lr %08X)", t->id, t->cpu.r[15], t->cpu.r[14]);
    HANDLE h = CreateThread(nullptr, 1 << 20, ReportThread, &j, 0, nullptr);
    if (h) {
        WaitForSingleObject(h, 20000);
        CloseHandle(h);
    }
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* e)
{
    Report(e, "unhandled", e->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW);
    std::_Exit(1);
}

// First look at every exception: only those that nothing can recover from.
LONG WINAPI OnVectored(EXCEPTION_POINTERS* e)
{
    switch (e->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_STACK_OVERFLOW:
        Report(e, "stack overflow", false);
        std::_Exit(1);
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case 0xC0000374: {  // heap corruption
        // Noted, not acted on: some drivers probe the CPU this way and
        // handle it themselves. If nothing handles it, the unhandled filter
        // reports it as a crash.
        static std::atomic<int> noted{0};
        if (noted++ < 3)
            Log(LOG_INFO, "crash", "first chance: %s %08lX in %s (may be handled)", CodeName(e->ExceptionRecord->ExceptionCode),
                (unsigned long)e->ExceptionRecord->ExceptionCode, ModuleOf(e->ExceptionRecord->ExceptionAddress).c_str());
        return EXCEPTION_CONTINUE_SEARCH;
    }
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

[[noreturn]] void Die(const char* how)
{
    Report(nullptr, how, true);
    std::_Exit(3);
}

void OnTerminate()
{
    static char what[400];
    std::snprintf(what, sizeof what, "C++ terminate");
    if (auto ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& x) {
            std::snprintf(what, sizeof what, "C++ terminate: %s", x.what());
        } catch (...) {
            std::snprintf(what, sizeof what, "C++ terminate: unknown exception");
        }
    }
    Die(what);
}

}  // namespace

void Install()
{
    s_filter = OnUnhandled;
    SetUnhandledExceptionFilter(OnUnhandled);
    AddVectoredExceptionHandler(1, OnVectored);
    ULONG guarantee = 64 * 1024;  // room to report a stack overflow on this thread
    SetThreadStackGuarantee(&guarantee);
    std::set_terminate(OnTerminate);
    std::signal(SIGABRT, [](int) { Die("abort()"); });
    _set_purecall_handler([] { Die("pure virtual call"); });
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { Die("invalid CRT parameter"); });
    // Drivers and overlays (Discord, Steam, recording tools...) may install
    // their own filter over ours, which then never runs: put it back.
    std::thread([] {
        bool told = false;
        for (int second = 1;; ++second) {
            Sleep(1000);
            LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(OnUnhandled);
            if (prev != OnUnhandled && !told) {
                Log(LOG_INFO, "crash", "something replaced the crash handler (%s): put back", ModuleOf((void*)prev).c_str());
                told = true;
            }
            // The first half minute, a line a second: when the log stops
            // says when the game died.
            if (second <= 30) Log(LOG_INFO, "alive", "%d s, frame %llu", second, (unsigned long long)display::FrameCount());
        }
    }).detach();
    // Testing: DS2RB_CRASHTEST=<kind>[:seconds] crashes on purpose (default
    // after 5 s), to check that each kind of crash is reported.
    if (const char* t = std::getenv("DS2RB_CRASHTEST")) {
        std::string kind = t;
        int after = 5;
        const size_t colon = kind.find(':');
        if (colon != std::string::npos) {
            after = std::atoi(kind.c_str() + colon + 1);
            kind = kind.substr(0, colon);
        }
        std::thread([kind, after] {
            Sleep(after * 1000);
            Log(LOG_INFO, "crash", "crash test: %s", kind.c_str());
            if (kind == "av") *(volatile int*)(uintptr_t)16 = 1;
            else if (kind == "stack") {
                struct R { static int Down(int n) { volatile char pad[4096]; pad[0] = (char)n; return Down(n + 1) + pad[0]; } };
                R::Down(0);
            } else if (kind == "abort") std::abort();
            else if (kind == "terminate") std::terminate();
            else if (kind == "illegal") __builtin_trap();
            else if (kind == "replace") {
                SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG { return EXCEPTION_EXECUTE_HANDLER; });
                Sleep(1500);
                *(volatile int*)(uintptr_t)16 = 1;
            }
        }).detach();
    }
    // How it ends normally, to tell that apart from a crash.
    std::atexit([] { Log(LOG_INFO, "exit", "the game is exiting normally"); });
}

// What machine this is: the details that differ between players.
void LogSystem()
{
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW v = {};
    v.dwOSVersionInfoSize = sizeof v;
    if (auto f = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) f(&v);
    MEMORYSTATUSEX m = {};
    m.dwLength = sizeof m;
    GlobalMemoryStatusEx(&m);
    char cpu[64] = "?";
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD size = sizeof cpu - 1;
        RegQueryValueExA(k, "ProcessorNameString", nullptr, nullptr, (BYTE*)cpu, &size);
        RegCloseKey(k);
    }
    Log(LOG_INFO, "system", "Windows %lu.%lu build %lu, %s, %u threads, %.1f GB RAM (%.1f GB free)", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
        cpu, std::thread::hardware_concurrency(), m.ullTotalPhys / 1073741824.0, m.ullAvailPhys / 1073741824.0);
}

}  // namespace crash
}  // namespace rt

#endif
