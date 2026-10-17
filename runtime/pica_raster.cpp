// PICA200 primitive assembly, clipping, rasterisation, texturing, texture
// combiners and per-fragment operations, in software.
//
// Buffers and textures are stored in 8x8 tiles (Morton order inside a
// tile), bottom row first.
#include "gpu.h"
#include "pica.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <condition_variable>

namespace rt {
namespace pica {

namespace {

#define HOT inline __attribute__((always_inline))

struct Color {
    int r = 0, g = 0, b = 0, a = 0;
    HOT int& operator[](int i) { return (&r)[i]; }
    HOT int operator[](int i) const { return (&r)[i]; }
};

// Morton order inside an 8x8 tile, indexed by y * 8 + x.
struct MortonTable {
    u8 m[64];
    constexpr MortonTable() : m()
    {
        for (u32 y = 0; y < 8; ++y)
            for (u32 x = 0; x < 8; ++x) {
                u32 v = 0;
                for (u32 b = 0; b < 3; ++b) v |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
                m[y * 8 + x] = (u8)v;
            }
    }
};
constexpr MortonTable kMorton;

// Offset (in pixels) of (x, y) in a tiled buffer of the given width.
inline u32 TileOffset(u32 x, u32 y, u32 width) { return (y & ~7u) * width + (x & ~7u) * 8 + kMorton.m[(y & 7) * 8 + (x & 7)]; }

// ---- textures ----

const int kEtcModifiers[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};

Color DecodeEtc1(u64 block, u32 x, u32 y)
{
    const u32 hi = (u32)(block >> 32), lo = (u32)block;
    const bool flip = hi & 1, diff = hi >> 1 & 1;
    const bool second = flip ? y >= 2 : x >= 2;
    int r, g, b;
    if (diff) {
        auto ch = [&](int shift) {
            int base = (hi >> shift) & 0x1F;
            int d = (hi >> (shift - 3)) & 7;
            if (d >= 4) d -= 8;
            int v = second ? base + d : base;
            v &= 0x1F;
            return v << 3 | v >> 2;
        };
        r = ch(27);
        g = ch(19);
        b = ch(11);
    } else {
        auto ch = [&](int shift) {
            int v = second ? (hi >> (shift - 4)) & 0xF : (hi >> shift) & 0xF;
            return v << 4 | v;
        };
        r = ch(28);
        g = ch(20);
        b = ch(12);
    }
    const u32 table = second ? (hi >> 2) & 7 : (hi >> 5) & 7;
    const u32 i = x * 4 + y;
    const u32 msb = (lo >> (16 + i)) & 1, lsb = (lo >> i) & 1;
    const int m = kEtcModifiers[table][lsb] * (msb ? -1 : 1);
    Color c;
    c.r = std::clamp(r + m, 0, 255);
    c.g = std::clamp(g + m, 0, 255);
    c.b = std::clamp(b + m, 0, 255);
    c.a = 255;
    return c;
}

struct Texture {
    u32 addr = 0, width = 0, height = 0, format = 0, param = 0, border = 0;
};

HOT Color Texel(const Texture& t, u32 x, u32 y)
{
    y = t.height - 1 - y;  // stored bottom row first
    Color c;
    if (t.format == 12 || t.format == 13) {  // ETC1, ETC1A4: 4x4 blocks in 8x8 tiles
        const u32 blockBytes = t.format == 12 ? 8 : 16;
        const u32 tile = (y / 8) * (t.width / 8) + x / 8;
        const u32 sub = ((x / 4) & 1) + 2 * ((y / 4) & 1);
        const u32 a = t.addr + (tile * 4 + sub) * blockBytes;
        u64 alpha = ~0ull;
        u32 colorAt = a;
        if (t.format == 13) {
            alpha = (u64)mem::R32(a) | (u64)mem::R32(a + 4) << 32;
            colorAt = a + 8;
        }
        const u64 block = (u64)mem::R32(colorAt) | (u64)mem::R32(colorAt + 4) << 32;
        c = DecodeEtc1(block, x & 3, y & 3);
        if (t.format == 13) c.a = (int)((alpha >> (4 * ((x & 3) * 4 + (y & 3)))) & 0xF) * 17;
        return c;
    }
    const u32 off = TileOffset(x, y, t.width);
    switch (t.format) {
    case 0: { const u8* p = mem::Ptr(t.addr + off * 4); c = {p[3], p[2], p[1], p[0]}; break; }  // RGBA8
    case 1: { const u8* p = mem::Ptr(t.addr + off * 3); c = {p[2], p[1], p[0], 255}; break; }   // RGB8
    case 2: {  // RGB5A1
        const u32 v = mem::R16(t.addr + off * 2);
        c = {(int)((v >> 11) & 31) * 255 / 31, (int)((v >> 6) & 31) * 255 / 31, (int)((v >> 1) & 31) * 255 / 31, v & 1 ? 255 : 0};
        break;
    }
    case 3: {  // RGB565
        const u32 v = mem::R16(t.addr + off * 2);
        c = {(int)((v >> 11) & 31) * 255 / 31, (int)((v >> 5) & 63) * 255 / 63, (int)(v & 31) * 255 / 31, 255};
        break;
    }
    case 4: {  // RGBA4
        const u32 v = mem::R16(t.addr + off * 2);
        c = {(int)((v >> 12) & 15) * 17, (int)((v >> 8) & 15) * 17, (int)((v >> 4) & 15) * 17, (int)(v & 15) * 17};
        break;
    }
    case 5: { const u8* p = mem::Ptr(t.addr + off * 2); c = {p[1], p[1], p[1], p[0]}; break; }  // IA8
    case 6: { const u8* p = mem::Ptr(t.addr + off * 2); c = {p[1], p[0], 0, 255}; break; }      // RG8
    case 7: { const int i = mem::R8(t.addr + off); c = {i, i, i, 255}; break; }                // I8
    case 8: { c = {0, 0, 0, mem::R8(t.addr + off)}; break; }                                    // A8
    case 9: {  // IA4
        const u32 v = mem::R8(t.addr + off);
        const int i = (int)(v >> 4) * 17;
        c = {i, i, i, (int)(v & 15) * 17};
        break;
    }
    case 10: {  // I4
        const u32 v = mem::R8(t.addr + off / 2);
        const int i = (int)((off & 1) ? v >> 4 : v & 15) * 17;
        c = {i, i, i, 255};
        break;
    }
    case 11: {  // A4
        const u32 v = mem::R8(t.addr + off / 2);
        c = {0, 0, 0, (int)((off & 1) ? v >> 4 : v & 15) * 17};
        break;
    }
    default: c = {255, 0, 255, 255}; break;
    }
    return c;
}

// Applies a wrap mode; false when the coordinate falls outside with
// clamp-to-border.
bool Wrap(int& v, int size, u32 mode)
{
    switch (mode) {
    case 0: v = std::clamp(v, 0, size - 1); return true;   // clamp to edge
    case 1: return v >= 0 && v < size;                     // clamp to border
    case 2: v = ((v % size) + size) % size; return true;   // repeat
    case 3: {                                              // mirrored repeat
        int p = ((v % (2 * size)) + 2 * size) % (2 * size);
        v = p < size ? p : 2 * size - 1 - p;
        return true;
    }
    default: v = std::clamp(v, 0, size - 1); return true;
    }
}

HOT int FastFloor(float f)
{
    const int i = (int)f;
    return i - (f < (float)i);
}

HOT Color Sample(const Texture& t, float u, float v)
{
    if (!t.addr || !t.width || !t.height) return {0, 0, 0, 255};
    const u32 wrapT = (t.param >> 8) & 7, wrapS = (t.param >> 12) & 7;
    const bool linear = (t.param >> 1) & 1;  // magnification filter
    auto fetch = [&](int x, int y) -> Color {
        if ((unsigned)x < t.width && (unsigned)y < t.height) return Texel(t, (u32)x, (u32)y);
        if (!Wrap(x, (int)t.width, wrapS) || !Wrap(y, (int)t.height, wrapT))
            return {(int)(t.border & 255), (int)(t.border >> 8 & 255), (int)(t.border >> 16 & 255), (int)(t.border >> 24)};
        return Texel(t, (u32)x, (u32)y);
    };
    const float fx = u * t.width, fy = v * t.height;
    if (!linear) return fetch(FastFloor(fx), FastFloor(fy));
    // 8 bits of sub-texel position, as integers.
    const int sx = FastFloor((fx - 0.5f) * 256.0f), sy = FastFloor((fy - 0.5f) * 256.0f);
    const int x0 = sx >> 8, y0 = sy >> 8;
    const int ax = sx & 255, ay = sy & 255;
    const Color c00 = fetch(x0, y0), c10 = fetch(x0 + 1, y0), c01 = fetch(x0, y0 + 1), c11 = fetch(x0 + 1, y0 + 1);
    auto mix = [&](int a, int b, int c, int d) {
        const int top = a * (256 - ax) + b * ax, bottom = c * (256 - ax) + d * ax;
        return (top * (256 - ay) + bottom * ay + 32768) >> 16;
    };
    return {mix(c00.r, c10.r, c01.r, c11.r), mix(c00.g, c10.g, c01.g, c11.g), mix(c00.b, c10.b, c01.b, c11.b),
            mix(c00.a, c10.a, c01.a, c11.a)};
}

Texture Unit(int n)
{
    static const u32 base[3] = {0x081, 0x091, 0x099};
    const u32 b = base[n];
    Texture t;
    t.border = g.regs[b];
    t.height = g.regs[b + 1] & 0x7FF;
    t.width = (g.regs[b + 1] >> 16) & 0x7FF;
    t.param = g.regs[b + 2];
    t.addr = gpu::PhysToVirt(g.regs[b + 4] << 3);
    t.format = (n == 0 ? g.regs[0x08E] : g.regs[b + 5]) & 0xF;
    return t;
}

// ---- texture combiners ----

HOT int Clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }





// The six combiner stages, decoded from the registers once per batch of
// triangles: each input is a slot (primary colour, a texture, the buffer,
// the constant, the previous stage...) and an operand is a channel of it,
// possibly inverted.
enum Slot { S_PRIM, S_LIGHT, S_ZERO, S_TEX0, S_TEX1, S_TEX2, S_TEX3, S_BUFFER, S_CONST, S_PREV, S_COUNT };

struct TevStage {
    u8 src[3], srcA[3];
    int8_t rgbSel[3];  // channel the RGB operand reads (-1: the channel being computed)
    u8 rgbInv[3];  // 255 to invert
    u8 aSel[3], aInv[3];
    u8 rgbOp, aOp, rs, as;
    Color konst;
    bool passthrough;  // output = previous stage's output
};
struct TevSetup {
    TevStage stage[6];
    Color buffer;
    u32 update;
};

u8 SlotOf(u32 s)
{
    switch (s) {
    case 0x0: return S_PRIM;
    case 0x1: return S_LIGHT;  // fragment lighting: not done yet, primary colour stands in
    case 0x2: return S_ZERO;
    case 0x3: return S_TEX0;
    case 0x4: return S_TEX1;
    case 0x5: return S_TEX2;
    case 0x6: return S_TEX3;
    case 0xD: return S_BUFFER;
    case 0xE: return S_CONST;
    default: return S_PREV;
    }
}

void RgbOperandOf(u32 op, int8_t* sel, u8* inv)
{
    switch (op) {
    case 0: *sel = -1; *inv = 0; break;
    case 1: *sel = -1; *inv = 255; break;
    case 2: *sel = 3; *inv = 0; break;
    case 3: *sel = 3; *inv = 255; break;
    case 4: *sel = 0; *inv = 0; break;
    case 5: *sel = 0; *inv = 255; break;
    case 8: *sel = 1; *inv = 0; break;
    case 9: *sel = 1; *inv = 255; break;
    case 0xC: *sel = 2; *inv = 0; break;
    case 0xD: *sel = 2; *inv = 255; break;
    default: *sel = -1; *inv = 0; break;
    }
}

void AlphaOperandOf(u32 op, u8* sel, u8* inv)
{
    static const u8 chan[8] = {3, 3, 0, 0, 1, 1, 2, 2};
    *sel = chan[op & 7];
    *inv = (op & 1) ? 255 : 0;
}

HOT int Combine(u32 op, int a, int b, int c)
{
    switch (op) {
    case 0: return a;
    case 1: return a * b / 255;
    case 2: return std::min(a + b, 255);
    case 3: return Clamp8(a + b - 128);
    case 4: return (a * c + b * (255 - c)) / 255;
    case 5: return std::max(a - b, 0);
    case 8: return std::min(a * b / 255 + c, 255);
    case 9: return std::min(a + b, 255) * c / 255;
    default: return a;
    }
}

TevSetup DecodeTev()
{
    static const u32 stageBase[6] = {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8};
    TevSetup t;
    t.update = g.regs[TEXENV_UPDATE_BUFFER];
    const u32 bc = g.regs[TEXENV_BUFFER_COLOR];
    t.buffer = {(int)(bc & 255), (int)(bc >> 8 & 255), (int)(bc >> 16 & 255), (int)(bc >> 24)};
    for (int s = 0; s < 6; ++s) {
        const u32 b = stageBase[s];
        const u32 src = g.regs[b], opr = g.regs[b + 1], cmb = g.regs[b + 2], kc = g.regs[b + 3], scale = g.regs[b + 4];
        TevStage& st = t.stage[s];
        for (int i = 0; i < 3; ++i) {
            st.src[i] = SlotOf((src >> (4 * i)) & 0xF);
            st.srcA[i] = SlotOf((src >> (16 + 4 * i)) & 0xF);
            RgbOperandOf((opr >> (4 * i)) & 0xF, &st.rgbSel[i], &st.rgbInv[i]);
            AlphaOperandOf((opr >> (12 + 4 * i)) & 7, &st.aSel[i], &st.aInv[i]);
        }
        st.rgbOp = cmb & 0xF;
        st.aOp = (cmb >> 16) & 0xF;
        st.rs = (u8)(1 << (scale & 3));
        st.as = (u8)(1 << ((scale >> 16) & 3));
        st.konst = {(int)(kc & 255), (int)(kc >> 8 & 255), (int)(kc >> 16 & 255), (int)(kc >> 24)};
        st.passthrough = s > 0 && st.rgbOp == 0 && st.aOp == 0 && st.src[0] == S_PREV && st.srcA[0] == S_PREV &&
                         st.rgbSel[0] == -1 && st.rgbInv[0] == 0 && st.aSel[0] == 3 && st.aInv[0] == 0 && st.rs == 1 && st.as == 1;
    }
    return t;
}

HOT Color Combiners(const TevSetup& t, const Color& prim, const Color* tex)
{
    Color slot[S_COUNT];
    slot[S_PRIM] = prim;
    slot[S_LIGHT] = prim;
    slot[S_ZERO] = {0, 0, 0, 0};
    slot[S_TEX0] = tex[0];
    slot[S_TEX1] = tex[1];
    slot[S_TEX2] = tex[2];
    slot[S_TEX3] = tex[3];
    Color next = t.buffer;
    slot[S_BUFFER] = t.buffer;
    slot[S_PREV] = {0, 0, 0, 0};
    for (int s = 0; s < 6; ++s) {
        const TevStage& st = t.stage[s];
        Color out;
        if (st.passthrough) {
            out = slot[S_PREV];
        } else {
            slot[S_CONST] = st.konst;
            const Color &i0 = slot[st.src[0]], &i1 = slot[st.src[1]], &i2 = slot[st.src[2]];
            const u32 rgbOp = st.rgbOp, aOp = st.aOp;
            if (rgbOp == 6 || rgbOp == 7) {  // DOT3
                int d = 0;
                for (int ch = 0; ch < 3; ++ch) {
                    const int a = (st.rgbSel[0] < 0 ? i0[ch] : i0[st.rgbSel[0]]) ^ st.rgbInv[0];
                    const int b = (st.rgbSel[1] < 0 ? i1[ch] : i1[st.rgbSel[1]]) ^ st.rgbInv[1];
                    d += (a - 128) * (b - 128);
                }
                d = Clamp8(d * 4 / 255);
                out.r = out.g = out.b = d;
                if (rgbOp == 7) out.a = d;
            } else {
                for (int ch = 0; ch < 3; ++ch) {
                    const int a = (st.rgbSel[0] < 0 ? i0[ch] : i0[st.rgbSel[0]]) ^ st.rgbInv[0];
                    const int b = (st.rgbSel[1] < 0 ? i1[ch] : i1[st.rgbSel[1]]) ^ st.rgbInv[1];
                    const int c = (st.rgbSel[2] < 0 ? i2[ch] : i2[st.rgbSel[2]]) ^ st.rgbInv[2];
                    out[ch] = Combine(rgbOp, a, b, c);
                }
            }
            if (rgbOp != 7) {
                const Color &a0 = slot[st.srcA[0]], &a1 = slot[st.srcA[1]], &a2 = slot[st.srcA[2]];
                const int a = a0[st.aSel[0]] ^ st.aInv[0];
                const int b = a1[st.aSel[1]] ^ st.aInv[1];
                const int c = a2[st.aSel[2]] ^ st.aInv[2];
                out.a = (aOp == 6 || aOp == 7) ? out.a : Combine(aOp, a, b, c);
            }
            if (st.rs != 1) {
                out.r = Clamp8(out.r * st.rs);
                out.g = Clamp8(out.g * st.rs);
                out.b = Clamp8(out.b * st.rs);
            }
            if (st.as != 1) out.a = Clamp8(out.a * st.as);
        }
        // The buffer a stage reads is one stage behind the updates.
        slot[S_BUFFER] = next;
        if (s < 4) {
            if (t.update >> (8 + s) & 1) { next.r = out.r; next.g = out.g; next.b = out.b; }
            if (t.update >> (12 + s) & 1) next.a = out.a;
        }
        slot[S_PREV] = out;
    }
    return slot[S_PREV];
}

// ---- per-fragment operations ----

HOT bool Test(u32 func, int a, int b)
{
    switch (func) {
    case 0: return false;
    case 1: return true;
    case 2: return a == b;
    case 3: return a != b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return a > b;
    default: return a >= b;
    }
}

HOT int BlendFactor(u32 f, const Color& src, const Color& dst, const Color& k, int ch)
{
    auto C = [&](const Color& c) { return ch == 0 ? c.r : ch == 1 ? c.g : ch == 2 ? c.b : c.a; };
    switch (f) {
    case 0: return 0;
    case 1: return 255;
    case 2: return C(src);
    case 3: return 255 - C(src);
    case 4: return C(dst);
    case 5: return 255 - C(dst);
    case 6: return src.a;
    case 7: return 255 - src.a;
    case 8: return dst.a;
    case 9: return 255 - dst.a;
    case 10: return C(k);
    case 11: return 255 - C(k);
    case 12: return k.a;
    case 13: return 255 - k.a;
    case 14: return ch == 3 ? 255 : std::min(src.a, 255 - dst.a);
    default: return 255;
    }
}

HOT int BlendEq(u32 eq, int s, int d, int sf, int df)
{
    const int a = s * sf / 255, b = d * df / 255;
    switch (eq) {
    case 0: return Clamp8(a + b);
    case 1: return Clamp8(a - b);
    case 2: return Clamp8(b - a);
    case 3: return std::min(s, d);
    case 4: return std::max(s, d);
    default: return Clamp8(a + b);
    }
}

struct Target {
    u32 color = 0, depth = 0, width = 0, height = 0, colorFormat = 0, depthFormat = 0;
};

Target CurrentTarget()
{
    Target t;
    t.color = gpu::PhysToVirt(g.regs[COLOR_LOC] << 3);
    t.depth = gpu::PhysToVirt(g.regs[DEPTH_LOC] << 3);
    t.width = g.regs[FB_DIM] & 0x7FF;
    t.height = ((g.regs[FB_DIM] >> 12) & 0x3FF) + 1;
    t.colorFormat = (g.regs[COLOR_FORMAT] >> 16) & 7;
    t.depthFormat = g.regs[DEPTH_FORMAT] & 3;
    return t;
}

HOT Color ReadColor(const Target& t, u32 off)
{
    switch (t.colorFormat) {
    case 0: { const u8* p = mem::Ptr(t.color + off * 4); return {p[3], p[2], p[1], p[0]}; }
    case 2: { const u32 v = mem::R16(t.color + off * 2); return {(int)((v >> 11) & 31) * 255 / 31, (int)((v >> 6) & 31) * 255 / 31, (int)((v >> 1) & 31) * 255 / 31, v & 1 ? 255 : 0}; }
    case 3: { const u32 v = mem::R16(t.color + off * 2); return {(int)((v >> 11) & 31) * 255 / 31, (int)((v >> 5) & 63) * 255 / 63, (int)(v & 31) * 255 / 31, 255}; }
    case 4: { const u32 v = mem::R16(t.color + off * 2); return {(int)((v >> 12) & 15) * 17, (int)((v >> 8) & 15) * 17, (int)((v >> 4) & 15) * 17, (int)(v & 15) * 17}; }
    default: { const u8* p = mem::Ptr(t.color + off * 3); return {p[2], p[1], p[0], 255}; }
    }
}

HOT void WriteColor(const Target& t, u32 off, const Color& c)
{
    switch (t.colorFormat) {
    case 0: { u8* p = mem::Ptr(t.color + off * 4); p[0] = (u8)c.a; p[1] = (u8)c.b; p[2] = (u8)c.g; p[3] = (u8)c.r; break; }
    case 2: mem::W16(t.color + off * 2, (u16)((c.r >> 3) << 11 | (c.g >> 3) << 6 | (c.b >> 3) << 1 | (c.a >= 128))); break;
    case 3: mem::W16(t.color + off * 2, (u16)((c.r >> 3) << 11 | (c.g >> 2) << 5 | (c.b >> 3))); break;
    case 4: mem::W16(t.color + off * 2, (u16)((c.r >> 4) << 12 | (c.g >> 4) << 8 | (c.b >> 4) << 4 | (c.a >> 4))); break;
    default: { u8* p = mem::Ptr(t.color + off * 3); p[0] = (u8)c.b; p[1] = (u8)c.g; p[2] = (u8)c.r; break; }
    }
}

u32 DepthBits(u32 fmt) { return fmt == 0 ? 16 : 24; }
u32 DepthBytes(u32 fmt) { return fmt == 0 ? 2 : fmt == 3 ? 4 : 3; }

HOT u32 ReadDepth(const Target& t, u32 off)
{
    const u32 n = DepthBytes(t.depthFormat);
    const u8* p = mem::Ptr(t.depth + off * n);
    return n == 2 ? (u32)(p[0] | p[1] << 8) : (u32)(p[0] | p[1] << 8 | p[2] << 16);
}

HOT void WriteDepth(const Target& t, u32 off, u32 z)
{
    const u32 n = DepthBytes(t.depthFormat);
    u8* p = mem::Ptr(t.depth + off * n);
    p[0] = (u8)z;
    p[1] = (u8)(z >> 8);
    if (n >= 3) p[2] = (u8)(z >> 16);
}

// ---- rasteriser ----

struct ScreenVertex {
    float x, y, z, invw;  // screen position, depth (0..1), 1/w
    Vertex v;
};

// Triangles wait in a queue until the state changes (FlushTriangles), then
// are drawn in horizontal bands, one per core: every band takes the
// triangles in order, so the result is the same as drawing them one by one.
struct Tri {
    ScreenVertex v0, v1, v2;
    int minx, maxx, miny, maxy;
    float inv;    // 1 / signed area
    u32 state;    // index into s_states
};

// The fragment state, the same for every triangle in the queue.
struct RasterState {
    Target t;
    Texture tex[3];
    u32 texCfg, alphaTest, depthFunc, colorMask, bf;
    bool depthTest, depthWrite, depthOk, blend;
    Color bk;
    float depthMax;
    TevSetup tev;
};

struct Counts {
    u64 pixels = 0, depthFail = 0, alphaFail = 0, masked = 0;
};

std::vector<Tri> s_tris;
std::vector<RasterState> s_states;
bool s_stateDirty = true;
u64 s_queuedArea = 0;

RasterState CurrentState()
{
    RasterState s;
    s.t = CurrentTarget();
    s.texCfg = g.regs[TEXUNIT_CONFIG];
    for (int i = 0; i < 3; ++i)
        if (s.texCfg >> i & 1) s.tex[i] = Unit(i);
    s.alphaTest = g.regs[ALPHA_TEST];
    const u32 dcm = g.regs[DEPTH_COLOR_MASK];
    s.depthTest = dcm & 1;
    s.depthWrite = (dcm >> 12) & 1;
    s.depthFunc = (dcm >> 4) & 7;
    s.colorMask = (dcm >> 8) & 0xF;
    s.depthOk = s.t.depth && mem::IsMapped(s.t.depth, 1) && (g.regs[DEPTH_READ] || g.regs[DEPTH_WRITE]);
    s.blend = (g.regs[COLOR_OPERATION] >> 8) & 1;
    s.bf = g.regs[BLEND_FUNC];
    const u32 bcol = g.regs[BLEND_COLOR];
    s.bk = {(int)(bcol & 255), (int)(bcol >> 8 & 255), (int)(bcol >> 16 & 255), (int)(bcol >> 24)};
    s.depthMax = (float)((1u << DepthBits(s.t.depthFormat)) - 1);
    s.tev = DecodeTev();
    return s;
}

// Draws rows y0..y1 (inclusive) of one triangle.
void RasterRows(const RasterState& s, const Tri& tr, int y0, int y1, Counts& n)
{
    if (s.colorMask == 0 && !(s.depthOk && s.depthWrite)) return;
    const Target& t = s.t;
    const ScreenVertex &v0 = tr.v0, &v1 = tr.v1, &v2 = tr.v2;
    const float inv = tr.inv;
    const u32 texCfg = s.texCfg, alphaTest = s.alphaTest, bf = s.bf, colorMask = s.colorMask;
    // The barycentric weights are linear in x: step them along the row.
    const float d0 = (v1.y - v2.y) * inv, d1 = (v2.y - v0.y) * inv;
    const bool affine = v0.invw == v1.invw && v1.invw == v2.invw;  // flat (2D) triangles
    constexpr float kEps = -1e-6f;
    for (int y = std::max(y0, tr.miny); y <= std::min(y1, tr.maxy); ++y) {
        const float py = y + 0.5f;
        const float px0 = tr.minx + 0.5f;
        float w0s = ((v1.x - px0) * (v2.y - py) - (v2.x - px0) * (v1.y - py)) * inv;
        float w1s = ((v2.x - px0) * (v0.y - py) - (v0.x - px0) * (v2.y - py)) * inv;
        // The span of x where all three weights are >= 0 (a little wider, the
        // exact test stays below).
        float lo = 0, hi = (float)(tr.maxx - tr.minx);
        auto clip = [&](float w, float d) {
            if (d > 0) lo = std::max(lo, (kEps - w) / d - 1);
            else if (d < 0) hi = std::min(hi, (kEps - w) / d + 1);
            else if (w < kEps) hi = -1;
        };
        clip(w0s, d0);
        clip(w1s, d1);
        clip(1.0f - w0s - w1s, -d0 - d1);
        if (hi < lo) continue;
        const int xs = tr.minx + std::max(0, (int)lo), xe = tr.minx + std::min(tr.maxx - tr.minx, (int)hi + 1);
        float w0 = w0s + d0 * (float)(xs - tr.minx), w1 = w1s + d1 * (float)(xs - tr.minx);
        for (int x = xs; x <= xe; ++x, w0 += d0, w1 += d1) {
            const float w2 = 1.0f - w0 - w1;
            if (w0 < kEps || w1 < kEps || w2 < kEps) continue;
            // Perspective-correct weights.
            float p0 = w0, p1 = w1, p2 = w2;
            if (!affine) {
                const float r = 1.0f / (w0 * v0.invw + w1 * v1.invw + w2 * v2.invw);
                p0 = w0 * v0.invw * r;
                p1 = w1 * v1.invw * r;
                p2 = w2 * v2.invw * r;
            }
            auto lerp = [&](const Vec4& a, const Vec4& b, const Vec4& c) {
                Vec4 r;
                for (int k = 0; k < 4; ++k) r[k] = a[k] * p0 + b[k] * p1 + c[k] * p2;
                return r;
            };
            // Buffers are stored bottom row first.
            const u32 off = TileOffset((u32)x, t.height - 1 - (u32)y, t.width);
            // Depth.
            const float z = w0 * v0.z + w1 * v1.z + w2 * v2.z;
            const u32 zi = (u32)std::clamp(z * s.depthMax, 0.0f, s.depthMax);
            if (s.depthOk && s.depthTest && !Test(s.depthFunc, (int)zi, (int)ReadDepth(t, off))) { ++n.depthFail; continue; }
            // Colour: vertex colour, textures, combiners.
            const Vec4 vc = lerp(v0.v.color, v1.v.color, v2.v.color);
            const Color prim = {Clamp8((int)(vc.x * 255 + 0.5f)), Clamp8((int)(vc.y * 255 + 0.5f)), Clamp8((int)(vc.z * 255 + 0.5f)),
                                Clamp8((int)(vc.w * 255 + 0.5f))};
            Color tc[4] = {};
            if (texCfg & 1) {
                const Vec4 uv = lerp(v0.v.tc0, v1.v.tc0, v2.v.tc0);
                tc[0] = Sample(s.tex[0], uv.x, uv.y);
            }
            if (texCfg & 2) {
                const Vec4 uv = lerp(v0.v.tc1, v1.v.tc1, v2.v.tc1);
                tc[1] = Sample(s.tex[1], uv.x, uv.y);
            }
            if (texCfg & 4) {
                const Vec4 uv = (texCfg >> 13 & 1) ? lerp(v0.v.tc1, v1.v.tc1, v2.v.tc1) : lerp(v0.v.tc2, v1.v.tc2, v2.v.tc2);
                tc[2] = Sample(s.tex[2], uv.x, uv.y);
            }
            Color c = Combiners(s.tev, prim, tc);
            // Alpha test.
            if ((alphaTest & 1) && !Test((alphaTest >> 4) & 7, c.a, (int)((alphaTest >> 8) & 0xFF))) { ++n.alphaFail; continue; }
            // Blending.
            if (s.blend) {
                const Color d = ReadColor(t, off);
                Color o;
                const u32 eqRgb = bf & 7, eqA = (bf >> 8) & 7;
                const u32 sRgb = (bf >> 16) & 0xF, dRgb = (bf >> 20) & 0xF, sA = (bf >> 24) & 0xF, dA = (bf >> 28) & 0xF;
                o.r = BlendEq(eqRgb, c.r, d.r, BlendFactor(sRgb, c, d, s.bk, 0), BlendFactor(dRgb, c, d, s.bk, 0));
                o.g = BlendEq(eqRgb, c.g, d.g, BlendFactor(sRgb, c, d, s.bk, 1), BlendFactor(dRgb, c, d, s.bk, 1));
                o.b = BlendEq(eqRgb, c.b, d.b, BlendFactor(sRgb, c, d, s.bk, 2), BlendFactor(dRgb, c, d, s.bk, 2));
                o.a = BlendEq(eqA, c.a, d.a, BlendFactor(sA, c, d, s.bk, 3), BlendFactor(dA, c, d, s.bk, 3));
                c = o;
            }
            if (colorMask != 0xF) {
                const Color d = ReadColor(t, off);
                if (!(colorMask & 1)) c.r = d.r;
                if (!(colorMask & 2)) c.g = d.g;
                if (!(colorMask & 4)) c.b = d.b;
                if (!(colorMask & 8)) c.a = d.a;
            }
            if (colorMask) WriteColor(t, off, c);
            else ++n.masked;
            ++n.pixels;
            if (s.depthOk && s.depthWrite) WriteDepth(t, off, zi);
        }
    }
}

// Worker threads for the bands; job 0 runs on the caller.
class Pool {
public:
    static Pool& Get()
    {
        static Pool p;
        return p;
    }
    int Size() const { return (int)m_threads.size() + 1; }
    void Run(int jobs, const std::function<void(int)>& fn)
    {
        if (jobs <= 1 || m_threads.empty()) {
            for (int i = 0; i < jobs; ++i) fn(i);
            return;
        }
        {
            std::lock_guard<std::mutex> g(m_lock);
            m_fn = &fn;
            m_jobs = jobs;
            m_next = 1;
            m_done = 0;
            ++m_generation;
        }
        m_cv.notify_all();
        fn(0);
        for (;;) {  // help with what is left, then wait for the rest
            int i;
            {
                std::lock_guard<std::mutex> g(m_lock);
                if (m_next >= m_jobs) break;
                i = m_next++;
            }
            fn(i);
            std::lock_guard<std::mutex> g(m_lock);
            ++m_done;
        }
        std::unique_lock<std::mutex> g(m_lock);
        m_doneCv.wait(g, [&] { return m_done >= m_jobs - 1; });
        m_fn = nullptr;
    }

private:
    Pool()
    {
        const unsigned hw = std::thread::hardware_concurrency();
        const int n = std::clamp((int)hw - 1, 0, 15);
        for (int i = 0; i < n; ++i) m_threads.emplace_back([this] { SetThreadRole(ThreadRole::Background); Work(); });
        for (auto& t : m_threads) t.detach();
    }
    void Work()
    {
        u64 seen = 0;
        for (;;) {
            std::unique_lock<std::mutex> g(m_lock);
            m_cv.wait(g, [&] { return m_generation != seen && m_fn && m_next < m_jobs; });
            seen = m_generation;
            while (m_fn && m_next < m_jobs) {
                const int i = m_next++;
                const std::function<void(int)>* fn = m_fn;
                g.unlock();
                (*fn)(i);
                g.lock();
                if (++m_done >= m_jobs - 1) m_doneCv.notify_all();
            }
        }
    }
    std::vector<std::thread> m_threads;
    std::mutex m_lock;
    std::condition_variable m_cv, m_doneCv;
    const std::function<void(int)>* m_fn = nullptr;
    int m_jobs = 0, m_next = 0, m_done = 0;
    u64 m_generation = 0;
};

void RasterTriangle(const ScreenVertex& v0, const ScreenVertex& v1, const ScreenVertex& v2)
{
    ++g_stats.triangles;
    const Target t = CurrentTarget();
    if (!t.color || !t.width || !t.height || !mem::IsMapped(t.color, 1)) { ++g_stats.noTarget; return; }
    // Culling, with y up as the viewport transform leaves it: mode 0 keeps
    // everything, mode 1 keeps triangles of positive signed area, mode 2
    // those of negative area (the hardware's "clockwise" naming is from the
    // other side).
    const float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    if (area == 0) { ++g_stats.culled; return; }
    const u32 cull = g.regs[CULL_MODE] & 3;
    if (cull == 1 && area < 0) { ++g_stats.culled; return; }
    if (cull == 2 && area > 0) { ++g_stats.culled; return; }
    // Bounds: framebuffer, then scissor.
    int minx = (int)std::floor(std::min({v0.x, v1.x, v2.x})), maxx = (int)std::ceil(std::max({v0.x, v1.x, v2.x}));
    int miny = (int)std::floor(std::min({v0.y, v1.y, v2.y})), maxy = (int)std::ceil(std::max({v0.y, v1.y, v2.y}));
    minx = std::max(minx, 0);
    miny = std::max(miny, 0);
    maxx = std::min(maxx, (int)t.width - 1);
    maxy = std::min(maxy, (int)t.height - 1);
    if ((g.regs[SCISSOR_MODE] & 3) == 3) {  // inside
        minx = std::max(minx, (int)(g.regs[SCISSOR_POS] & 0x3FF));
        miny = std::max(miny, (int)((g.regs[SCISSOR_POS] >> 16) & 0x3FF));
        maxx = std::min(maxx, (int)(g.regs[SCISSOR_DIM] & 0x3FF));
        maxy = std::min(maxy, (int)((g.regs[SCISSOR_DIM] >> 16) & 0x3FF));
    }
    if (minx > maxx || miny > maxy) { ++g_stats.offscreen; return; }
    if (s_stateDirty || s_states.empty()) {
        RasterState st = CurrentState();
        if (!s_states.empty()) {
            // Draw what is queued first if this state draws elsewhere, or
            // samples what the queue is drawing.
            const Target& q = s_states.front().t;
            bool barrier = st.t.color != q.color || st.t.depth != q.depth || st.t.width != q.width || st.t.height != q.height ||
                           st.t.colorFormat != q.colorFormat;
            const u32 qEnd = q.color + q.width * q.height * 4;
            for (int i = 0; i < 3 && !barrier; ++i)
                if ((st.texCfg >> i & 1) && st.tex[i].addr < qEnd && st.tex[i].addr + st.tex[i].width * st.tex[i].height * 4 > q.color)
                    barrier = true;
            if (barrier) {
                ExecuteTriangles();
                st = CurrentState();
            }
        }
        s_states.push_back(st);
        s_stateDirty = false;
    }
    s_tris.push_back({v0, v1, v2, minx, maxx, miny, maxy, 1.0f / area, (u32)s_states.size() - 1});
    s_queuedArea += (u64)(maxx - minx + 1) * (u64)(maxy - miny + 1);
}

// Clipping against z <= 0, z >= -w and w > 0 (x and y are bounded by the
// rasteriser); then viewport transform.
Vertex Lerp(const Vertex& a, const Vertex& b, float t)
{
    auto L = [&](const Vec4& x, const Vec4& y) {
        Vec4 r;
        for (int k = 0; k < 4; ++k) r[k] = x[k] + (y[k] - x[k]) * t;
        return r;
    };
    Vertex r;
    r.pos = L(a.pos, b.pos);
    r.color = L(a.color, b.color);
    r.tc0 = L(a.tc0, b.tc0);
    r.tc1 = L(a.tc1, b.tc1);
    r.tc2 = L(a.tc2, b.tc2);
    r.view = L(a.view, b.view);
    r.quat = L(a.quat, b.quat);
    r.tc0w = a.tc0w + (b.tc0w - a.tc0w) * t;
    return r;
}

ScreenVertex ToScreen(const Vertex& v)
{
    ScreenVertex s;
    s.v = v;
    const float w = v.pos.w;
    s.invw = 1.0f / w;
    const float hw = Float24(g.regs[VIEWPORT_WIDTH]), hh = Float24(g.regs[VIEWPORT_HEIGHT]);
    const s32 ox = (s32)((g.regs[VIEWPORT_XY] & 0x3FF) << 22) >> 22, oy = (s32)(((g.regs[VIEWPORT_XY] >> 16) & 0x3FF) << 22) >> 22;
    s.x = (v.pos.x * s.invw + 1.0f) * hw + (float)ox;
    s.y = (v.pos.y * s.invw + 1.0f) * hh + (float)oy;
    const float scale = Float24(g.regs[DEPTH_SCALE]), offset = Float24(g.regs[DEPTH_OFFSET]);
    const float zw = (g.regs[DEPTHMAP_ENABLE] & 1) ? v.pos.z * s.invw : v.pos.z;
    s.z = zw * scale + offset;
    return s;
}

}  // namespace

void DrawTriangle(const Vertex& a, const Vertex& b, const Vertex& c)
{
    // Sutherland-Hodgman against three planes.
    Vertex poly[12], tmp[12];
    int n = 3;
    poly[0] = a;
    poly[1] = b;
    poly[2] = c;
    auto clip = [&](auto dist) {
        int m = 0;
        for (int i = 0; i < n; ++i) {
            const Vertex& p = poly[i];
            const Vertex& q = poly[(i + 1) % n];
            const float dp = dist(p), dq = dist(q);
            if (dp >= 0) tmp[m++] = p;
            if ((dp >= 0) != (dq >= 0) && m < 12) tmp[m++] = Lerp(p, q, dp / (dp - dq));
        }
        n = m;
        for (int i = 0; i < n; ++i) poly[i] = tmp[i];
    };
    clip([](const Vertex& v) { return v.pos.w - 1e-5f; });
    if (n < 3) return;
    clip([](const Vertex& v) { return -v.pos.z; });
    if (n < 3) return;
    clip([](const Vertex& v) { return v.pos.z + v.pos.w; });
    if (n < 3) return;
    ScreenVertex s[12];
    for (int i = 0; i < n; ++i) s[i] = ToScreen(poly[i]);
    for (int i = 1; i + 1 < n; ++i) RasterTriangle(s[0], s[i], s[i + 1]);
}

void FlushTriangles() { s_stateDirty = true; }

void ExecuteTriangles()
{
    s_stateDirty = true;
    if (s_tris.empty()) {
        s_states.clear();
        return;
    }
    Pool& pool = Pool::Get();
    // Small batches are not worth waking the other cores for.
    const int bands = s_queuedArea < 16384 ? 1 : std::min(pool.Size(), 16);
    const int height = (int)std::max<u32>(s_states.front().t.height, 1);
    const int bandRows = ((height + bands - 1) / bands + 7) & ~7;
    Counts counts[16];
    pool.Run(bands, [&](int b) {
        const int y0 = b * bandRows, y1 = std::min(height - 1, y0 + bandRows - 1);
        for (const Tri& tr : s_tris)
            if (tr.maxy >= y0 && tr.miny <= y1) RasterRows(s_states[tr.state], tr, y0, y1, counts[b]);
    });
    for (int b = 0; b < bands; ++b) {
        g_stats.pixels += counts[b].pixels;
        g_stats.depthFail += counts[b].depthFail;
        g_stats.alphaFail += counts[b].alphaFail;
        g_stats.masked += counts[b].masked;
    }
    s_tris.clear();
    s_states.clear();
    s_queuedArea = 0;
}

void ResetPrimitive()
{
    g.pendingCount = 0;
    g.stripOdd = false;
}

void SubmitVertex(const Vertex& v)
{
    const u32 mode = (g.regs[PRIMITIVE_CONFIG] >> 8) & 3;
    switch (mode) {
    case 1:  // strip
        if (g.pendingCount < 2) {
            g.pending[g.pendingCount++] = v;
            return;
        }
        if (g.stripOdd) DrawTriangle(g.pending[1], g.pending[0], v);
        else DrawTriangle(g.pending[0], g.pending[1], v);
        g.pending[0] = g.pending[1];
        g.pending[1] = v;
        g.stripOdd = !g.stripOdd;
        return;
    case 2:  // fan
        if (g.pendingCount < 2) {
            g.pending[g.pendingCount++] = v;
            return;
        }
        DrawTriangle(g.pending[0], g.pending[1], v);
        g.pending[1] = v;
        return;
    default:  // list (and geometry primitives)
        g.pending[g.pendingCount++] = v;
        if (g.pendingCount == 3) {
            DrawTriangle(g.pending[0], g.pending[1], g.pending[2]);
            g.pendingCount = 0;
        }
        return;
    }
}

}  // namespace pica
}  // namespace rt
