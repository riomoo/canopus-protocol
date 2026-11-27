// y2r:u, the YUV -> RGB converter the game's movie player uses. A
// conversion runs at once when started: the Y/U/V (or YUYV) data the game
// set up for sending becomes RGB at the receiving buffer, linear or in the
// GPU's 8x8 tiles, and the transfer-end event fires.
#include "ipc.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace rt {
namespace ipc {

namespace {

struct Transfer {
    u32 addr = 0, size = 0, unit = 0, gap = 0;
};

// Standard coefficients: Y, R from V, G from V, G from U, B from U, then the
// R, G, B offsets.
const s32 kCoefficients[4][8] = {
    {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B},  // ITU-R BT.601
    {0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51},   // ITU-R BT.709
    {0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B},  // BT.601, scaling
    {0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99A, -0x2421},   // BT.709, scaling
};

struct Y2r : Service {
    Y2r() : Service("y2r:u")
    {
        done->name = "y2r transfer end event";
        done->reset = kernel::ONESHOT;
    }
    std::shared_ptr<kernel::Event> done = std::make_shared<kernel::Event>();
    u32 inFormat = 0, outFormat = 0, rotation = 0, block = 0, lineWidth = 0, lines = 0, alpha = 0xFF;
    s32 coeff[8];
    bool interrupt = false;
    Transfer y, u, v, yuyv, out;

    // Reads a transfer's bytes: units of `unit` bytes, `gap` bytes apart.
    static std::vector<u8> Gather(const Transfer& t)
    {
        std::vector<u8> d(t.size);
        u32 a = t.addr;
        const u32 unit = t.unit ? t.unit : t.size;
        for (u32 i = 0; i < t.size;) {
            const u32 n = std::min(unit, t.size - i);
            std::memcpy(&d[i], mem::Ptr(a), n);
            i += n;
            a += n + t.gap;
        }
        return d;
    }

    static void Scatter(const Transfer& t, const std::vector<u8>& d)
    {
        u32 a = t.addr;
        const u32 unit = t.unit ? t.unit : t.size;
        const u32 total = std::min<u32>(t.size, (u32)d.size());
        for (u32 i = 0; i < total;) {
            const u32 n = std::min(unit, total - i);
            std::memcpy(mem::Ptr(a), &d[i], n);
            i += n;
            a += n + t.gap;
        }
    }

    void Convert()
    {
        const u32 w = lineWidth, h = lines;
        if (!w || !h || !out.addr) return;
        const u32 bpp = outFormat == 0 ? 4 : outFormat == 1 ? 3 : 2;
        std::vector<u8> Y, U, V, P;
        if (inFormat == 4) {
            P = Gather(yuyv);
        } else {
            Y = Gather(y);
            U = Gather(u);
            V = Gather(v);
        }
        const bool sixteen = inFormat == 2 || inFormat == 3;  // 16-bit samples: high byte used
        const bool is420 = inFormat == 1 || inFormat == 3;
        auto sample = [&](const std::vector<u8>& p, u32 i) -> s32 {
            if (sixteen) return (i * 2 + 1 < p.size()) ? p[i * 2 + 1] : 0;
            return i < p.size() ? p[i] : 0;
        };
        std::vector<u8> o(w * h * bpp);
        for (u32 yy = 0; yy < h; ++yy) {
            for (u32 xx = 0; xx < w; ++xx) {
                s32 Ys, Us, Vs;
                if (inFormat == 4) {
                    const u32 pair = (yy * w + xx) / 2 * 4;
                    Ys = pair + ((xx & 1) ? 2 : 0) < P.size() ? P[pair + ((xx & 1) ? 2 : 0)] : 0;
                    Us = pair + 1 < P.size() ? P[pair + 1] : 128;
                    Vs = pair + 3 < P.size() ? P[pair + 3] : 128;
                } else {
                    Ys = sample(Y, yy * w + xx);
                    const u32 ci = is420 ? (yy / 2) * (w / 2) + xx / 2 : yy * (w / 2) + xx / 2;
                    Us = sample(U, ci);
                    Vs = sample(V, ci);
                }
                const s32 cY = coeff[0] * Ys;
                s32 r = ((cY + coeff[1] * Vs) >> 3) + coeff[5] + 0x18;
                s32 g = ((cY - coeff[2] * Vs - coeff[3] * Us) >> 3) + coeff[6] + 0x18;
                s32 b = ((cY + coeff[4] * Us) >> 3) + coeff[7] + 0x18;
                r = std::clamp(r >> 5, 0, 255);
                g = std::clamp(g >> 5, 0, 255);
                b = std::clamp(b >> 5, 0, 255);
                u32 off;
                if (block) {  // 8x8 tiles, Morton order inside a tile
                    u32 m = 0;
                    for (u32 k = 0; k < 3; ++k) m |= ((xx >> k) & 1) << (2 * k) | ((yy >> k) & 1) << (2 * k + 1);
                    off = (((yy / 8) * (w / 8) + xx / 8) * 64 + m) * bpp;
                } else {
                    off = (yy * w + xx) * bpp;
                }
                u8* p = &o[off];
                switch (outFormat) {
                case 0: p[0] = (u8)alpha; p[1] = (u8)b; p[2] = (u8)g; p[3] = (u8)r; break;
                case 1: p[0] = (u8)b; p[1] = (u8)g; p[2] = (u8)r; break;
                case 2: { const u16 x = (u16)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (alpha >= 128)); std::memcpy(p, &x, 2); break; }
                default: { const u16 x = (u16)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); std::memcpy(p, &x, 2); break; }
                }
            }
        }
        if (rotation) Log(LOG_WARN, "y2r", "rotation %u not done", rotation);
        Scatter(out, o);
    }

    void Handle(Request& r) override
    {
        auto& c = r.cmd;
        auto ok = [&] { r.Reply(1, 0); };
        auto val = [&](u32 x) { r.Reply(2, 0); c[2] = x; };
        auto transfer = [&](Transfer& t) { t = {c[1], c[2], c[3] & 0xFFFF, c[4] & 0xFFFF}; ok(); };
        switch (r.id()) {
        case 0x0001: inFormat = c[1]; ok(); return;
        case 0x0002: val(inFormat); return;
        case 0x0003: outFormat = c[1]; ok(); return;
        case 0x0004: val(outFormat); return;
        case 0x0005: rotation = c[1]; ok(); return;
        case 0x0006: val(rotation); return;
        case 0x0007: block = c[1]; ok(); return;
        case 0x0008: val(block); return;
        case 0x0009: case 0x000B: ok(); return;  // spatial / temporal dithering
        case 0x000A: case 0x000C: val(0); return;
        case 0x000D: interrupt = c[1] != 0; ok(); return;  // SetTransferEndInterrupt
        case 0x000E: val(interrupt); return;
        case 0x000F:  // GetTransferEndEvent
            r.Reply(1, 2);
            c[2] = 0;
            c[3] = kernel::NewHandle(done);
            return;
        case 0x0010: transfer(y); return;
        case 0x0011: transfer(u); return;
        case 0x0012: transfer(v); return;
        case 0x0013: transfer(yuyv); return;
        case 0x0014: case 0x0015: case 0x0016: case 0x0017: case 0x0019: val(1); return;  // finished sending / receiving
        case 0x0018: transfer(out); return;  // SetReceiving
        case 0x001A: lineWidth = c[1] & 0xFFFF; ok(); return;
        case 0x001B: val(lineWidth); return;
        case 0x001C: lines = c[1] & 0xFFFF; ok(); return;
        case 0x001D: val(lines); return;
        case 0x001E:  // SetCoefficient(8 x s16)
            for (int i = 0; i < 8; ++i) coeff[i] = (s16)(i & 1 ? c[1 + i / 2] >> 16 : c[1 + i / 2]);
            ok();
            return;
        case 0x0020:  // SetStandardCoefficient
            std::memcpy(coeff, kCoefficients[c[1] & 3], sizeof coeff);
            ok();
            return;
        case 0x0022: alpha = c[1] & 0xFF; ok(); return;
        case 0x0023: val(alpha); return;
        case 0x0026:  // StartConversion
            { ProfScope p(PROF_Y2R); Convert(); }
            if (interrupt) {
                done->signaled = true;
                kernel::Signal(done.get(), *r.lock);
            }
            ok();
            return;
        case 0x0027: ok(); return;   // StopConversion
        case 0x0028: val(0); return;  // IsBusyConversion
        case 0x002A: val(0); return;  // PingProcess
        case 0x002B:  // DriverInitialize
            std::memcpy(coeff, kCoefficients[0], sizeof coeff);
            ok();
            return;
        case 0x002C: ok(); return;  // DriverFinalize
        }
        Unhandled(r);
    }
};

}  // namespace

void RegisterY2r() { Register("y2r:u", std::make_shared<Y2r>()); }

}  // namespace ipc
}  // namespace rt
