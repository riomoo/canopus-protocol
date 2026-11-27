// The PICA200 vertex shader, interpreted. Floats are host floats (the GPU
// uses 24-bit ones); a multiplication of 0 by infinity gives 0 as on the GPU.
#include "pica.h"

#include <algorithm>
#include <cmath>

namespace rt {
namespace pica {

namespace {

inline float Mul(float a, float b)
{
    const float r = a * b;
    if (r != r && a == a && b == b) return 0.0f;  // 0 * inf
    return r;
}

struct Frame {
    u32 end;   // when pc reaches this...
    u32 ret;   // ...continue here (loops: the body start)
    s32 loops; // loop iterations left
    s32 inc;
    bool loop;
};

struct Run {
    const Shader& sh;
    Vec4 v[16], r[16], o[16];
    s32 a0x = 0, a0y = 0, aL = 0;
    bool cmpx = false, cmpy = false;
    Frame stack[16];
    int depth = 0;

    explicit Run(const Shader& s) : sh(s) {}

    // A 7-bit source: inputs, temporaries, or float uniforms (relative to an address register).
    const Vec4& Src(u32 idx, u32 rel)
    {
        if (idx < 0x10) return v[idx];
        if (idx < 0x20) return r[idx - 0x10];
        s32 off = rel == 1 ? a0x : rel == 2 ? a0y : rel == 3 ? aL : 0;
        s32 u = (s32)(idx - 0x20) + off;
        static Vec4 zero{};
        if (u < 0 || u >= 96) return zero;
        return sh.f[u];
    }

    static Vec4 Swizzle(const Vec4& s, u32 desc, u32 shift, bool neg)
    {
        Vec4 out;
        for (int c = 0; c < 4; ++c) {
            const u32 sel = (desc >> (shift + 2 * (3 - c))) & 3;
            out[c] = neg ? -s[sel] : s[sel];
        }
        return out;
    }

    Vec4 Op1(u32 desc, u32 idx, u32 rel) { return Swizzle(Src(idx, rel), desc, 5, desc >> 4 & 1); }
    Vec4 Op2(u32 desc, u32 idx, u32 rel) { return Swizzle(Src(idx, rel), desc, 14, desc >> 13 & 1); }
    Vec4 Op3(u32 desc, u32 idx, u32 rel) { return Swizzle(Src(idx, rel), desc, 23, desc >> 22 & 1); }

    void Dst(u32 dst, u32 desc, const Vec4& val)
    {
        Vec4& d = dst < 0x10 ? o[dst] : r[(dst - 0x10) & 0xF];
        for (int c = 0; c < 4; ++c)
            if (desc >> (3 - c) & 1) d[c] = val[c];
    }

    bool Cond(u32 w)
    {
        const bool refx = w >> 25 & 1, refy = w >> 24 & 1;
        const bool x = cmpx == refx, y = cmpy == refy;
        switch ((w >> 22) & 3) {
        case 0: return x || y;
        case 1: return x && y;
        case 2: return x;
        default: return y;
        }
    }

    static bool Compare(u32 op, float a, float b)
    {
        switch (op) {
        case 0: return a == b;
        case 1: return a != b;
        case 2: return a < b;
        case 3: return a <= b;
        case 4: return a > b;
        case 5: return a >= b;
        default: return true;
        }
    }

    void Push(u32 end, u32 ret, s32 loops = 0, s32 inc = 0, bool loop = false)
    {
        if (depth < 16) stack[depth++] = {end, ret, loops, inc, loop};
    }

    void Exec(u32 pc)
    {
        for (u32 steps = 0; pc < 512 && steps < 100000; ++steps) {
            const u32 w = sh.code[pc];
            const u32 op = w >> 26;
            u32 next = pc + 1;
            if (op < 0x20) {
                const bool inverted = op >= 0x18 && op <= 0x1B;
                const u32 dst = (w >> 21) & 0x1F, rel = (w >> 19) & 3;
                const u32 desc = sh.opdesc[w & 0x7F];
                const u32 s1 = inverted ? (w >> 14) & 0x1F : (w >> 12) & 0x7F;
                const u32 s2 = inverted ? (w >> 7) & 0x7F : (w >> 7) & 0x1F;
                const Vec4 a = Op1(desc, s1, inverted ? 0 : rel);
                const Vec4 b = Op2(desc, s2, inverted ? rel : 0);
                Vec4 d;
                switch (op) {
                case 0x00: for (int c = 0; c < 4; ++c) d[c] = a[c] + b[c]; break;  // ADD
                case 0x01: {  // DP3
                    const float x = Mul(a.x, b.x) + Mul(a.y, b.y) + Mul(a.z, b.z);
                    d = {x, x, x, x};
                    break;
                }
                case 0x02: {  // DP4
                    const float x = Mul(a.x, b.x) + Mul(a.y, b.y) + Mul(a.z, b.z) + Mul(a.w, b.w);
                    d = {x, x, x, x};
                    break;
                }
                case 0x03: case 0x18: {  // DPH, DPHI
                    const float x = Mul(a.x, b.x) + Mul(a.y, b.y) + Mul(a.z, b.z) + b.w;
                    d = {x, x, x, x};
                    break;
                }
                case 0x04: case 0x19: d = {1.0f, Mul(a.y, b.y), a.z, b.w}; break;  // DST, DSTI
                case 0x05: { const float x = std::exp2(a.x); d = {x, x, x, x}; break; }  // EX2
                case 0x06: { const float x = std::log2(a.x); d = {x, x, x, x}; break; }  // LG2
                case 0x07:  // LITP
                    d = {std::max(a.x, 0.0f), std::clamp(a.y, -127.9961f, 127.9961f), 0.0f, std::max(a.w, 0.0f)};
                    cmpx = a.x >= 0;
                    cmpy = a.w >= 0;
                    break;
                case 0x08: for (int c = 0; c < 4; ++c) d[c] = Mul(a[c], b[c]); break;  // MUL
                case 0x09: case 0x1A: for (int c = 0; c < 4; ++c) d[c] = a[c] >= b[c] ? 1.0f : 0.0f; break;  // SGE
                case 0x0A: case 0x1B: for (int c = 0; c < 4; ++c) d[c] = a[c] < b[c] ? 1.0f : 0.0f; break;   // SLT
                case 0x0B: for (int c = 0; c < 4; ++c) d[c] = std::floor(a[c]); break;  // FLR
                case 0x0C: for (int c = 0; c < 4; ++c) d[c] = a[c] > b[c] ? a[c] : b[c]; break;  // MAX
                case 0x0D: for (int c = 0; c < 4; ++c) d[c] = a[c] < b[c] ? a[c] : b[c]; break;  // MIN
                case 0x0E: { const float x = 1.0f / a.x; d = {x, x, x, x}; break; }  // RCP
                case 0x0F: { const float x = 1.0f / std::sqrt(a.x); d = {x, x, x, x}; break; }  // RSQ
                case 0x12:  // MOVA
                    if (desc >> 3 & 1) a0x = (s32)a.x;
                    if (desc >> 2 & 1) a0y = (s32)a.y;
                    pc = next;
                    goto flow;
                case 0x13: d = a; break;  // MOV
                default:
                    pc = next;
                    goto flow;
                }
                Dst(dst, desc, d);
            } else if (op >= 0x30) {  // MAD (0x38+) / MADI (0x30-0x37)
                const bool madi = op < 0x38;
                const u32 dst = (w >> 24) & 0x1F, rel = (w >> 22) & 3;
                const u32 desc = sh.opdesc[w & 0x1F];
                const u32 s1 = (w >> 17) & 0x1F;
                const u32 s2 = madi ? (w >> 12) & 0x1F : (w >> 10) & 0x7F;
                const u32 s3 = madi ? (w >> 5) & 0x7F : (w >> 5) & 0x1F;
                const Vec4 a = Op1(desc, s1, 0);
                const Vec4 b = Op2(desc, s2, madi ? 0 : rel);
                const Vec4 cc = Op3(desc, s3, madi ? rel : 0);
                Vec4 d;
                for (int c = 0; c < 4; ++c) d[c] = Mul(a[c], b[c]) + cc[c];
                Dst(dst, desc, d);
            } else if (op == 0x2E || op == 0x2F) {  // CMP
                const u32 desc = sh.opdesc[w & 0x7F];
                const Vec4 a = Op1(desc, (w >> 12) & 0x7F, (w >> 19) & 3);
                const Vec4 b = Op2(desc, (w >> 7) & 0x1F, 0);
                cmpx = Compare((w >> 24) & 7, a.x, b.x);
                cmpy = Compare((w >> 21) & 7, a.y, b.y);
            } else {
                const u32 num = w & 0xFF, dst = (w >> 10) & 0xFFF;
                switch (op) {
                case 0x20:  // BREAK
                case 0x23:  // BREAKC
                    if (op == 0x20 || Cond(w)) {
                        while (depth > 0 && !stack[depth - 1].loop) --depth;
                        if (depth > 0) next = stack[--depth].end;
                    }
                    break;
                case 0x21: break;           // NOP
                case 0x22: return;          // END
                case 0x24:                  // CALL
                    Push(dst + num, next);
                    next = dst;
                    break;
                case 0x25:  // CALLC
                    if (Cond(w)) { Push(dst + num, next); next = dst; }
                    break;
                case 0x26:  // CALLU
                    if (sh.b >> ((w >> 22) & 0xF) & 1) { Push(dst + num, next); next = dst; }
                    break;
                case 0x27:  // IFU
                case 0x28: {  // IFC
                    const bool c = op == 0x27 ? (sh.b >> ((w >> 22) & 0xF) & 1) : Cond(w);
                    if (c) {
                        Push(dst, dst + num);
                    } else {
                        next = dst;
                    }
                    break;
                }
                case 0x29: {  // LOOP over an int uniform: x+1 times, aL from y, step z
                    const u32 iu = sh.i[(w >> 22) & 3];
                    aL = (s32)((iu >> 8) & 0xFF);
                    const s32 inc = (s32)(int8_t)((iu >> 16) & 0xFF);
                    Push(dst + 1, next, (s32)(iu & 0xFF), inc, true);
                    break;
                }
                case 0x2A: case 0x2B: break;  // EMIT, SETEMIT (geometry shaders)
                case 0x2C:  // JMPC
                    if (Cond(w)) next = dst;
                    break;
                case 0x2D:  // JMPU (num bit 0 inverts)
                    if (((sh.b >> ((w >> 22) & 0xF)) & 1) != (num & 1)) next = dst;
                    break;
                default:
                    break;
                }
            }
            pc = next;
        flow:
            // Blocks that end here: return, leave the if, or loop again.
            while (depth > 0 && pc == stack[depth - 1].end) {
                Frame& f = stack[depth - 1];
                if (f.loop && f.loops > 0) {
                    --f.loops;
                    aL += f.inc;
                    pc = f.ret;
                    break;
                }
                pc = f.loop ? f.end : f.ret;
                --depth;
            }
        }
    }
};

}  // namespace

void RunShader(const Shader& sh, u32 entry, const Vec4* in, Vec4* out)
{
    Run run(sh);
    for (int i = 0; i < 16; ++i) run.v[i] = in[i];
    run.Exec(entry);
    for (int i = 0; i < 16; ++i) out[i] = run.o[i];
}

}  // namespace pica
}  // namespace rt
