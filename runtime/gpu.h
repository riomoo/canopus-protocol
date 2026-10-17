// The 3DS GPU as the game drives it through GSP: memory fills, display
// transfers (render target -> framebuffer), texture copies, DMA, and PICA200
// command lists. Addresses here are the game's virtual addresses unless
// noted; PICA registers hold physical ones.
#pragma once

#include "rt.h"

namespace rt {
namespace gpu {

// Physical <-> virtual: FCRAM through the linear heap, VRAM.
u32 PhysToVirt(u32 pa);
u32 VirtToPhys(u32 va);

void MemoryFill(u32 start, u32 end, u32 value, u32 control);
void DisplayTransfer(u32 in, u32 out, u32 inDims, u32 outDims, u32 flags);
void TextureCopy(u32 in, u32 out, u32 size, u32 inGap, u32 outGap, u32 flags);
// Runs a PICA command list (virtual address, size in bytes).
void ProcessCommandList(u32 addr, u32 size);

// Pixel formats of framebuffers and display transfers.
enum Format { RGBA8 = 0, RGB8 = 1, RGB565 = 2, RGB5A1 = 3, RGBA4 = 4 };
u32 BytesPerPixel(u32 format);
// Reads a pixel of a linear buffer as 0xAARRGGBB.
u32 ReadPixel(const u8* p, u32 format);

}  // namespace gpu
}  // namespace rt
