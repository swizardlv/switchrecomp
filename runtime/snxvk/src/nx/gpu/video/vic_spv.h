/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The VIC shader's SPIR-V (vic.comp): the build for word aligned rows and the -DVIC_UNALIGNED one. The words are in
   vic_spv.c and vic_unaligned_spv.c, which the build makes from the shader with glslang (cmake/glsl.cmake): nothing
   compiled is checked in. */
#ifndef SNX_VIC_SPV_H
#define SNX_VIC_SPV_H
#include "common/cpu/aarch64/cpu.h"

extern const u32 vic_spv[];
extern const u64 vic_spv_size; /* in bytes */
extern const u32 vic_unaligned_spv[];
extern const u64 vic_unaligned_spv_size;

#endif
