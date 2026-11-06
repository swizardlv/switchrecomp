/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/texture/depth.h"

/* Where a texel keeps its depth, from the TIC formats' component types (component 0 in the low bits): Eden's format
   table reads G24R8 and Z24S8 with R UINT as S8_UINT_D24_UNORM, and S8Z24 with R UNORM as D24_UNORM_S8_UINT, as
   deko3d's format table does. So G24R8 and Z24S8 have stencil in the low byte and depth above it, G8R24, X8Z24 and
   S8Z24 depth in the low 24 bits and the stencil (or nothing) above (G8R24's R is its 24 bits, vk_tic_reads_stencil). Depth-surface names run the other way round */
u32 depth24_unpack(u32 tic_format, u32 texel, u8* stencil) {
    if (tic_format == 0x0D || tic_format == 0x2A || tic_format == 0x2B) {
        *stencil = tic_format == 0x2A ? 0 : (u8)(texel >> 24);
        return texel & 0xFFFFFF;
    }
    *stencil = (u8)texel;
    return texel >> 8;
}
