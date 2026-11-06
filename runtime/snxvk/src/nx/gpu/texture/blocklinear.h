/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Block linear surfaces copied between two buffers of the same layout (blocklinear.c). The GPU swizzles a render
   target written back (blocklinear.comp), and this lands its bytes in guest memory */
#ifndef SNX_TEXTURE_BLOCKLINEAR_H
#define SNX_TEXTURE_BLOCKLINEAR_H
#include "common/cpu/aarch64/cpu.h"

/* the bytes of a surface row_bytes wide, h rows high and d slices deep, in blocks of 2^bh GOBs high and 2^bd deep
   (gpu.c's bl_offset), from src to dst, both in that layout. The padding (bytes past a row, rows past h, slices past
   d) is left as dst had it: whole GOBs inside the surface go in runs as long as memory has them, those on its edges
   16 bytes or less at a time */
void bl_land(u8* dst, const u8* src, u32 row_bytes, u32 h, u32 d, u32 bh, u32 bd);

#endif
