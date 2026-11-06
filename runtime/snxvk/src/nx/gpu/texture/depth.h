/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Four-byte packed depth/stencil texels (depth.c), split for Vulkan's per-aspect copies */
#ifndef SNX_TEXTURE_DEPTH_H
#define SNX_TEXTURE_DEPTH_H
#include "common/cpu/aarch64/cpu.h"

/* The 24-bit depth of a texel of TIC format tic_format (G8R24 0x0D, G24R8 0x0E, Z24S8 0x29, X8Z24 0x2A or S8Z24
   0x2B), its stencil in *stencil (0 for X8Z24) */
u32 depth24_unpack(u32 tic_format, u32 texel, u8* stencil);

#endif
