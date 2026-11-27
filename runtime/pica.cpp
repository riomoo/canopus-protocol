// PICA200 command lists: register writes, shader and uniform uploads,
// command-buffer jumps and draw calls (vertex fetch, the vertex shader, and
// output mapping to rasteriser vertices).
#include "pica.h"

#include <cstdio>
#include <string>
#include "gpu.h"

#include <cmath>
#include <cstring>

namespace rt {
namespace pica {

State g;
Stats g_stats;

float Float24(u32 v)
{
    v &= 0xFFFFFF;
    const u32 sign = v >> 23, exp = (v >> 16) & 0x7F, mant = v & 0xFFFF;
    u32 bits;
    if (!exp && !mant) bits = sign << 31;
    else if (exp == 0x7F) bits = sign << 31 | 0xFFu << 23 | mant << 7;
    else bits = sign << 31 | (exp + 64) << 23 | mant << 7;
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

float Float16(u32 v)
{
    v &= 0xFFFF;
    const u32 sign = v >> 15, exp = (v >> 10) & 0x1F, mant = v & 0x3FF;
    u32 bits;
    if (!exp && !mant) bits = sign << 31;
    else if (exp == 0x1F) bits = sign << 31 | 0xFFu << 23 | mant << 13;
    else bits = sign << 31 | (exp + 112) << 23 | mant << 13;
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

float Float31(u32 v)
{
    v &= 0x7FFFFFFF;
    const u32 sign = v >> 30, exp = (v >> 23) & 0x7F, mant = v & 0x7FFFFF;
    u32 bits = (!exp && !mant) ? sign << 31 : (sign << 31 | (exp + 64) << 23 | mant);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

namespace {

// Four float24 packed in three words: w, z, y, x from the top.
Vec4 Unpack24(const u32* w)
{
    Vec4 v;
    v.w = Float24(w[0] >> 8);
    v.z = Float24((w[0] & 0xFF) << 16 | (w[1] >> 16));
    v.y = Float24((w[1] & 0xFFFF) << 8 | (w[2] >> 24));
    v.x = Float24(w[2] & 0xFFFFFF);
    return v;
}

u32 NumAttributes() { return (g.regs[VSH_NUM_ATTR] & 0xF) + 1; }

// Shader outputs -> a rasteriser vertex through the output map.
Vertex MapOutputs(const Vec4* o)
{
    Vertex v;
    v.color = {1, 1, 1, 1};
    v.pos.w = 1;
    Vec4 regs[7];
    const u32 mask = g.regs[VSH_OUTMAP_MASK];
    int n = 0;
    for (int i = 0; i < 16 && n < 7; ++i)
        if (mask >> i & 1) regs[n++] = o[i];
    const u32 total = std::min<u32>(g.regs[OUTMAP_TOTAL] & 7, 7);
    for (u32 r = 0; r < total; ++r) {
        const u32 m = g.regs[OUTMAP_O0 + r];
        for (int c = 0; c < 4; ++c) {
            const u32 s = (m >> (8 * c)) & 0x1F;
            const float x = regs[r][c];
            if (s < 4) v.pos[s] = x;
            else if (s < 8) v.quat[s - 4] = x;
            else if (s < 12) v.color[s - 8] = x;
            else if (s == 0x0C) v.tc0.x = x;
            else if (s == 0x0D) v.tc0.y = x;
            else if (s == 0x0E) v.tc1.x = x;
            else if (s == 0x0F) v.tc1.y = x;
            else if (s == 0x10) v.tc0w = x;
            else if (s >= 0x12 && s <= 0x14) v.view[s - 0x12] = x;
            else if (s == 0x16) v.tc2.x = x;
            else if (s == 0x17) v.tc2.y = x;
        }
    }
    return v;
}

void RunVertex(const Vec4* attrs)
{
    Vec4 in[16], out[16];
    for (auto& x : in) x = {0, 0, 0, 1};
    const u64 perm = g.regs[VSH_PERMUTATION_LOW] | (u64)g.regs[VSH_PERMUTATION_HIGH] << 32;
    for (u32 i = 0; i < NumAttributes(); ++i) in[(perm >> (4 * i)) & 0xF] = attrs[i];
    RunShader(g.vs, g.regs[VSH_ENTRY] & 0xFFFF, in, out);
    const Vertex v = MapOutputs(out);
    if (v.pos.x != v.pos.x || v.pos.y != v.pos.y || v.pos.z != v.pos.z || v.pos.w != v.pos.w) {
        static int logged = 0;
        if (logged++ < 4) {
            std::string t;
            char b[160];
            for (int i = 0; i < 16; ++i) {
                std::snprintf(b, sizeof b, "\n    v%d (%g %g %g %g)  o%d (%g %g %g %g)", i, in[i].x, in[i].y, in[i].z, in[i].w, i, out[i].x,
                              out[i].y, out[i].z, out[i].w);
                t += b;
            }
            std::string nan;
            for (int i = 0; i < 96; ++i) {
                const Vec4& f = g.vs.f[i];
                if (f.x != f.x || f.y != f.y || f.z != f.z || f.w != f.w) nan += " c" + std::to_string(i);
            }
            Log(LOG_DEBUG, "pica", "vertex with NaN position (entry %X, attributes %u, perm %llX, fixed %03X; NaN uniforms:%s):%s",
                g.regs[VSH_ENTRY] & 0xFFFF, NumAttributes(), (unsigned long long)perm, (g.regs[ATTR_FORMAT_HIGH] >> 16) & 0xFFF,
                nan.c_str(), t.c_str());
            if (logged == 1) {
                std::string code;
                for (u32 pc = g.regs[VSH_ENTRY] & 0xFFFF, k = 0; pc < 512 && k < 48; ++pc, ++k) {
                    std::snprintf(b, sizeof b, "%s%03X:%08X", k % 8 ? " " : "\n    ", pc, g.vs.code[pc]);
                    code += b;
                }
                Log(LOG_DEBUG, "pica", "shader code:%s", code.c_str());
                std::string ops;
                for (int i = 0; i < 32; ++i) {
                    std::snprintf(b, sizeof b, "%s%02X:%08X", i % 8 ? " " : "\n    ", i, g.vs.opdesc[i]);
                    ops += b;
                }
                Log(LOG_DEBUG, "pica", "operand descriptors:%s", ops.c_str());
                std::string us;
                for (int i = 0; i < 96; ++i) {
                    const Vec4& f = g.vs.f[i];
                    if (f.x == 0 && f.y == 0 && f.z == 0 && f.w == 0) continue;
                    std::snprintf(b, sizeof b, "\n    c%d (%g %g %g %g)", i, f.x, f.y, f.z, f.w);
                    us += b;
                }
                Log(LOG_DEBUG, "pica", "float uniforms:%s", us.c_str());
            }
        }
    }
    SubmitVertex(v);
}

// Loads the attributes of vertex `index` from the attribute buffers.
void LoadVertex(u32 index, Vec4* attrs)
{
    const u32 basePA = g.regs[ATTR_BASE] << 3;
    const u64 fmt = g.regs[ATTR_FORMAT_LOW] | (u64)g.regs[ATTR_FORMAT_HIGH] << 32;
    const u32 fixedMask = (g.regs[ATTR_FORMAT_HIGH] >> 16) & 0xFFF;
    for (u32 i = 0; i < 12; ++i) attrs[i] = (fixedMask >> i & 1) ? g.fixedAttr[i] : Vec4{0, 0, 0, 1};
    if (!basePA) return;
    for (u32 b = 0; b < 12; ++b) {
        const u32 off = g.regs[ATTR_BUFFER0 + 3 * b] & 0x0FFFFFFF;
        const u32 cfg1 = g.regs[ATTR_BUFFER0 + 3 * b + 1], cfg2 = g.regs[ATTR_BUFFER0 + 3 * b + 2];
        const u32 elems = cfg2 >> 28, stride = (cfg2 >> 16) & 0xFF;
        if (!elems) continue;
        // Offsets are relative to the physical base; the sum is a physical address.
        const u32 start = gpu::PhysToVirt(basePA + off) + index * stride;
        u32 pos = 0;
        for (u32 k = 0; k < elems; ++k) {
            const u32 id = k < 8 ? (cfg1 >> (4 * k)) & 0xF : (cfg2 >> (4 * (k - 8))) & 0xF;
            if (id >= 12) {  // padding of 4, 8, 12 or 16 bytes
                pos = (pos + 3) & ~3u;
                pos += (id - 11) * 4;
                continue;
            }
            const u32 f = (fmt >> (4 * id)) & 0xF;
            const u32 type = f & 3, count = (f >> 2) + 1;
            const u32 size = type == 3 ? 4 : type == 2 ? 2 : 1;
            pos = (pos + size - 1) & ~(size - 1);
            if (fixedMask >> id & 1) {
                pos += size * count;
                continue;
            }
            Vec4 v{0, 0, 0, 1};
            for (u32 c = 0; c < count; ++c) {
                const u32 a = start + pos + c * size;
                switch (type) {
                case 0: v[c] = (float)(int8_t)mem::R8(a); break;
                case 1: v[c] = (float)mem::R8(a); break;
                case 2: v[c] = (float)(int16_t)mem::R16(a); break;
                default: {
                    u32 x = mem::R32(a);
                    float fv;
                    std::memcpy(&fv, &x, 4);
                    v[c] = fv;
                    if (fv != fv) WatchArm(a, "NaN vertex attribute");
                    break;
                }
                }
            }
            attrs[id] = v;
            pos += size * count;
        }
    }
}

void Draw(bool indexed)
{
    ++g_stats.draws;
    const u32 count = g.regs[NUM_VERTICES];
    const u32 first = g.regs[VERTEX_OFFSET];
    const u32 indexCfg = g.regs[INDEX_CONFIG];
    const u32 basePA = g.regs[ATTR_BASE] << 3;
    const u32 indexAddr = gpu::PhysToVirt(basePA + (indexCfg & 0x0FFFFFFF));
    const bool idx16 = indexCfg >> 31;
    // Every buffer the draw reads must be mapped.
    bool ok = basePA != 0;
    if (indexed) ok = ok && mem::IsMapped(indexAddr, count * (idx16 ? 2 : 1));
    for (u32 b = 0; ok && b < 12; ++b) {
        const u32 cfg2 = g.regs[ATTR_BUFFER0 + 3 * b + 2];
        if (!(cfg2 >> 28)) continue;
        const u32 at = gpu::PhysToVirt(basePA + (g.regs[ATTR_BUFFER0 + 3 * b] & 0x0FFFFFFF));
        if (!at || !mem::IsMapped(at, 1)) ok = false;
    }
    if (!ok) {
        static int n = 0;
        if (n++ < 10)
            Log(LOG_WARN, "pica", "draw skipped: buffers not mapped (base %08X index %08X count %u, buffer0 %08X %08X %08X)",
                g.regs[ATTR_BASE] << 3, indexCfg, count, g.regs[ATTR_BUFFER0], g.regs[ATTR_BUFFER0 + 1], g.regs[ATTR_BUFFER0 + 2]);
        return;
    }
    ResetPrimitive();
    Vec4 attrs[16];
    for (u32 i = 0; i < count; ++i) {
        u32 index = first + i;
        if (indexed) index = idx16 ? mem::R16(indexAddr + 2 * i) : mem::R8(indexAddr + i);
        LoadVertex(index, attrs);
        RunVertex(attrs);
        ++g_stats.vertices;
    }

}

void Write(u32 id, u32 value, u32 mask);

// Runs a command list from virtual address addr, size bytes; follows jumps.
void Run(u32 addr, u32 size)
{
    u32 p = addr, end = addr + size;
    u32 guard = 0;
    while (p + 8 <= end && guard++ < 4000000) {
        const u32 param = mem::R32(p), header = mem::R32(p + 4);
        p += 8;
        u32 id = header & 0xFFFF;
        const u32 mask = (header >> 16) & 0xF;
        const u32 extra = (header >> 20) & 0x7FF;
        const bool consecutive = header >> 31;
        Write(id, param, mask);
        for (u32 k = 0; k < extra; ++k) {
            if (consecutive) ++id;
            Write(id, mem::R32(p), mask);
            p += 4;
        }
        if (extra & 1) p += 4;
        // Command-buffer jumps continue at another buffer.
        if (id == CMDBUF_JUMP0 || id == CMDBUF_JUMP1) {
            const int n = id == CMDBUF_JUMP0 ? 0 : 1;
            p = gpu::PhysToVirt(g.regs[CMDBUF_ADDR0 + n] << 3);
            end = p + (g.regs[CMDBUF_SIZE0 + n] << 3);
            if (!p) return;
        }
    }
}

void Write(u32 id, u32 value, u32 mask)
{
    if (id >= 0x300) return;
    // Queued triangles are drawn with the state they were queued under.
    // Immediate-mode vertex data does not change that state.
    if (!(id >= FIXED_INDEX && id <= FIXED_DATA0 + 2)) FlushTriangles();
    const u32 m = (mask & 1 ? 0xFFu : 0) | (mask & 2 ? 0xFF00u : 0) | (mask & 4 ? 0xFF0000u : 0) | (mask & 8 ? 0xFF000000u : 0);
    g.regs[id] = (g.regs[id] & ~m) | (value & m);
    value = g.regs[id];
    Shader& vs = g.vs;
    switch (id) {
    case VSH_FLOAT_INDEX:
        vs.floatIndex = value & 0xFF;
        vs.float32 = value >> 31;
        vs.floatCount = 0;
        break;
    case VSH_FLOAT_DATA0: case VSH_FLOAT_DATA0 + 1: case VSH_FLOAT_DATA0 + 2: case VSH_FLOAT_DATA0 + 3:
    case VSH_FLOAT_DATA0 + 4: case VSH_FLOAT_DATA0 + 5: case VSH_FLOAT_DATA0 + 6: case VSH_FLOAT_DATA0 + 7:
        vs.floatWords[vs.floatCount++] = value;
        if (vs.float32 && vs.floatCount == 4) {
            Vec4 v;
            for (int i = 0; i < 4; ++i) {
                float f;
                std::memcpy(&f, &vs.floatWords[i], 4);
                v[3 - i] = f;
            }
            if (vs.floatIndex < 96) vs.f[vs.floatIndex] = v;
            ++vs.floatIndex;
            vs.floatCount = 0;
        } else if (!vs.float32 && vs.floatCount == 3) {
            if (vs.floatIndex < 96) vs.f[vs.floatIndex] = Unpack24(vs.floatWords);
            ++vs.floatIndex;
            vs.floatCount = 0;
        }
        break;
    case VSH_CODE_INDEX:
        vs.codeIndex = value & 0xFFF;
        break;
    case VSH_CODE_DATA0: case VSH_CODE_DATA0 + 1: case VSH_CODE_DATA0 + 2: case VSH_CODE_DATA0 + 3:
    case VSH_CODE_DATA0 + 4: case VSH_CODE_DATA0 + 5: case VSH_CODE_DATA0 + 6: case VSH_CODE_DATA0 + 7:
        if (vs.codeIndex < 512) vs.code[vs.codeIndex] = value;
        ++vs.codeIndex;
        break;
    case VSH_OPDESC_INDEX:
        vs.opdescIndex = value & 0x7F;
        break;
    case VSH_OPDESC_DATA0: case VSH_OPDESC_DATA0 + 1: case VSH_OPDESC_DATA0 + 2: case VSH_OPDESC_DATA0 + 3:
    case VSH_OPDESC_DATA0 + 4: case VSH_OPDESC_DATA0 + 5: case VSH_OPDESC_DATA0 + 6: case VSH_OPDESC_DATA0 + 7:
        if (vs.opdescIndex < 128) vs.opdesc[vs.opdescIndex] = value;
        ++vs.opdescIndex;
        break;
    case VSH_BOOL_UNIFORM:
        vs.b = (u16)value;
        break;
    case VSH_INT_UNIFORM0: case VSH_INT_UNIFORM0 + 1: case VSH_INT_UNIFORM0 + 2: case VSH_INT_UNIFORM0 + 3:
        vs.i[id - VSH_INT_UNIFORM0] = value;
        break;
    case FIXED_INDEX:
        g.fixedIndex = value & 0xF;
        g.fixedCount = 0;
        if (g.fixedIndex == 0xF) {
            g.immediateAttr = 0;
            ResetPrimitive();
        }
        break;
    case FIXED_DATA0: case FIXED_DATA0 + 1: case FIXED_DATA0 + 2:
        g.fixedWords[g.fixedCount++] = value;
        if (g.fixedCount == 3) {
            const Vec4 v = Unpack24(g.fixedWords);
            g.fixedCount = 0;
            if (g.fixedIndex == 0xF) {  // immediate mode: a vertex when all attributes are in
                g.immediate[g.immediateAttr++] = v;
                if (g.immediateAttr >= NumAttributes()) {
                    RunVertex(g.immediate.data());
                    g.immediateAttr = 0;
                }
            } else if (g.fixedIndex < 16) {
                g.fixedAttr[g.fixedIndex++] = v;
            }
        }
        break;
    case DRAW_ARRAYS:
        Draw(false);
        break;
    case DRAW_ELEMENTS:
        Draw(true);
        break;
    case RESTART_PRIMITIVE:
        ResetPrimitive();
        break;
    default:
        break;
    }
}

}  // namespace

}  // namespace pica

namespace gpu {

void ProcessCommandList(u32 addr, u32 size)
{
    auto& st = pica::g_stats;
    ++st.lists;
    pica::Run(addr, size);
    pica::ExecuteTriangles();
    if (st.lists % 60 == 0)
        Log(LOG_DEBUG, "pica", "%llu lists: %llu draws, %llu vertices, %llu triangles, %llu pixels; target %08X %ux%u fmt %u",
            (unsigned long long)st.lists, (unsigned long long)st.draws, (unsigned long long)st.vertices,
            (unsigned long long)st.triangles, (unsigned long long)st.pixels, pica::g.regs[pica::COLOR_LOC] << 3,
            pica::g.regs[pica::FB_DIM] & 0x7FF, ((pica::g.regs[pica::FB_DIM] >> 12) & 0x3FF) + 1,
            (pica::g.regs[pica::COLOR_FORMAT] >> 16) & 7);
}

}  // namespace gpu
}  // namespace rt
