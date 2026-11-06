/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <string.h>
#include "nx/gpu/texture/blocklinear.h"

/* GOBs are walked in memory order, so the whole ones next to each other in memory (a block's, and the blocks of a
   row of blocks when the row's width is a multiple of 64 bytes) are one memcpy */
void bl_land(u8* dst, const u8* src, u32 row_bytes, u32 h, u32 d, u32 bh, u32 bd) {
    u32 gobs_x = (row_bytes + 63) >> 6;
    u64 block_rows = ((((u64)h + 7) >> 3) + (1u << bh) - 1) >> bh;
    u64 slabs = ((u64)d + (1u << bd) - 1) >> bd;
    u64 gobs = (slabs * block_rows * gobs_x) << (bh + bd), whole = 0, g;
    for (g = 0; g < gobs; g++) {
        u64 block = g >> (bh + bd), brow = block / gobs_x;
        u32 in = (u32)g & ((1u << (bh + bd)) - 1), gx = (u32)(block % gobs_x);
        u64 gy = ((brow % block_rows) << bh) + (in & ((1u << bh) - 1));
        u64 z = ((brow / block_rows) << bd) + (in >> bh);
        if ((u64)gx * 64 + 64 <= row_bytes && gy * 8 + 8 <= h && z < d) continue; /* whole: copied with its run */
        if (whole < g) memcpy(dst + whole * 512, src + whole * 512, (size_t)(g - whole) * 512);
        whole = g + 1;
        if (gy * 8 >= h || z >= d) continue; /* padding */
        u32 rows = h - (u32)gy * 8 < 8 ? h - (u32)gy * 8 : 8, x, y;
        u32 cols = row_bytes - gx * 64 < 64 ? row_bytes - gx * 64 : 64;
        for (y = 0; y < rows; y++)
            for (x = 0; x < cols; x += 16) {
                u64 at = g * 512 + ((y >> 1) << 6) + ((y & 1) << 4) + ((x & 32) << 3) + ((x & 16) << 1);
                memcpy(dst + at, src + at, cols - x < 16 ? cols - x : 16);
            }
    }
    if (whole < gobs) memcpy(dst + whole * 512, src + whole * 512, (size_t)(gobs - whole) * 512);
}
