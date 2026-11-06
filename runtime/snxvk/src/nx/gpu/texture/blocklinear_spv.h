/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The block linear deswizzle and swizzle's SPIR-V (blocklinear.comp). The words are in blocklinear_spv.c, which the
   build makes from the shader with glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_BLOCKLINEAR_SPV_H
#define SNX_BLOCKLINEAR_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 blocklinear_spv[];
extern const u64 blocklinear_spv_size; /* in bytes */

#endif
