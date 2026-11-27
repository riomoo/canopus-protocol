// Host fibers: separate C stacks within one host thread, switched by hand.
// Guest code that swaps its own stack (coroutines) runs each guest context
// on a fiber, so the recompiled C call stack follows the guest's.
#pragma once

#include <cstddef>
#include <atomic>
#include <functional>

namespace rt {

class Fiber {
public:
    // The fiber of the calling host thread (made on first use).
    static Fiber* Current();
    // A new fiber that runs fn when first switched to. fn must not return:
    // it switches away for good instead.
    static Fiber* Create(std::function<void()> fn, size_t stackSize = 64u << 20);
    void SwitchTo();  // from the current fiber of this host thread to this one
    // Frees a fiber that will never run again. A thread's own (first) fiber
    // and a fiber running on any thread are never freed: freeing those ends
    // or corrupts a host thread.
    static void Destroy(Fiber* f);

    struct Impl;
    Impl* impl = nullptr;
    std::function<void()> fn;
    bool threadMain = false;              // the fiber a host thread started on
    std::atomic<bool> running{false};     // on some host thread right now
};

}  // namespace rt
