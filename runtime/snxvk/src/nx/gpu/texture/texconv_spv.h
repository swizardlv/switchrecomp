/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The texture conversions' SPIR-V (texconv.comp). The words are in texconv_spv.c, which the build makes from the
   shader with glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_TEXCONV_SPV_H
#define SNX_TEXCONV_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 texconv_spv[];
extern const u64 texconv_spv_size; /* in bytes */

#endif
