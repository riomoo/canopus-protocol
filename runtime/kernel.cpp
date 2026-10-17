// Kernel objects, the scheduler and the kernel calls (svc) the game makes.
// Register use of each call is as the game's own wrappers set it up.
#include "kernel.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include "ipc.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>

namespace rt {
s64 g_profNs[PROF_COUNT];
double g_speed = 1.0;
std::atomic<s64> g_gpuBusyNs{0};

namespace {
std::mutex s_tallyLock;
std::map<std::string, u32> s_tallies;
}  // namespace

void Tally(const char* fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> g(s_tallyLock);
    ++s_tallies[b];
}

std::string TakeTallies()
{
    std::lock_guard<std::mutex> g(s_tallyLock);
    std::string out;
    for (auto& [k, n] : s_tallies) out += "\n    " + std::to_string(n) + " x " + k;
    s_tallies.clear();
    return out;
}

namespace kernel {

namespace {

std::mutex s_lock;
Thread* s_current = nullptr;
std::vector<std::shared_ptr<Thread>> s_threads;
std::map<u32, ObjectPtr> s_handles;
u32 s_nextHandle = 0x100;
u32 s_nextThreadId = 1;
u64 s_seq = 0;
std::condition_variable s_timerCv;  // the timeout thread
std::condition_variable s_exitCv;   // RunProcess
bool s_exited = false;
int s_exitCode = 0;
std::thread s_timerThread;
// Diagnostics: time with no guest thread running, and how late sleeps wake.
s64 s_lastSwitch = 0, s_idleNs = 0;
s64 s_sleeps = 0, s_sleepAskedNs = 0, s_sleepTookNs = 0;

// The heaps.
std::map<u32, std::pair<u32, u32>> s_aliases;  // mapped at -> (original, size)
u32 s_heapEnd = mem::HEAP;
u32 s_linearEnd = mem::LINEAR;

// Guest time: host time, sped up by g_speed (DS2RB_SPEED, for tests).
s64 Now()
{
    const s64 real = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return g_speed == 1.0 ? real : (s64)(real * g_speed);
}

bool Better(const Thread* a, const Thread* b)
{
    if (!b) return true;
    if (a->priority != b->priority) return a->priority < b->priority;
    return a->readySeq < b->readySeq;
}

Thread* PickReady()
{
    Thread* best = nullptr;
    for (auto& t : s_threads)
        if ((t->state == Thread::READY || t->state == Thread::RUNNING) && Better(t.get(), best)) best = t.get();
    return best;
}

// Whether a ready thread should take over from the running one (the flag
// loops in the generated code check).
void UpdatePreempt()
{
    Thread* best = PickReady();
    rc_preempt = s_current && best && best != s_current && best->priority < s_current->priority;
}

// Gives the baton to the best ready thread; the caller then waits for its
// own turn (or not at all if it is dead).
void Reschedule()
{
    Thread* next = PickReady();
    if (next != s_current) {
        const s64 now = Now();
        if (s_lastSwitch) {
            if (s_current) s_current->runNs += now - s_lastSwitch;
            else s_idleNs += now - s_lastSwitch;
        }
        s_lastSwitch = now;
    }
    if (s_current && s_current->state == Thread::RUNNING && s_current != next) {
        s_current->state = Thread::READY;
    }
    s_current = next;
    if (next) {
        next->state = Thread::RUNNING;
        next->cv.notify_all();
    }
    UpdatePreempt();
}

void WaitTurn(std::unique_lock<std::mutex>& lk, Thread* self)
{
    self->where = "waiting for its turn";
    // Never trust a single wake-up: MinGW's condition variables (winpthreads)
    // can lose one, and a lost turn hangs the whole game. Checking again every
    // millisecond bounds what a lost one costs.
    while (s_current != self) self->cv.wait_for(lk, std::chrono::milliseconds(1));
    self->where = "has its turn";
}

// Tries to satisfy t's wait; true (and the objects taken) if it can go on.
bool TryWake(Thread* t)
{
    WaitState& w = t->wait;
    if (w.arbiter) return false;
    if (w.all) {
        for (auto& o : w.objects)
            if (o->ShouldWait(t)) return false;
        for (auto& o : w.objects) o->Acquire(t);
        w.index = -1;
    } else {
        s32 hit = -1;
        for (size_t i = 0; i < w.objects.size(); ++i)
            if (!w.objects[i]->ShouldWait(t)) { hit = (s32)i; break; }
        if (hit < 0) return false;
        w.objects[hit]->Acquire(t);
        w.index = hit;
    }
    w.result = RESULT_OK;
    return true;
}

void MakeReady(Thread* t)
{
    for (auto& o : t->wait.objects) {
        auto& v = o->waiters;
        v.erase(std::remove(v.begin(), v.end(), t), v.end());
    }
    t->wait.objects.clear();
    t->wait.deadline = -1;
    t->wait.arbiter = false;
    t->state = Thread::READY;
    t->readySeq = ++s_seq;
    if (!s_current) Reschedule();  // the system was idle
    else UpdatePreempt();
}

void Status(bool full)
{
    static s64 last = 0;
    const s64 now = Now(), span = last ? now - last : 0;
    last = now;
    if (span > 0) {
        std::string share;
        for (auto& t : s_threads) {
            if (t->runNs * 100 >= span || t->sleeps) {
                share += " t" + std::to_string(t->id) + ":" + std::to_string(t->runNs * 100 / span) + "%";
                if (t->sleeps) share += "/" + std::to_string(t->sleeps) + "z";
            }
            t->runNs = 0;
            t->sleeps = 0;
        }
        Log(LOG_INFO, "status", "busy%s, idle %lld%%; %lld sleeps averaging %lld us asked, %lld us taken", share.c_str(),
            (long long)(s_idleNs * 100 / span), (long long)s_sleeps, (long long)(s_sleeps ? s_sleepAskedNs / s_sleeps / 1000 : 0),
            (long long)(s_sleeps ? s_sleepTookNs / s_sleeps / 1000 : 0));
        static const char* names[PROF_COUNT] = {"gpu lists", "fills", "display transfers", "texture copies", "y2r"};
        std::string host;
        for (int i = 0; i < PROF_COUNT; ++i) {
            if (g_profNs[i] * 100 >= span) host += std::string(" ") + names[i] + " " + std::to_string(g_profNs[i] * 100 / span) + "%";
            g_profNs[i] = 0;
        }
        if (!host.empty() && full) Log(LOG_INFO, "status", "host work:%s", host.c_str());
        // DS2RB_PEEK=addr:words[,addr:words...]: guest memory as floats.
        if (const char* pk = std::getenv("DS2RB_PEEK")) {
            std::string spec = pk;
            size_t at = 0;
            while (at < spec.size()) {
                size_t comma = spec.find(',', at);
                if (comma == std::string::npos) comma = spec.size();
                const std::string item = spec.substr(at, comma - at);
                at = comma + 1;
                const size_t colon = item.find(':');
                const u32 addr = (u32)std::strtoul(item.substr(0, colon).c_str(), nullptr, 16);
                const u32 words = colon == std::string::npos ? 16 : (u32)std::strtoul(item.substr(colon + 1).c_str(), nullptr, 0);
                std::string line;
                for (u32 i = 0; i < words && mem::IsMapped(addr + 4 * i, 4); ++i) {
                    const u32 w = mem::R32(addr + 4 * i);
                    float f;
                    std::memcpy(&f, &w, 4);
                    char b[48];
                    std::snprintf(b, sizeof b, "%s%08X(%g)", i % 4 ? " " : "\n    ", w, f);
                    line += b;
                }
                Log(LOG_INFO, "peek", "%08X:%s", addr, line.c_str());
            }
        }
        const std::string tallies = TakeTallies();
        if (!tallies.empty() && full) Log(LOG_INFO, "status", "events:%s", tallies.c_str());
        s_idleNs = 0;
        s_sleeps = s_sleepAskedNs = s_sleepTookNs = 0;
    }
    if (!full) return;
    Log(LOG_INFO, "status", "the turn: %s", s_current ? ("thread " + std::to_string(s_current->id) + (s_current->state == Thread::DEAD ? " (dead)" : "")).c_str() : "nobody");
    for (auto& t : s_threads) {
        if (t->state == Thread::DEAD) continue;
        static const char* st[] = {"ready", "running", "waiting", "dead"};
        std::string what;
        for (auto& o : t->wait.objects) {
            what += " " + (o->name.empty() ? "object of type " + std::to_string((int)o->type) : o->name);
            if (o->type == Type::Mutex) {
                auto m = std::static_pointer_cast<Mutex>(o);
                what += m->owner ? " (owned by thread " + std::to_string(m->owner->id) + ")" : " (free)";
            }
        }
        if (t->wait.arbiter) {
            char b[64];
            std::snprintf(b, sizeof b, " arbiter at %08X (value %d)", t->wait.arbiterAddr, (s32)mem::R32(t->wait.arbiterAddr));
            what += b;
        }
        Log(LOG_INFO, "status", "thread %u prio %d %s in %08X lr %08X sp %08X%s%s [host %X: %s]", t->id, t->priority, st[t->state],
            t->cpu.r[15], t->cpu.r[14], t->cpu.r[13], what.empty() ? "" : " on", what.c_str(), t->osThread, t->where);
    }
}

void TimerLoop()
{
    SetThreadRole(ThreadRole::Timing);
    std::unique_lock<std::mutex> lk(s_lock);
    s64 nextStatus = Now() + 5000000000LL;
    while (!s_exited) {
        s64 next = -1;
        const s64 now = Now();
        if (now >= nextStatus) {
            // Every 5 s (guest time): with -v the full status, otherwise every
            // other time a one-line summary of how busy the guest threads are.
            static int n = 0;
            if (g_logLevel >= LOG_DEBUG || ++n % 2 == 0) Status(g_logLevel >= LOG_DEBUG);
            nextStatus = now + 5000000000LL;
        }
        for (auto& t : s_threads) {
            if (t->state != Thread::WAITING || t->wait.deadline < 0) continue;
            if (t->wait.deadline <= now) {
                t->wait.result = RESULT_TIMEOUT;
                t->wait.index = -1;
                MakeReady(t.get());
            } else if (next < 0 || t->wait.deadline < next) {
                next = t->wait.deadline;
            }
        }
        if (!s_current && next < 0) {
            // Nothing runs and nothing will wake by itself: report after a while.
            if (s_timerCv.wait_for(lk, std::chrono::seconds(3)) == std::cv_status::timeout && !s_current) {
                Log(LOG_WARN, "kernel", "every thread is waiting:");
                for (auto& t : s_threads) {
                    if (t->state == Thread::DEAD) continue;
                    std::string what;
                    for (auto& o : t->wait.objects) what += " " + o->name;
                    if (t->wait.arbiter) what += " arbiter@" + std::to_string(t->wait.arbiterAddr);
                    Log(LOG_WARN, "kernel", "  thread %u prio %d in %08X lr %08X waits on%s", t->id, t->priority,
                        t->cpu.r[15], t->cpu.r[14], what.empty() ? " (nothing)" : what.c_str());
                }
            }
            continue;
        }
        if (next < 0 || next > nextStatus) next = nextStatus;
        if (next < 0) {
            s_timerCv.wait(lk);
            continue;
        }
        const s64 waitNs = (s64)((next - now) / g_speed);
#ifdef _WIN32
        // Windows timed waits come in whole milliseconds or worse, and games
        // sleep for half a millisecond in loops (the movie player does, over
        // a thousand times a second): the last stretch is waited out by
        // spinning, so a 0.5 ms sleep takes 0.5 ms and not 2.
        constexpr s64 kSpin = 2000000;
        if (waitNs <= kSpin) {
            lk.unlock();
            const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(waitNs);
            while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
            lk.lock();
            continue;
        }
        s_timerCv.wait_for(lk, std::chrono::nanoseconds(waitNs - kSpin));
#else
        s_timerCv.wait_for(lk, std::chrono::nanoseconds(waitNs));
#endif
    }
}

void EndThread(Thread* t, std::unique_lock<std::mutex>& lk);

void SetHostFloatMode()
{
#if defined(__x86_64__) || defined(_M_X64)
    // VFP RunFast: denormals flush to zero (FTZ, DAZ).
    unsigned csr;
    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    csr |= 0x8040;
    __asm__ volatile("ldmxcsr %0" : : "m"(csr));
#endif
}

void HostThreadMain(Thread* t)
{
#ifdef _WIN32
    t->osThread = (u32)GetCurrentThreadId();
#else
    t->osThread = (u32)syscall(SYS_gettid);
#endif
    SetHostFloatMode();
    SetThreadRole(ThreadRole::Game);
    std::unique_lock<std::mutex> lk(s_lock);
    WaitTurn(lk, t);
    lk.unlock();
    Log(LOG_DEBUG, "kernel", "thread %u runs on host thread %X", t->id, t->osThread);
    if (setjmp(t->exitJump) != 0) {  // svcExitThread: handled there
        Log(LOG_DEBUG, "kernel", "thread %u (host %X) exited; kernel lock %s", t->id, t->osThread, lk.owns_lock() ? "HELD" : "free");
        return;
    }
    rc_call(&t->cpu, t->entry);
    lk.lock();
    Log(LOG_DEBUG, "kernel", "thread %u returns", t->id);
    EndThread(t, lk);
    if (t->id == 1) {  // the main thread returned: the process ends
        s_exited = true;
        s_exitCv.notify_all();
        s_timerCv.notify_all();
        return;
    }
    Reschedule();
}

std::shared_ptr<Thread> NewThread(u32 entry, u32 arg, u32 stackTop, s32 prio, s32 core)
{
    auto t = std::make_shared<Thread>();
    t->id = s_nextThreadId++;
    t->entry = entry;
    t->arg = arg;
    t->stackTop = stackTop;
    t->priority = prio;
    t->core = core;
    t->tls = mem::TLS + (t->id - 1) * mem::TLS_SIZE;
    std::memset(mem::Ptr(t->tls), 0, mem::TLS_SIZE);
    t->cpu.r[0] = arg;
    t->cpu.r[13] = stackTop & ~7u;
    t->cpu.tls = t->tls;
    t->cpu.thread = t.get();
    t->cpu.fpscr = 0x03C00000;  // the 3DS runs VFP in RunFast mode (flush to zero, default NaN)
    t->state = Thread::READY;
    t->readySeq = ++s_seq;
    t->name = "thread " + std::to_string(t->id);
    s_threads.push_back(t);
    t->host = std::thread(HostThreadMain, t.get());
    t->host.detach();
    return t;
}

Thread* Self(RcCpu* c) { return (Thread*)c->thread; }

// A thread ends: it is signalled, and the kernel releases the mutexes it
// still holds.
void EndThread(Thread* t, std::unique_lock<std::mutex>& lk)
{
    t->state = Thread::DEAD;
    Signal(t, lk);
    std::vector<ObjectPtr> held;
    for (auto& [h, o] : s_handles)
        if (o->type == Type::Mutex && std::static_pointer_cast<Mutex>(o)->owner == t) held.push_back(o);
    for (auto& o : held) {
        auto m = std::static_pointer_cast<Mutex>(o);
        if (m->owner != t) continue;
        m->owner = nullptr;
        m->count = 0;
        Signal(m.get(), lk);
    }
}

u32 Handle(Thread* t, u32 h, ObjectPtr* out)
{
    *out = h == CURRENT_THREAD ? nullptr : Get(h);
    if (h == CURRENT_THREAD) {
        for (auto& x : s_threads)
            if (x.get() == t) *out = x;
    }
    return *out ? RESULT_OK : RESULT_INVALID_HANDLE;
}

}  // namespace

void LogStatus()
{
    auto lk = Lock();
    Status(true);
}

std::unique_lock<std::mutex> Lock() { return std::unique_lock<std::mutex>(s_lock); }

}  // namespace kernel
}  // namespace rt

extern "C" volatile int rc_preempt = 0;

// A loop in guest code saw rc_preempt: hand over to the better thread.
extern "C" void rc_yield(RcCpu* c)
{
    auto lk = rt::kernel::Lock();
    if ((rt::kernel::Thread*)c->thread != rt::kernel::Current()) {
        static int n = 0;
        if (n++ < 20)
            rt::Log(rt::LOG_ERROR, "kernel", "thread %u yields while thread %u has the turn", ((rt::kernel::Thread*)c->thread)->id,
                    rt::kernel::Current() ? rt::kernel::Current()->id : 0);
    }
    rt::kernel::Preempt(lk);
}

namespace rt {
namespace kernel {
Thread* Current() { return s_current; }
s64 NowNs() { return Now(); }

u32 NewHandle(ObjectPtr o)
{
    const u32 h = s_nextHandle++;
    s_handles[h] = std::move(o);
    return h;
}

ObjectPtr Get(u32 handle)
{
    auto it = s_handles.find(handle);
    return it == s_handles.end() ? nullptr : it->second;
}

bool CloseHandle(u32 handle) { return s_handles.erase(handle) != 0; }

void Signal(Object* o, std::unique_lock<std::mutex>&)
{
    // Highest priority first.
    std::vector<Thread*> ws = o->waiters;
    std::sort(ws.begin(), ws.end(), [](Thread* a, Thread* b) { return Better(a, b); });
    for (Thread* t : ws) {
        if (t->state != Thread::WAITING) continue;
        if (TryWake(t)) MakeReady(t);
    }
}

void SignalEvent(const std::shared_ptr<Event>& e)
{
    auto lk = Lock();
    e->signaled = true;
    Signal(e.get(), lk);
    if (e->reset == PULSE) e->signaled = false;
}

u32 WaitObjects(std::unique_lock<std::mutex>& lk, std::vector<ObjectPtr> objs, bool all, s64 timeoutNs, s32* index)
{
    Thread* t = s_current;
    t->wait = WaitState{};
    t->wait.objects = std::move(objs);
    t->wait.all = all;
    if (TryWake(t)) {
        if (index) *index = t->wait.index;
        t->wait.objects.clear();
        return RESULT_OK;
    }
    if (timeoutNs == 0) {
        t->wait.objects.clear();
        return RESULT_TIMEOUT;
    }
    for (auto& o : t->wait.objects) o->waiters.push_back(t);
    t->wait.deadline = timeoutNs < 0 ? -1 : Now() + timeoutNs;
    t->state = Thread::WAITING;
    s_timerCv.notify_all();
    Reschedule();
    WaitTurn(lk, t);
    if (index) *index = t->wait.index;
    return t->wait.result;
}

void Preempt(std::unique_lock<std::mutex>& lk)
{
    Thread* t = s_current;
    Thread* best = PickReady();
    if (best && best != t && Better(best, t) && best->priority < t->priority) {
        Reschedule();
        WaitTurn(lk, t);
    }
}

int RunProcess(u32 entry, u32 stackTop, s32 priority)
{
    std::unique_lock<std::mutex> lk(s_lock);
    s_timerThread = std::thread(TimerLoop);
    NewThread(entry, 0, stackTop, priority, 0);
    Reschedule();
    s_exitCv.wait(lk, [] { return s_exited; });
    lk.unlock();
    s_timerCv.notify_all();
    s_timerThread.join();
    return s_exitCode;
}

// ---------------------------------------------------------------- svc

namespace {

u32 ControlMemory(RcCpu* c)
{
    const u32 op = c->r[0], addr0 = c->r[1], size = c->r[3];
    const u32 kind = op & 0xFF;
    const bool linear = (op & 0x10000) != 0;
    if (kind == 3) {  // COMMIT
        u32 at = addr0;
        if (linear) {
            at = s_linearEnd;
            s_linearEnd += (size + 0xFFF) & ~0xFFFu;
        } else if (at == 0) {
            at = s_heapEnd;
        }
        if (!mem::Map(at, size)) return RESULT_OUT_OF_MEMORY;
        if (!linear && at + size > s_heapEnd) s_heapEnd = at + size;
        c->r[1] = at;
        Log(LOG_INFO, "kernel", "ControlMemory commit %08X size %X%s", at, size, linear ? " (linear)" : "");
        return RESULT_OK;
    }
    if (kind == 1) {  // FREE
        mem::Unmap(addr0, size);
        return RESULT_OK;
    }
    if (kind == 4) {  // MAP addr1 at addr0. The SDK moves memory this way and
        // makes the original inaccessible, so a copy behaves like the alias.
        const u32 from = c->r[2];
        if (!mem::Map(addr0, size)) return RESULT_INVALID_ADDRESS;
        std::memcpy(mem::Ptr(addr0), mem::Ptr(from), size);
        s_aliases[addr0] = {from, size};
        Log(LOG_DEBUG, "kernel", "ControlMemory map %08X at %08X size %X", from, addr0, size);
        return RESULT_OK;
    }
    if (kind == 5) {  // UNMAP: the contents go back to the original
        auto it = s_aliases.find(addr0);
        if (it != s_aliases.end()) {
            std::memcpy(mem::Ptr(it->second.first), mem::Ptr(addr0), std::min(size, it->second.second));
            s_aliases.erase(it);
        }
        mem::Unmap(addr0, size);
        return RESULT_OK;
    }
    if (kind == 6) return RESULT_OK;  // PROTECT: ignored
    Log(LOG_WARN, "kernel", "ControlMemory op %X addr0 %08X addr1 %08X size %X perm %X not handled", op, addr0, c->r[2], size, c->r[4]);
    return RESULT_OK;
}

u32 ArbitrateAddress(RcCpu* c, std::unique_lock<std::mutex>& lk)
{
    const u32 addr = c->r[1], type = c->r[2];
    const s32 value = (s32)c->r[3];
    const s64 timeout = (s64)((u64)c->r[5] << 32 | c->r[4]);
    Thread* self = s_current;
    if (type == 0) {  // signal: wake `value` waiters (all if < 0), highest priority first
        std::vector<Thread*> ws;
        for (auto& t : s_threads)
            if (t->state == Thread::WAITING && t->wait.arbiter && t->wait.arbiterAddr == addr) ws.push_back(t.get());
        std::sort(ws.begin(), ws.end(), [](Thread* a, Thread* b) { return Better(a, b); });
        s32 n = 0;
        for (Thread* t : ws) {
            if (value >= 0 && n >= value) break;
            t->wait.result = RESULT_OK;
            MakeReady(t);
            ++n;
        }
        return RESULT_OK;
    }
    const s32 cur = (s32)mem::R32(addr);
    if (cur >= value) return RESULT_OK;
    if (type == 2 || type == 4) mem::W32(addr, (u32)(cur - 1));
    self->wait = WaitState{};
    self->wait.arbiter = true;
    self->wait.arbiterAddr = addr;
    self->wait.deadline = (type >= 3 && timeout >= 0) ? Now() + timeout : -1;
    self->state = Thread::WAITING;
    s_timerCv.notify_all();
    Reschedule();
    WaitTurn(lk, self);
    return self->wait.result;
}

}  // namespace

void Svc(RcCpu* c, u32 imm)
{
    auto lk = Lock();
    Thread* self = Self(c);
    if (self != s_current) {
        static int n = 0;
        if (n++ < 20)
            Log(LOG_ERROR, "kernel", "thread %u made svc %02X while thread %u has the turn (lr %08X)", self->id, imm,
                s_current ? s_current->id : 0, c->r[14]);
    }
    u32 res = RESULT_OK;
    // DS2RB_TRACE=<seconds>: from then on, log every svc except the noisy ones.
    static const s64 traceFrom = [] {
        const char* e = std::getenv("DS2RB_TRACE");
        return e ? (s64)(std::atof(e) * 1e9) : -1;
    }();
    static const s64 start = Now();
    if (traceFrom >= 0 && Now() - start >= traceFrom && imm != 0x28 && imm != 0x0A)
        Log(LOG_INFO, "trace", "thread %u svc %02X r0 %08X r1 %08X r2 %08X r3 %08X lr %08X", self->id, imm, c->r[0], c->r[1],
            c->r[2], c->r[3], c->r[14]);
    switch (imm) {
    case 0x01:
        res = ControlMemory(c);
        break;
    case 0x03:
        Log(LOG_INFO, "kernel", "ExitProcess");
        s_exited = true;
        s_exitCv.notify_all();
        lk.unlock();
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    case 0x08: {  // CreateThread
        const u32 entry = c->r[1], arg = c->r[2], top = c->r[3];
        const s32 prio = (s32)c->r[0], core = (s32)c->r[4];
        auto t = NewThread(entry, arg, top, prio, core);
        c->r[1] = NewHandle(t);
        Log(LOG_INFO, "kernel", "CreateThread %u entry %08X arg %08X stack %08X prio %d core %d", t->id, entry, arg, top, prio, core);
        Preempt(lk);
        break;
    }
    case 0x09:  // ExitThread
        EndThread(self, lk);
        Reschedule();
        lk.unlock();
        std::longjmp(self->exitJump, 1);
    case 0x0A: {  // SleepThread
        const s64 ns = (s64)((u64)c->r[1] << 32 | c->r[0]);
        if (ns <= 0) {  // yield to threads of the same priority
            self->readySeq = ++s_seq;
            self->state = Thread::READY;
            Reschedule();
            WaitTurn(lk, self);
        } else {
            const s64 t0 = Now();
            WaitObjects(lk, {}, false, ns, nullptr);
            ++s_sleeps;
            ++self->sleeps;
            s_sleepAskedNs += ns;
            s_sleepTookNs += Now() - t0;
        }
        res = RESULT_OK;
        break;
    }
    case 0x0B: {  // GetThreadPriority
        ObjectPtr o;
        res = Handle(self, c->r[1], &o);
        if (!res) c->r[1] = (u32)std::static_pointer_cast<Thread>(o)->priority;
        break;
    }
    case 0x13: {  // CreateMutex
        auto m = std::make_shared<Mutex>();
        if (c->r[1]) { m->owner = self; m->count = 1; }
        c->r[1] = NewHandle(m);
        m->name = "mutex " + std::to_string(c->r[1]);
        break;
    }
    case 0x14: {  // ReleaseMutex
        auto m = GetAs<Mutex>(c->r[0], Type::Mutex);
        if (!m) { res = RESULT_INVALID_HANDLE; break; }
        if (m->owner != self) Log(LOG_WARN, "kernel", "thread %u releases %s it does not own", self->id, m->name.c_str());
        if (m->owner == self && m->count && --m->count == 0) {
            m->owner = nullptr;
            Signal(m.get(), lk);
            Preempt(lk);
        }
        break;
    }
    case 0x15: {  // CreateSemaphore
        auto s = std::make_shared<Semaphore>();
        s->count = (s32)c->r[1];
        s->max = (s32)c->r[2];
        c->r[1] = NewHandle(s);
        s->name = "semaphore " + std::to_string(c->r[1]);
        break;
    }
    case 0x16: {  // ReleaseSemaphore
        auto s = GetAs<Semaphore>(c->r[1], Type::Semaphore);
        if (!s) { res = RESULT_INVALID_HANDLE; break; }
        const s32 old = s->count;
        s->count = std::min(s->max, s->count + (s32)c->r[2]);
        c->r[1] = (u32)old;
        Signal(s.get(), lk);
        Preempt(lk);
        break;
    }
    case 0x17: {  // CreateEvent
        auto e = std::make_shared<Event>();
        e->reset = (ResetType)c->r[1];
        c->r[1] = NewHandle(e);
        e->name = "event " + std::to_string(c->r[1]);
        break;
    }
    case 0x18: {  // SignalEvent
        auto o = Get(c->r[0]);
        if (!o || (o->type != Type::Event && o->type != Type::Timer)) { res = RESULT_INVALID_HANDLE; break; }
        auto e = std::static_pointer_cast<Event>(o);
        e->signaled = true;
        Signal(e.get(), lk);
        if (e->reset == PULSE) e->signaled = false;
        Preempt(lk);
        break;
    }
    case 0x19: {  // ClearEvent
        auto o = Get(c->r[0]);
        if (!o || (o->type != Type::Event && o->type != Type::Timer)) { res = RESULT_INVALID_HANDLE; break; }
        std::static_pointer_cast<Event>(o)->signaled = false;
        break;
    }
    case 0x1E: {  // CreateMemoryBlock: r0 other perm, r1 addr, r2 size, r3 own perm
        auto b = std::make_shared<SharedMemory>();
        b->addr = c->r[1];
        b->size = c->r[2];
        b->name = "game memory block";
        if (!b->addr) Log(LOG_WARN, "kernel", "CreateMemoryBlock with no address (size %X)", b->size);
        c->r[1] = NewHandle(b);
        break;
    }
    case 0x1F: {  // MapMemoryBlock: r0 handle, r1 addr
        auto b = GetAs<SharedMemory>(c->r[0], Type::SharedMemory);
        if (!b) { res = RESULT_INVALID_HANDLE; break; }
        if (b->addr && b->addr != c->r[1]) {
            Log(LOG_WARN, "kernel", "%s mapped again at %08X (already at %08X)", b->name.c_str(), c->r[1], b->addr);
        }
        if (!b->addr) {
            const u32 at = c->r[1];
            if (!mem::Map(at, b->size)) { res = RESULT_INVALID_ADDRESS; break; }
            if (!b->initial.empty()) std::memcpy(mem::Ptr(at), b->initial.data(), std::min<size_t>(b->size, b->initial.size()));
            b->addr = at;
            if (b->onMap) b->onMap(at);
        }
        Log(LOG_INFO, "kernel", "MapMemoryBlock %s at %08X size %X", b->name.c_str(), b->addr, b->size);
        break;
    }
    case 0x20:  // UnmapMemoryBlock
        break;
    case 0x21:  // CreateAddressArbiter
        c->r[1] = NewHandle(std::make_shared<Arbiter>());
        break;
    case 0x22:
        res = ArbitrateAddress(c, lk);
        break;
    case 0x23:  // CloseHandle
        CloseHandle(c->r[0]);
        break;
    case 0x25: {  // WaitSynchronizationN: r1 handles, r2 count, r3 all, r0:r4 timeout
        const u32 list = c->r[1], n = c->r[2];
        const bool all = c->r[3] != 0;
        const s64 timeout = (s64)((u64)c->r[4] << 32 | c->r[0]);
        std::vector<ObjectPtr> objs;
        for (u32 i = 0; i < n; ++i) {
            ObjectPtr o;
            if (Handle(self, mem::R32(list + 4 * i), &o)) { res = RESULT_INVALID_HANDLE; break; }
            objs.push_back(o);
        }
        if (res) break;
        s32 index = -1;
        res = WaitObjects(lk, std::move(objs), all, timeout, &index);
        c->r[1] = (u32)index;
        break;
    }
    case 0x27: {  // DuplicateHandle
        ObjectPtr o;
        res = Handle(self, c->r[1], &o);
        if (!res) c->r[1] = NewHandle(o);
        break;
    }
    case 0x28: {  // GetSystemTick: 268,111,856 Hz
        const u64 ticks = (u64)((double)Now() * 0.268111856);
        c->r[0] = (u32)ticks;
        c->r[1] = (u32)(ticks >> 32);
        return;
    }
    case 0x2D: {  // ConnectToPort
        const std::string port = mem::ReadString(c->r[1], 12);
        res = ipc::ConnectToPort(port, &c->r[1]);
        Log(LOG_INFO, "kernel", "ConnectToPort %s -> %08X", port.c_str(), res);
        break;
    }
    case 0x32:  // SendSyncRequest
        res = ipc::SendSyncRequest(c->r[0], self, lk);
        break;
    case 0x35:  // GetProcessId
        c->r[1] = 0x30;
        break;
    case 0x37: {  // GetThreadId
        ObjectPtr o;
        res = Handle(self, c->r[1], &o);
        if (!res) c->r[1] = std::static_pointer_cast<Thread>(o)->id;
        break;
    }
    case 0x38:  // GetResourceLimit
        c->r[1] = NewHandle(std::make_shared<Object>(Type::ResourceLimit));
        break;
    case 0x3A: {  // GetResourceLimitCurrentValues: r0 out (s64[]), r1 limit, r2 names, r3 count
        for (u32 i = 0; i < c->r[3]; ++i) {
            const u32 name = mem::R32(c->r[2] + 4 * i);
            u64 v = 0;
            if (name == 1) v = s_heapEnd - mem::HEAP + (s_linearEnd - mem::LINEAR);  // commit
            mem::W32(c->r[0] + 8 * i, (u32)v);
            mem::W32(c->r[0] + 8 * i + 4, (u32)(v >> 32));
        }
        break;
    }
    case 0x3C:  // Break
        lk.unlock();
        Fatal("the game stopped itself (svcBreak %u) at %08X", c->r[0], c->r[14]);
    default:
        Log(LOG_WARN, "kernel", "svc %02X not handled (lr %08X)", imm, c->r[14]);
        res = RESULT_NOT_IMPLEMENTED;
        break;
    }
    c->r[0] = res;
}

}  // namespace kernel
}  // namespace rt
