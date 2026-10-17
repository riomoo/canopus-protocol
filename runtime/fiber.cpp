#include "fiber.h"
#include "rt.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <ucontext.h>
#include <cstdlib>
#endif

namespace rt {

#ifdef _WIN32

struct Fiber::Impl {
    void* handle = nullptr;
    bool converted = false;
};

namespace {
thread_local Fiber* t_current = nullptr;

void WINAPI Start(void* p)
{
    Fiber* f = static_cast<Fiber*>(p);
    f->fn();
    Fatal("a fiber returned");
}
}  // namespace

Fiber* Fiber::Current()
{
    if (!t_current) {
        t_current = new Fiber;
        t_current->impl = new Impl;
        t_current->impl->handle = ConvertThreadToFiber(nullptr);
        t_current->impl->converted = true;
        t_current->threadMain = true;
        t_current->running = true;
    }
    return t_current;
}

Fiber* Fiber::Create(std::function<void()> fn, size_t stackSize)
{
    Current();
    Fiber* f = new Fiber;
    f->fn = std::move(fn);
    f->impl = new Impl;
    // Reserved, committed as it grows: deep recompiled call chains need room.
    f->impl->handle = CreateFiberEx(64 * 1024, stackSize, FIBER_FLAG_FLOAT_SWITCH, Start, f);
    if (!f->impl->handle) Fatal("cannot create a fiber");
    return f;
}

void Fiber::SwitchTo()
{
    Fiber* from = Current();
    if (from == this) return;
    from->running = false;
    running = true;
    t_current = this;
    SwitchToFiber(impl->handle);
}

void Fiber::Destroy(Fiber* f)
{
    if (!f || f == t_current || f->threadMain || f->running) return;
    if (f->impl->handle && !f->impl->converted) DeleteFiber(f->impl->handle);
    delete f->impl;
    delete f;
}

#else

struct Fiber::Impl {
    ucontext_t ctx;
    void* stack = nullptr;
    void* mapped = nullptr;
    size_t mappedSize = 0;
};

namespace {
thread_local Fiber* t_current = nullptr;

void Start(unsigned hi, unsigned lo)
{
    Fiber* f = reinterpret_cast<Fiber*>((uintptr_t)hi << 32 | lo);
    f->fn();
    Fatal("a fiber returned");
}
}  // namespace

Fiber* Fiber::Current()
{
    if (!t_current) {
        t_current = new Fiber;
        t_current->impl = new Impl;
        t_current->threadMain = true;
        t_current->running = true;
    }
    return t_current;
}

Fiber* Fiber::Create(std::function<void()> fn, size_t stackSize)
{
    Current();
    Fiber* f = new Fiber;
    f->fn = std::move(fn);
    f->impl = new Impl;
    // Mapped lazily, with a guard page below it: an overflow faults instead
    // of quietly corrupting the heap.
    void* m = mmap(nullptr, stackSize + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) Fatal("cannot map a fiber stack");
    mprotect(m, 4096, PROT_NONE);
    f->impl->mapped = m;
    f->impl->mappedSize = stackSize + 4096;
    f->impl->stack = (char*)m + 4096;
    getcontext(&f->impl->ctx);
    f->impl->ctx.uc_stack.ss_sp = f->impl->stack;
    f->impl->ctx.uc_stack.ss_size = stackSize;
    f->impl->ctx.uc_link = nullptr;
    const uintptr_t p = reinterpret_cast<uintptr_t>(f);
    makecontext(&f->impl->ctx, (void (*)())Start, 2, (unsigned)(p >> 32), (unsigned)p);
    return f;
}

void Fiber::SwitchTo()
{
    Fiber* from = Current();
    if (from == this) return;
    from->running = false;
    running = true;
    t_current = this;
    swapcontext(&from->impl->ctx, &impl->ctx);
}

void Fiber::Destroy(Fiber* f)
{
    if (!f || f == t_current || f->threadMain || f->running) return;
    if (f->impl->mapped) munmap(f->impl->mapped, f->impl->mappedSize);
    delete f->impl;
    delete f;
}

#endif

}  // namespace rt
