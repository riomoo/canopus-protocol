/* recomp.h: what the generated C needs. The generated files are made on the
 * player's machine from their own code.bin; this header is part of the
 * recompiler. Guest memory is one flat 4 GB reservation at g_mem; guest
 * address a is at g_mem + a. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RcCpu {
    uint32_t r[16];
    uint32_t n, z, c, v, q; /* flags, each 0 or 1 */
    uint32_t ge;            /* GE[3:0] */
    union {
        float s[32];
        double d[16];
        uint32_t w[32];
    } f;
    uint32_t fpscr;
    uint32_t tls; /* thread-local storage base (CP15 c13 c0 3) */
    void* thread; /* runtime's thread */
    /* The exclusive monitor: what LDREX read, for STREX to compare. */
    uint32_t excl_addr; /* RC_NO_EXCL when clear */
    uint64_t excl_val;
} RcCpu;

#define RC_NO_EXCL 0xFFFFFFFFu

typedef void (*RcFunc)(RcCpu*);
/* Code with computed jumps: enterable at any instruction in [start, end). */
typedef struct { uint32_t start, end; void (*fn)(RcCpu*, uint32_t); } RcDyn;

extern uint8_t* g_mem;

/* Runtime entry points (the runtime provides these). */
void rc_call(RcCpu* c, uint32_t addr);           /* indirect call through the function table */
void rc_svc(RcCpu* c, uint32_t imm);             /* kernel call */
void rc_bkpt(RcCpu* c, uint32_t pc, uint32_t imm);
void rc_trap(RcCpu* c, uint32_t pc, uint32_t raw); /* instruction the recompiler could not translate */
void rc_unreachable(RcCpu* c, uint32_t pc);     /* flow ran past what was decoded */
void rc_badreturn(RcCpu* c, uint32_t pc, uint32_t to, uint32_t lr); /* a return to somewhere else than the caller */
void rc_cp15_write(RcCpu* c, uint32_t pc, uint32_t raw, uint32_t value);
uint32_t rc_cp15_read(RcCpu* c, uint32_t pc, uint32_t raw);

/* ---- memory ---- */
static inline uint32_t ld32(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
static inline uint32_t ld16(uint32_t a) { uint16_t v; memcpy(&v, g_mem + a, 2); return v; }
static inline uint32_t lds16(uint32_t a) { int16_t v; memcpy(&v, g_mem + a, 2); return (uint32_t)(int32_t)v; }
static inline uint32_t ld8(uint32_t a) { return g_mem[a]; }
static inline uint32_t lds8(uint32_t a) { return (uint32_t)(int32_t)(int8_t)g_mem[a]; }
static inline void st32(uint32_t a, uint32_t v) { memcpy(g_mem + a, &v, 4); }

/* LDREX/STREX. Guest threads switch only at known points, never between
 * the two, but host threads (the GPU, VBlank and audio threads) write shared
 * memory at any time: STREX is a compare-and-swap against what LDREX read,
 * so an update in between makes it fail and the guest retry. */
extern int rc_excl_cas; /* 0: STREX always succeeds (diagnostics) */
#define RC_EXCL(bits, type) \
    static inline uint32_t rc_ldrex##bits(RcCpu* c, uint32_t a) \
    { \
        type v = __atomic_load_n((type*)(g_mem + a), __ATOMIC_SEQ_CST); \
        c->excl_addr = a; \
        c->excl_val = v; \
        return (uint32_t)v; \
    } \
    static inline uint32_t rc_strex##bits(RcCpu* c, uint32_t a, uint32_t v) \
    { \
        if (!rc_excl_cas) { type x = (type)v; memcpy(g_mem + a, &x, sizeof x); return 0; } \
        if (c->excl_addr != a) return 1; \
        type expected = (type)c->excl_val; \
        c->excl_addr = RC_NO_EXCL; \
        return __atomic_compare_exchange_n((type*)(g_mem + a), &expected, (type)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1; \
    }
RC_EXCL(8, uint8_t)
RC_EXCL(16, uint16_t)
RC_EXCL(32, uint32_t)
static inline void rc_ldrexd(RcCpu* c, uint32_t a, uint32_t* lo, uint32_t* hi)
{
    uint64_t v = __atomic_load_n((uint64_t*)(g_mem + a), __ATOMIC_SEQ_CST);
    c->excl_addr = a;
    c->excl_val = v;
    *lo = (uint32_t)v;
    *hi = (uint32_t)(v >> 32);
}
static inline uint32_t rc_strexd(RcCpu* c, uint32_t a, uint32_t lo, uint32_t hi)
{
    if (c->excl_addr != a) return 1;
    uint64_t expected = c->excl_val;
    c->excl_addr = RC_NO_EXCL;
    return __atomic_compare_exchange_n((uint64_t*)(g_mem + a), &expected, (uint64_t)hi << 32 | lo, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
}
static inline void st16(uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(g_mem + a, &x, 2); }
static inline void st8(uint32_t a, uint32_t v) { g_mem[a] = (uint8_t)v; }

/* ---- flags ---- */
static inline uint32_t rc_add(RcCpu* c, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t s = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)s;
    c->n = r >> 31; c->z = r == 0; c->c = (uint32_t)(s >> 32); c->v = ((a ^ ~b) & (a ^ r)) >> 31;
    return r;
}
/* a - b - !cin (cin = 1: plain subtraction) */
static inline uint32_t rc_sub(RcCpu* c, uint32_t a, uint32_t b, uint32_t cin) {
    return rc_add(c, a, ~b, cin);
}
static inline void rc_nz(RcCpu* c, uint32_t r) { c->n = r >> 31; c->z = r == 0; }

/* ---- shifts ---- */
static inline uint32_t rc_ror(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

/* Shift by register (amount = Rs & 0xFF) with carry out; type 0 LSL, 1 LSR, 2 ASR, 3 ROR. */
static inline uint32_t rc_shift_c(uint32_t cin, uint32_t v, int type, uint32_t amt, uint32_t* cout) {
    amt &= 0xFF;
    *cout = cin;
    if (amt == 0) return v;
    switch (type) {
    case 0:
        if (amt < 32) { *cout = (v >> (32 - amt)) & 1; return v << amt; }
        *cout = amt == 32 ? (v & 1) : 0; return 0;
    case 1:
        if (amt < 32) { *cout = (v >> (amt - 1)) & 1; return v >> amt; }
        *cout = amt == 32 ? (v >> 31) : 0; return 0;
    case 2:
        if (amt < 32) { *cout = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); }
        *cout = v >> 31; return (uint32_t)((int32_t)v >> 31);
    default:
        if ((amt & 31) == 0) { *cout = v >> 31; return v; }
        *cout = (v >> ((amt & 31) - 1)) & 1; return rc_ror(v, amt);
    }
}

/* ---- misc ALU ---- */
static inline uint32_t rc_clz(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }

static inline int32_t rc_sat32(RcCpu* c, int64_t x) {
    if (x > INT32_MAX) { c->q = 1; return INT32_MAX; }
    if (x < INT32_MIN) { c->q = 1; return INT32_MIN; }
    return (int32_t)x;
}
static inline uint32_t rc_ssat(RcCpu* c, int32_t x, uint32_t bits /* 1..32 */) {
    int64_t hi = (bits >= 32) ? INT32_MAX : ((1LL << (bits - 1)) - 1), lo = -hi - 1;
    if (x > hi) { c->q = 1; return (uint32_t)hi; }
    if (x < lo) { c->q = 1; return (uint32_t)lo; }
    return (uint32_t)x;
}
static inline uint32_t rc_usat(RcCpu* c, int32_t x, uint32_t bits /* 0..31 */) {
    int64_t hi = (1LL << bits) - 1;
    if (x > hi) { c->q = 1; return (uint32_t)hi; }
    if (x < 0) { c->q = 1; return 0; }
    return (uint32_t)x;
}

static inline uint32_t rc_cpsr(RcCpu* c) {
    return c->n << 31 | c->z << 30 | c->c << 29 | c->v << 28 | c->q << 27 | (c->ge & 15) << 16 | 0x10;
}
static inline void rc_set_cpsr(RcCpu* c, uint32_t v, uint32_t mask) {
    if (mask & 8) { c->n = v >> 31; c->z = (v >> 30) & 1; c->c = (v >> 29) & 1; c->v = (v >> 28) & 1; c->q = (v >> 27) & 1; }
    if (mask & 4) c->ge = (v >> 16) & 15;
}

/* Parallel add/sub (ARMv6 media). op = prefix<<3 | op2 as the decoder packs it:
 * prefix 1 S, 2 Q, 3 SH, 5 U, 6 UQ, 7 UH; op2 0 ADD16, 1 ASX, 2 SAX, 3 SUB16, 4 ADD8, 7 SUB8. */
static inline int32_t rc_lane(uint32_t v, int i, int w, int sgn) {
    uint32_t x = (v >> (i * w)) & ((1u << w) - 1);
    if (sgn && (x >> (w - 1))) x |= ~((1u << w) - 1);
    return (int32_t)x;
}
static inline uint32_t rc_parallel(RcCpu* c, int op, uint32_t a, uint32_t b) {
    int prefix = op >> 3, op2 = op & 7;
    int sgn = prefix < 4, kind = prefix & 3; /* 1 plain(GE), 2 saturating, 3 halving */
    int w = op2 >= 4 ? 8 : 16, lanes = 32 / w;
    uint32_t r = 0, ge = 0;
    for (int i = 0; i < lanes; i++) {
        int32_t x = rc_lane(a, i, w, sgn), y;
        int sub;
        if (op2 == 1 || op2 == 2) { /* ASX / SAX: cross halves */
            y = rc_lane(b, 1 - i, w, sgn);
            sub = (op2 == 1) ? (i == 0) : (i == 1);
        } else {
            y = rc_lane(b, i, w, sgn);
            sub = op2 == 3 || op2 == 7;
        }
        int64_t s = sub ? (int64_t)x - y : (int64_t)x + y;
        int64_t res = s;
        if (kind == 2) {
            int64_t hi = sgn ? (1LL << (w - 1)) - 1 : (1LL << w) - 1, lo = sgn ? -(1LL << (w - 1)) : 0;
            if (res > hi) res = hi;
            if (res < lo) res = lo;
        } else if (kind == 3) {
            res = s >> 1;
        } else {
            int g = sgn ? s >= 0 : (sub ? s >= 0 : s >= (1LL << w));
            if (g) ge |= (w == 8 ? 1u : 3u) << (i * (w / 8));
        }
        r |= ((uint32_t)res & ((1u << w) - 1)) << (i * w);
    }
    if (kind == 1) c->ge = ge;
    return r;
}
static inline uint32_t rc_sel(RcCpu* c, uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) r |= (((c->ge >> i) & 1) ? a : b) & (0xFFu << (i * 8));
    return r;
}

/* ---- VFP ---- */
static inline void rc_fcmp(RcCpu* c, double a, double b) {
    uint32_t f;
    if (a != a || b != b) f = 0x3;      /* unordered: C V */
    else if (a == b) f = 0x6;           /* Z C */
    else if (a < b) f = 0x8;            /* N */
    else f = 0x2;                       /* C */
    c->fpscr = (c->fpscr & 0x0FFFFFFF) | f << 28;
}
static inline void rc_fpscr_to_flags(RcCpu* c) {
    uint32_t f = c->fpscr >> 28;
    c->n = f >> 3; c->z = (f >> 2) & 1; c->c = (f >> 1) & 1; c->v = f & 1;
}
static inline double rc_round_mode(RcCpu* c, double x) {
    switch ((c->fpscr >> 22) & 3) {
    case 0: return nearbyint(x);
    case 1: return ceil(x);
    case 2: return floor(x);
    default: return trunc(x);
    }
}
static inline uint32_t rc_f2s(double x) {
    if (x != x) return 0;
    if (x >= 2147483647.0) return 0x7FFFFFFF;
    if (x <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)x;
}
static inline uint32_t rc_f2u(double x) {
    if (x != x || x <= 0) return 0;
    if (x >= 4294967295.0) return 0xFFFFFFFFu;
    return (uint32_t)x;
}

#define RC_FUNC(addr) void f_##addr(RcCpu* c)

/* Preemption: the kernel sets rc_preempt when a thread of higher priority
 * than the running one becomes ready; loops check it. */
extern volatile int rc_preempt;
void rc_yield(RcCpu* c);
#define RC_POLL() do { if (rc_preempt) rc_yield(c); } while (0)

/* Debug checks (build with -DRC_CHECK): a function must return with the
 * stack pointer it was called with. */
#ifdef RC_CHECK
void rc_badsp(RcCpu* c, uint32_t entry, uint32_t pc, uint32_t sp0);
#define RC_ENTER(a) const uint32_t sp0 = c->r[13], entry_ = (a)
#define RC_LEAVE(pc) do { if (c->r[13] != sp0) rc_badsp(c, entry_, pc, sp0); } while (0)
#else
#define RC_ENTER(a) ((void)0)
#define RC_LEAVE(pc) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
