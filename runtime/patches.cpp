// Game code that cannot run as plain recompiled C: it switches stacks.
//
// The script system runs scripts as coroutines (hand-written ARM at
// 0x15C1F0-0x15C248). A context is 10 words: r4-r11, sp, lr. 0x5C57F8 holds
// [0] the resumer's context and [1] the running coroutine's.
//   0x15C214 swap(save, load)  saves the caller into `save`, resumes `load`
//   0x15C230 yield()           saves into [1], resumes [0]
//   0x15C1F0 start             (a new context's lr) runs r4(r5), then
//   0x15C1FC finish            resumes [0] with r0 = 0
// Each guest context runs on its own host fiber, so the C call stack of the
// recompiled code switches along with the guest stack.
//
// 0x16F4C0 runs r1(r2) on the stack r0 with r3 as its return address.
#include "fiber.h"
#include "kernel.h"
#include "rt.h"

#include <map>
#include <vector>

namespace rt {

namespace {

// Where these are depends on the game version (1.0: start 0x15C1F0, state
// 0x5C57F8; 1.1: 0x15C230, 0x5C58D0): found by their instructions at start-up.
u32 CORO_STATE = 0, CORO_START = 0, CORO_FINISH = 0, CORO_SWAP = 0, CORO_YIELD = 0, RUN_ON_STACK = 0;

// Context address -> the fiber suspended in it, and the context's words as
// they were saved. The game may abandon a suspended coroutine and set up a
// new one in the same memory; then the words differ and the old fiber is
// discarded instead of resumed.
struct Suspended {
    Fiber* fiber;
    u32 words[10];
};
std::map<u32, Suspended> s_owner;
std::vector<Fiber*> s_dead;     // finished coroutines, freed once switched away from

void SaveRegs(RcCpu* c, u32 a)
{
    for (int i = 0; i < 8; ++i) mem::W32(a + 4 * i, c->r[4 + i]);
    mem::W32(a + 32, c->r[13]);
    mem::W32(a + 36, c->r[14]);
}

void LoadRegs(RcCpu* c, u32 a)
{
    for (int i = 0; i < 8; ++i) c->r[4 + i] = mem::R32(a + 4 * i);
    c->r[13] = mem::R32(a + 32);
    c->r[14] = mem::R32(a + 36);
}

// Resumes the context at `load` (registers from it), on its fiber, or on a
// new fiber that starts by "returning" to the context's lr. Returns when
// some other context resumes the one the caller saved.
void Suspend(u32 save)
{
    Suspended& s = s_owner[save];
    if (s.fiber && s.fiber != Fiber::Current()) s_dead.push_back(s.fiber);  // abandoned earlier
    s.fiber = Fiber::Current();
    for (int i = 0; i < 10; ++i) s.words[i] = mem::R32(save + 4 * i);
}

void Resume(RcCpu* c, u32 load)
{
    LoadRegs(c, load);
    Fiber* target = nullptr;
    auto it = s_owner.find(load);
    if (it != s_owner.end()) {
        bool same = true;
        for (int i = 0; i < 10 && same; ++i) same = it->second.words[i] == mem::R32(load + 4 * i);
        if (same && it->second.fiber->running) {
            // Suspended there by another host thread that is still on it:
            // cannot be resumed from here. Start the context afresh instead.
            static int logged = 0;
            if (logged++ < 10) Log(LOG_WARN, "patch", "coroutine context %08X belongs to a running fiber", load);
        } else if (same) {
            target = it->second.fiber;
        } else {
            static int logged = 0;
            if (logged++ < 10) Log(LOG_DEBUG, "patch", "coroutine context %08X was set up anew: its old fiber is dropped", load);
            s_dead.push_back(it->second.fiber);
        }
        s_owner.erase(it);
    }
    if (!target) {
        target = Fiber::Create([c] {
            const u32 start = c->r[14];
            rc_call(c, start);
            Fatal("coroutine at %08X returned to its fiber", start);
        });
    }
    target->SwitchTo();
    // Back on this fiber: free coroutines that finished meanwhile.
    for (auto it2 = s_dead.begin(); it2 != s_dead.end();) {
        if (*it2 != Fiber::Current()) {
            Fiber::Destroy(*it2);
            it2 = s_dead.erase(it2);
        } else {
            ++it2;
        }
    }
}

extern "C" void CoroSwap(RcCpu* c)
{
    const u32 save = c->r[0], load = c->r[1];
    mem::W32(CORO_STATE, save);
    mem::W32(CORO_STATE + 4, load);
    SaveRegs(c, save);
    c->r[0] = save + 40;
    c->r[1] = load + 40;
    c->r[12] = CORO_STATE;
    Suspend(save);
    Resume(c, load);
}

extern "C" void CoroYield(RcCpu* c)
{
    const u32 save = mem::R32(CORO_STATE + 4), load = mem::R32(CORO_STATE);
    SaveRegs(c, save);
    c->r[1] = load + 40;
    c->r[12] = CORO_STATE;
    Suspend(save);
    Resume(c, load);
}

extern "C" void CoroFinish(RcCpu* c)
{
    const u32 load = mem::R32(CORO_STATE);
    c->r[0] = 0;
    c->r[1] = load + 40;
    c->r[12] = CORO_STATE;
    s_dead.push_back(Fiber::Current());
    Resume(c, load);
    Fatal("a finished coroutine was resumed");
}

extern "C" void CoroStart(RcCpu* c)
{
    const u32 entry = c->r[4];
    c->r[0] = c->r[5];
    c->r[14] = CORO_FINISH;
    rc_call(c, entry);
    CoroFinish(c);
}

extern "C" void RunOnStack(RcCpu* c)
{
    const u32 fn = c->r[1], cont = c->r[3];
    c->r[13] = c->r[0];
    c->r[0] = c->r[2];
    c->r[14] = cont;
    rc_call(c, fn);
    if (!FindFunction(cont)) Fatal("run-on-stack: the continuation %08X is not a recompiled function", cont);
    rc_call(c, cont);
}

}  // namespace

// The first address in the code whose words match `pattern` (0 = any word).
u32 FindCode(const u32* pattern, int n)
{
    const u32 start = 0x00100000, end = 0x00400000;
    for (u32 a = start; a + 4 * n <= end; a += 4) {
        if (!mem::IsMapped(a, 4 * n)) break;
        bool ok = true;
        for (int i = 0; i < n && ok; ++i) ok = !pattern[i] || mem::R32(a + 4 * i) == pattern[i];
        if (ok) return a;
    }
    return 0;
}

void InstallPatches()
{
    // start: ldr lr,[pc,#20]; mov r0,r5; bx r4; ldr r12,[pc,#12]; mov r0,#0; b swap
    static const u32 coro[] = {0xE59FE014, 0xE1A00005, 0xE12FFF14, 0xE59FC00C, 0xE3A00000, 0xEA00000C};
    // run on stack: mov sp,r0; mov r0,r2; mov lr,r3; bx r1
    static const u32 runOnStack[] = {0xE1A0D000, 0xE1A00002, 0xE1A0E003, 0xE12FFF11};
    CORO_START = FindCode(coro, 6);
    RUN_ON_STACK = FindCode(runOnStack, 4);
    if (!CORO_START || !RUN_ON_STACK) {
        Log(LOG_ERROR, "patch", "the coroutine code was not found: unsupported game version?");
        return;
    }
    CORO_FINISH = CORO_START + 0x0C;
    CORO_STATE = mem::R32(CORO_START + 0x20);  // the literal after start/finish
    CORO_SWAP = CORO_START + 0x24;
    CORO_YIELD = CORO_START + 0x40;
    Log(LOG_INFO, "patch", "coroutines at %08X (state %08X), run-on-stack at %08X", CORO_START, CORO_STATE, RUN_ON_STACK);
    struct { u32 addr; RcFunc fn; } patches[] = {
        {CORO_START, CoroStart}, {CORO_FINISH, CoroFinish}, {CORO_SWAP, CoroSwap},
        {CORO_YIELD, CoroYield}, {RUN_ON_STACK, RunOnStack},
    };
    for (auto& p : patches)
        if (!Override(p.addr, p.fn)) Log(LOG_WARN, "patch", "no function at %08X to replace", p.addr);
}

}  // namespace rt
