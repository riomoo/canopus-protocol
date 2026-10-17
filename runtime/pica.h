// The PICA200, the 3DS GPU, drawn in software: register state from command
// lists, the vertex shader, primitive assembly and rasterisation, texture
// units, the texture combiners and the per-fragment operations.
#pragma once

#include "rt.h"

#include <array>

namespace rt {
namespace pica {

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};

// Register indices (3dbrew, "GPU/Internal Registers").
enum Reg : u32 {
    FINALIZE = 0x010,
    CULL_MODE = 0x040,
    VIEWPORT_WIDTH = 0x041,
    VIEWPORT_INVW = 0x042,
    VIEWPORT_HEIGHT = 0x043,
    VIEWPORT_INVH = 0x044,
    DEPTH_SCALE = 0x04D,
    DEPTH_OFFSET = 0x04E,
    OUTMAP_TOTAL = 0x04F,
    OUTMAP_O0 = 0x050,  // ..0x056
    SCISSOR_MODE = 0x065,
    SCISSOR_POS = 0x066,
    SCISSOR_DIM = 0x067,
    VIEWPORT_XY = 0x068,
    DEPTHMAP_ENABLE = 0x06D,
    TEXUNIT_CONFIG = 0x080,
    TEXENV_UPDATE_BUFFER = 0x0E0,
    TEXENV_BUFFER_COLOR = 0x0FD,
    COLOR_OPERATION = 0x100,
    BLEND_FUNC = 0x101,
    LOGIC_OP = 0x102,
    BLEND_COLOR = 0x103,
    ALPHA_TEST = 0x104,
    STENCIL_TEST = 0x105,
    STENCIL_OP = 0x106,
    DEPTH_COLOR_MASK = 0x107,
    FB_INVALIDATE = 0x110,
    FB_FLUSH = 0x111,
    COLOR_READ = 0x112,
    COLOR_WRITE = 0x113,
    DEPTH_READ = 0x114,
    DEPTH_WRITE = 0x115,
    DEPTH_FORMAT = 0x116,
    COLOR_FORMAT = 0x117,
    DEPTH_LOC = 0x11C,
    COLOR_LOC = 0x11D,
    FB_DIM = 0x11E,
    ATTR_BASE = 0x200,
    ATTR_FORMAT_LOW = 0x201,
    ATTR_FORMAT_HIGH = 0x202,
    ATTR_BUFFER0 = 0x203,  // 3 words per buffer, 12 buffers
    INDEX_CONFIG = 0x227,
    NUM_VERTICES = 0x228,
    GEOSTAGE_CONFIG = 0x229,
    VERTEX_OFFSET = 0x22A,
    DRAW_ARRAYS = 0x22E,
    DRAW_ELEMENTS = 0x22F,
    VTX_FUNC = 0x231,
    FIXED_INDEX = 0x232,
    FIXED_DATA0 = 0x233,  // ..0x235
    CMDBUF_SIZE0 = 0x238,
    CMDBUF_SIZE1 = 0x239,
    CMDBUF_ADDR0 = 0x23A,
    CMDBUF_ADDR1 = 0x23B,
    CMDBUF_JUMP0 = 0x23C,
    CMDBUF_JUMP1 = 0x23D,
    VSH_NUM_ATTR = 0x242,
    PRIMITIVE_CONFIG = 0x25E,
    RESTART_PRIMITIVE = 0x25F,
    VSH_BOOL_UNIFORM = 0x2B0,
    VSH_INT_UNIFORM0 = 0x2B1,  // ..0x2B4
    VSH_INPUT_CONFIG = 0x2B9,
    VSH_ENTRY = 0x2BA,
    VSH_PERMUTATION_LOW = 0x2BB,
    VSH_PERMUTATION_HIGH = 0x2BC,
    VSH_OUTMAP_MASK = 0x2BD,
    VSH_FLOAT_INDEX = 0x2C0,
    VSH_FLOAT_DATA0 = 0x2C1,  // ..0x2C8
    VSH_CODE_INDEX = 0x2CB,
    VSH_CODE_DATA0 = 0x2CC,  // ..0x2D3
    VSH_OPDESC_INDEX = 0x2D5,
    VSH_OPDESC_DATA0 = 0x2D6,  // ..0x2DD
};

// Vertex shader state.
struct Shader {
    std::array<u32, 512> code{};
    std::array<u32, 128> opdesc{};
    std::array<Vec4, 96> f{};      // float uniforms
    std::array<u32, 4> i{};        // int uniforms (x, y, z, w bytes)
    u16 b = 0;                     // bool uniforms
    u32 codeIndex = 0, opdescIndex = 0;
    u32 floatIndex = 0;
    bool float32 = false;
    u32 floatWords[4] = {};
    u32 floatCount = 0;
};

// A vertex after the shader, as the rasteriser uses it.
struct Vertex {
    Vec4 pos, color, tc0, tc1, tc2, view, quat;
    float tc0w = 0;
};

struct State {
    std::array<u32, 0x300> regs{};
    Shader vs;
    // Immediate-mode / fixed attributes.
    u32 fixedIndex = 0;
    u32 fixedWords[3] = {};
    u32 fixedCount = 0;
    std::array<Vec4, 16> fixedAttr{};
    std::array<Vec4, 16> immediate{};
    u32 immediateAttr = 0;
    // Primitive assembly across draws (strips, fans, immediate mode).
    Vertex pending[3];
    u32 pendingCount = 0;
    bool stripOdd = false;
};

extern State g;

// Counters for diagnostics.
struct Stats {
    u64 lists = 0, draws = 0, vertices = 0, triangles = 0, pixels = 0;
    // Why triangles or fragments went nowhere.
    u64 noTarget = 0, culled = 0, offscreen = 0, depthFail = 0, alphaFail = 0, masked = 0;
};
extern Stats g_stats;

float Float24(u32 v);
float Float16(u32 v);
float Float31(u32 v);

// The vertex shader: runs the program on inputs v[16], outputs o[16].
void RunShader(const Shader& sh, u32 entry, const Vec4* in, Vec4* out);
// Primitive assembly and rasterisation (pica_raster.cpp).
void SubmitVertex(const Vertex& v);
void ResetPrimitive();
// The state is about to change: triangles queued after this take a new
// snapshot of it.
void FlushTriangles();
// Draws every queued triangle (end of a command list, or a barrier).
void ExecuteTriangles();
void DrawTriangle(const Vertex& a, const Vertex& b, const Vertex& c);

}  // namespace pica
}  // namespace rt
