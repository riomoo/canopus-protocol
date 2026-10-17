#include "gpu.h"

#include <algorithm>
#include <cstring>

namespace rt {
namespace gpu {

u32 PhysToVirt(u32 pa)
{
    if (pa >= 0x20000000 && pa < 0x28000000) return pa - 0x20000000 + mem::LINEAR;  // FCRAM
    if (pa >= 0x18000000 && pa < 0x18600000) return pa - 0x18000000 + mem::VRAM;    // VRAM
    if (pa >= 0x1FF00000 && pa < 0x1FF80000) return pa;                              // DSP RAM
    return 0;
}

u32 VirtToPhys(u32 va)
{
    if (va >= mem::LINEAR && va < mem::LINEAR_END) return va - mem::LINEAR + 0x20000000;
    if (va >= mem::VRAM && va < mem::VRAM + mem::VRAM_SIZE) return va - mem::VRAM + 0x18000000;
    return va;
}

u32 BytesPerPixel(u32 f)
{
    switch (f) {
    case RGBA8: return 4;
    case RGB8: return 3;
    default: return 2;
    }
}

u32 ReadPixel(const u8* p, u32 f)
{
    switch (f) {
    case RGBA8:  // bytes A B G R
        return (u32)p[0] << 24 | (u32)p[3] << 16 | (u32)p[2] << 8 | p[1];
    case RGB8:  // bytes B G R
        return 0xFF000000u | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
    case RGB565: {
        const u32 v = p[0] | p[1] << 8;
        const u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    }
    case RGB5A1: {
        const u32 v = p[0] | p[1] << 8;
        const u32 r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
        return (v & 1 ? 0xFF000000u : 0) | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    }
    default: {  // RGBA4
        const u32 v = p[0] | p[1] << 8;
        const u32 r = (v >> 12) & 15, g = (v >> 8) & 15, b = (v >> 4) & 15, a = v & 15;
        return (a * 17) << 24 | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
    }
    }
}

namespace {

void WritePixel(u8* p, u32 f, u32 argb)
{
    const u32 a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    switch (f) {
    case RGBA8: p[0] = (u8)a; p[1] = (u8)b; p[2] = (u8)g; p[3] = (u8)r; break;
    case RGB8: p[0] = (u8)b; p[1] = (u8)g; p[2] = (u8)r; break;
    case RGB565: { const u16 v = (u16)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); p[0] = (u8)v; p[1] = (u8)(v >> 8); break; }
    case RGB5A1: { const u16 v = (u16)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >= 128)); p[0] = (u8)v; p[1] = (u8)(v >> 8); break; }
    default: { const u16 v = (u16)((r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | (a >> 4)); p[0] = (u8)v; p[1] = (u8)(v >> 8); break; }
    }
}

// Position of pixel (x, y) in a tiled (8x8 blocks, Morton order inside a
// block) buffer of the given width, in pixels.
u32 TiledOffset(u32 x, u32 y, u32 width)
{
    static const u8* const morton = [] {
        static u8 t[64];
        for (u32 ty = 0; ty < 8; ++ty)
            for (u32 tx = 0; tx < 8; ++tx) {
                u32 m = 0;
                for (u32 b = 0; b < 3; ++b) m |= ((tx >> b) & 1) << (2 * b) | ((ty >> b) & 1) << (2 * b + 1);
                t[ty * 8 + tx] = (u8)m;
            }
        return t;
    }();
    return ((y / 8) * (width / 8) + x / 8) * 64 + morton[(y & 7) * 8 + (x & 7)];
}

}  // namespace

void MemoryFill(u32 start, u32 end, u32 value, u32 control)
{
    static int logged = 0;
    if (logged++ < 12) Log(LOG_DEBUG, "gpu", "MemoryFill %08X-%08X value %08X control %X", start, end, value, control);
    if (!start || end <= start) return;
    // control: bit 8/9 = 24/32-bit fill width, else 16-bit.
    const u32 width = (control & 0x200) ? 4 : (control & 0x100) ? 3 : 2;
    u8* p = mem::Ptr(start);
    const u32 n = end - start;
    if (width == 4) {
        for (u32 i = 0; i + 4 <= n; i += 4) std::memcpy(p + i, &value, 4);
    } else if (width == 3) {
        for (u32 i = 0; i + 3 <= n; i += 3) { p[i] = (u8)value; p[i + 1] = (u8)(value >> 8); p[i + 2] = (u8)(value >> 16); }
    } else {
        const u16 v = (u16)value;
        for (u32 i = 0; i + 2 <= n; i += 2) std::memcpy(p + i, &v, 2);
    }
}

void DisplayTransfer(u32 in, u32 out, u32 inDims, u32 outDims, u32 flags)
{
    static int logged = 0;
    if (logged++ < 12) Log(LOG_DEBUG, "gpu", "DisplayTransfer %08X -> %08X in %08X out %08X flags %08X", in, out, inDims, outDims, flags);
    Tally("DisplayTransfer %08X -> %08X in %08X out %08X flags %08X", in, out, inDims, outDims, flags);
    const u32 inW = inDims & 0xFFFF, inH = inDims >> 16;
    const u32 outW = outDims & 0xFFFF, outH = outDims >> 16;
    const bool flip = flags & 1;
    const bool linearIn = flags & 2;  // "output tiled" when set means linear -> tiled
    const u32 inFmt = (flags >> 8) & 7, outFmt = (flags >> 12) & 7;
    const u32 scale = (flags >> 24) & 3;  // 0 none, 1 2x1, 2 2x2
    const u32 sx = scale ? 2 : 1, sy = scale == 2 ? 2 : 1;
    if (!in || !out || !inW || !outW) return;
    const u32 bin = BytesPerPixel(inFmt), bout = BytesPerPixel(outFmt);
    for (u32 y = 0; y < outH; ++y) {
        for (u32 x = 0; x < outW; ++x) {
            u32 r = 0, g = 0, b = 0, a = 0;
            for (u32 dy = 0; dy < sy; ++dy) {
                for (u32 dx = 0; dx < sx; ++dx) {
                    const u32 ix = x * sx + dx, iy = y * sy + dy;
                    if (ix >= inW || iy >= inH) continue;
                    // Tiled -> linear (the usual case: render target to framebuffer).
                    const u32 off = linearIn ? (iy * inW + ix) : TiledOffset(ix, iy, inW);
                    const u32 px = ReadPixel(mem::Ptr(in + off * bin), inFmt);
                    a += px >> 24; r += (px >> 16) & 255; g += (px >> 8) & 255; b += px & 255;
                }
            }
            const u32 n = sx * sy;
            const u32 px = (a / n) << 24 | (r / n) << 16 | (g / n) << 8 | (b / n);
            const u32 oy = flip ? outH - 1 - y : y;
            const u32 off = linearIn ? TiledOffset(x, oy, outW) : (oy * outW + x);
            WritePixel(mem::Ptr(out + off * bout), outFmt, px);
        }
    }
}

void TextureCopy(u32 in, u32 out, u32 size, u32 inGap, u32 outGap, u32 flags)
{
    // Copies lines of (gap & 0xFFFF) * 16 bytes, skipping (gap >> 16) * 16 bytes after each.
    const u32 inLine = (inGap & 0xFFFF) * 16, inSkip = (inGap >> 16) * 16;
    const u32 outLine = (outGap & 0xFFFF) * 16, outSkip = (outGap >> 16) * 16;
    if (!inLine || !outLine) {
        std::memmove(mem::Ptr(out), mem::Ptr(in), size);
        return;
    }
    u32 done = 0, ip = in, op = out, il = 0, ol = 0;
    while (done < size) {
        const u32 n = std::min({inLine - il, outLine - ol, size - done});
        std::memmove(mem::Ptr(op), mem::Ptr(ip), n);
        ip += n; op += n; il += n; ol += n; done += n;
        if (il == inLine) { ip += inSkip; il = 0; }
        if (ol == outLine) { op += outSkip; ol = 0; }
    }
}

}  // namespace gpu
}  // namespace rt
