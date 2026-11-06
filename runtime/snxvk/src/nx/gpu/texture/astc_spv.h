/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ASTC decoder's SPIR-V (astc.comp). The words are in astc_spv.c, which the build makes from the shader with
   glslang (cmake/glsl.cmake): nothing compiled is checked in. */
#ifndef SNX_ASTC_SPV_H
#define SNX_ASTC_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 astc_spv[];
extern const u64 astc_spv_size; /* in bytes */

#endif
