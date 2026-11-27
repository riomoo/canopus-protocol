// The parts of the 3DS kernel the game uses: handles, threads, the
// application-core scheduler and synchronisation objects.
//
// Every guest thread is a host thread, but only one runs guest code at a
// time: the highest-priority ready thread (lower number first, first come
// first served among equals), as on the 3DS application core, where a
// thread runs until it waits or a higher-priority thread becomes ready.
// Here a switch can only happen at a kernel call; host-side events (timers,
// the GPU's interrupts) make threads ready and the running thread hands over
// at its next kernel call.
#pragma once

#include "rt.h"

#include <condition_variable>
#include <csetjmp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rt {
namespace kernel {

// Result codes.
constexpr u32 RESULT_OK = 0;
constexpr u32 RESULT_TIMEOUT = 0x09401BFE;
constexpr u32 RESULT_INVALID_HANDLE = 0xD8E007F7;
constexpr u32 RESULT_NOT_FOUND = 0xD88007FA;
constexpr u32 RESULT_OUT_OF_MEMORY = 0xD86007F3;
constexpr u32 RESULT_INVALID_ADDRESS = 0xE0E01BF5;
constexpr u32 RESULT_NOT_IMPLEMENTED = 0xF8C007F4;

constexpr u32 CURRENT_THREAD = 0xFFFF8000;
constexpr u32 CURRENT_PROCESS = 0xFFFF8001;

struct Thread;

enum class Type { Thread, Mutex, Semaphore, Event, Timer, Arbiter, SharedMemory, Session, Port, Process, ResourceLimit };

enum ResetType { ONESHOT = 0, STICKY = 1, PULSE = 2 };

struct Object {
    explicit Object(Type t) : type(t) {}
    virtual ~Object() = default;
    Type type;
    std::string name;
    std::vector<Thread*> waiters;  // threads waiting on this object
    // Waitable objects: whether a wait by t must block, and what taking
    // the object does (a mutex is owned, a one-shot event is cleared).
    virtual bool ShouldWait(Thread*) const { return true; }
    virtual void Acquire(Thread*) {}
};
using ObjectPtr = std::shared_ptr<Object>;

struct WaitState {
    std::vector<ObjectPtr> objects;
    bool all = false;
    s64 deadline = -1;      // host time in ns, -1 = none
    u32 arbiterAddr = 0;    // waiting on an address arbiter at this address
    bool arbiter = false;
    u32 result = RESULT_OK;
    s32 index = -1;
};

struct Thread : Object {
    Thread() : Object(Type::Thread) {}
    enum State { READY, RUNNING, WAITING, DEAD } state = READY;
    RcCpu cpu{};
    u32 id = 0;
    s32 priority = 0x30;
    s32 core = 0;
    u32 entry = 0, arg = 0, stackTop = 0;
    u32 tls = 0;
    u64 readySeq = 0;  // order among threads of the same priority
    s64 runNs = 0;     // time spent holding the baton (diagnostics)
    u32 sleeps = 0;    // SleepThread calls since the last status (diagnostics)
    const char* volatile where = "new";  // what its host thread is doing (diagnostics)
    u32 osThread = 0;
    WaitState wait;
    std::condition_variable cv;
    std::thread host;
    std::jmp_buf exitJump;
    bool ShouldWait(Thread*) const override { return state != DEAD; }
};

struct Mutex : Object {
    Mutex() : Object(Type::Mutex) {}
    Thread* owner = nullptr;
    u32 count = 0;
    bool ShouldWait(Thread* t) const override { return owner && owner != t; }
    void Acquire(Thread* t) override { owner = t; ++count; }
};

struct Semaphore : Object {
    Semaphore() : Object(Type::Semaphore) {}
    s32 count = 0, max = 0;
    bool ShouldWait(Thread*) const override { return count <= 0; }
    void Acquire(Thread*) override { --count; }
};

struct Event : Object {
    Event() : Object(Type::Event) {}
    bool signaled = false;
    ResetType reset = ONESHOT;
    bool ShouldWait(Thread*) const override { return !signaled; }
    void Acquire(Thread*) override { if (reset == ONESHOT) signaled = false; }
};

struct Timer : Event {
    Timer() { type = Type::Timer; }
    s64 next = -1;      // host ns of the next expiry, -1 = stopped
    s64 interval = 0;
};

struct Arbiter : Object {
    Arbiter() : Object(Type::Arbiter) {}
};

// A memory block: either one the game made from its own memory (owner =
// game, at addr), or one a service made (the service's data; mapped where
// the game asks).
struct SharedMemory : Object {
    SharedMemory() : Object(Type::SharedMemory) {}
    u32 size = 0;
    u32 addr = 0;  // where it is mapped in the game's space (0 = not yet)
    std::vector<u8> initial;
    std::function<void(u32 addr)> onMap;
    u8* Ptr(u32 off = 0) const { return addr ? g_mem + addr + off : nullptr; }
};

// ---- the kernel lock and scheduler ----
// Every kernel structure is protected by Lock(); a guest thread holds the
// baton (is Current()) while it runs guest code, without the lock.
std::unique_lock<std::mutex> Lock();
// Logs every thread and what it waits on (diagnostics).
void LogStatus();
Thread* Current();
s64 NowNs();  // host monotonic clock

// Handles of the (single) process.
u32 NewHandle(ObjectPtr o);
ObjectPtr Get(u32 handle);
template <typename T> std::shared_ptr<T> GetAs(u32 handle, Type t)
{
    ObjectPtr o = Get(handle);
    return o && o->type == t ? std::static_pointer_cast<T>(o) : nullptr;
}
bool CloseHandle(u32 handle);

// Wakes the threads waiting on o that can now take it (call with the lock).
void Signal(Object* o, std::unique_lock<std::mutex>& lk);
// Signals an event from outside guest code (a service, a host timer).
void SignalEvent(const std::shared_ptr<Event>& e);

// Blocks the current thread on objects (lock held, released while waiting).
u32 WaitObjects(std::unique_lock<std::mutex>& lk, std::vector<ObjectPtr> objs, bool all, s64 timeoutNs, s32* index);

// Starts the main thread at the entry point and runs until the process exits.
int RunProcess(u32 entry, u32 stackTop, s32 priority);

// Called from a guest kernel call before returning to guest code: hands over
// if a higher-priority thread became ready.
void Preempt(std::unique_lock<std::mutex>& lk);

void Svc(RcCpu* c, u32 imm);

}  // namespace kernel
}  // namespace rt
