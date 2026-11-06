/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/kepler_compute.cpp, src/video_core/engines/kepler_compute.h,
   src/video_core/engines/kepler_memory.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The compute engine (KEPLER_COMPUTE_B, 0xB1C0): launches, which the renderer runs from their launch description
   (the QMD), its inline-to-memory methods and its report semaphore; and the inline-to-memory class
   (KEPLER_INLINE_TO_MEMORY_B, 0xA140) */
#include "nx/gpu/maxwell/engines_int.h"

/* With diag=1, each new compute program's launch description and code go to captures/compute, for debugging: the
   first 64 programs */
static void capture(const u32* q, u64 code) {
    static u64 seen[64];
    static u32 nseen;
    u64 pa = code + q[8];
    char path[sizeof g_exedir + 64];
    u8* buf;
    FILE* f;
    u32 i;
    for (i = 0; i < nseen; i++)
        if (seen[i] == pa) return;
    if (nseen == sizeof seen / sizeof seen[0]) return;
    seen[nseen++] = pa;
    buf = (u8*)malloc(0x100 + 0x10000);
    if (!buf) return;
    memcpy(buf, q, 0x100);
    snx_le32_array(buf, 64); /* the description as guest memory held it */
    gmem_read_code(pa, buf + 0x100, 0x10000); /* up to where guest memory ends */
    snprintf(path, sizeof path, "%scaptures", g_exedir);
    plat_mkdir(path);
    snprintf(path, sizeof path, "%scaptures/compute", g_exedir);
    plat_mkdir(path);
    snprintf(path, sizeof path, "%scaptures/compute/cs_%010llx.bin", g_exedir, (unsigned long long)pa);
    f = fopen(path, "wb");
    if (f) {
        if (fwrite(buf, 1, 0x100 + 0x10000, f) != 0x100 + 0x10000) WARN_LIMIT(1, "gpu: %s wasn't written\n", path);
        fclose(f);
    }
    free(buf);
}
/* LAUNCH (0xAF): the launch description at LAUNCH_DESC_LOC (0xAD, 256-byte units), the program at its offset (QMD
   word 8) from CODE_LOC (0x582-0x583) */
static void launch(GpuChannel* ch) {
    const u32* r = ch->rcomp;
    u32 q[64];
    u64 code = ((u64)(r[0x582] & 0xFF) << 32) | r[0x583];
    gmem_read((u64)r[0xAD] << 8, q, sizeof q);
    snx_le32_array(q, 64); /* the renderer reads its words and bitfields as numbers */
    if (g_settings.shader_captures) capture(q, code);
    if (g_renderer->dispatch) g_renderer->dispatch(ch, q, code);
}
void compute_method(GpuChannel* ch, u32 m, u32 v, int last) {
    (void)last;
    m &= 0xFFF;
    ch->rcomp[m] = v;
    switch (m) {
        case 0x6C: i2m_launch(ch, ch->rcomp, 0x60); break;
        case 0x6D: i2m_data(ch, v); break;
        case 0xAF: launch(ch); break;
        case 0x6C3: query_get(ch, ch->rcomp, 0x6C0, v); break; /* the report semaphore, the 3D engine's */
        default: break;
    }
}
void kepler_i2m_method(GpuChannel* ch, u32 m, u32 v) {
    if (m >= 0x80) return;
    ch->ri2m[m] = v;
    if (m == 0x6C)
        i2m_launch(ch, ch->ri2m, 0x60);
    else if (m == 0x6D)
        i2m_data(ch, v);
}
