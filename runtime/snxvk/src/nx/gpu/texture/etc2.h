/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ETC2 and EAC decoding on the CPU (etc2.c): the Khronos Data Format Specification's ETC2 chapter, 4x4 blocks. The
   reference the GPU's decoder (texconv.glsl, for GPUs that can't sample them) is tested against */
#ifndef SNX_ETC2_H
#define SNX_ETC2_H
#include "common/cpu/aarch64/cpu.h"

enum {
    ETC2_RGB,     /* 8 bytes a block, opaque RGB */
    ETC2_RGB_PTA, /* 8 bytes, RGB with punch-through alpha: a block whose opaque bit is clear has a transparent index */
    ETC2_RGBA,    /* 16 bytes: an EAC alpha block, then an ETC2 RGB block */
    EAC_R,        /* 8 bytes: one 11 bit channel, unsigned */
    EAC_R_SNORM,  /* the same, signed */
    EAC_RG,       /* 16 bytes: two EAC blocks, red then green, unsigned */
    EAC_RG_SNORM, /* the same, signed */
};

/* a w x h image of 4x4 blocks of kind (rows of whole blocks, row after row) to pixels: RGBA8 for the ETC2 kinds,
   one or two 16 bit channels for the EAC ones (R16 or R16G16, unorm or snorm as the kind says) */
void etc2_decode_image(const u8* src, u32 w, u32 h, int kind, u8* dst);
/* the bytes of one block of kind, and of one output pixel */
u32 etc2_block_bytes(int kind);
u32 etc2_pixel_bytes(int kind);

#endif
