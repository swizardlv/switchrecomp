/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/fermi_2d.cpp, src/video_core/engines/fermi_2d.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The 2D engine (FERMI_TWOD_A, 0x902D): a blit from one surface to another, scaled, started by the last word of the
   source origin (0x237). The renderer does it on the GPU: as an image blit when the source is a render target, else
   from memory (vk/blit.c), with the source pixels the blit samples worked out here, nearest sampled */
#include "nx/gpu/maxwell/engines_int.h"

/* a surface's registers from b: format, layout, block size, depth, layer, pitch, width, height, address */
static void f2d_surface(const u32* r, u32 b, Surf2D* s) {
    s->format = r[b];
    s->linear = r[b + 1] & 1;
    s->bh = (r[b + 2] >> 4) & 15;
    s->depth = r[b + 3];
    s->layer = r[b + 4];
    s->pitch = r[b + 5];
    s->width = r[b + 6];
    s->height = r[b + 7];
    s->addr = ((u64)(r[b + 8] & 0xFF) << 32) | r[b + 9];
}
/* a surface's slice as a plane: a 3D surface's layer is its z, with the block depth of its block size register; a 2D
   array's layer is a whole number of block rows further */
static GpuDmaSide f2d_plane(const Surf2D* s, u32 blk, u32 bpp) {
    GpuDmaSide pl;
    memset(&pl, 0, sizeof pl);
    pl.addr = s->addr;
    pl.bl = !s->linear;
    pl.pitch = s->pitch;
    pl.wb = s->width * bpp; /* a row's bytes; pitch linear rows are pitch apart */
    pl.h = s->height;
    pl.bh = s->bh;
    if (!s->linear) {
        if (s->depth > 1) {
            pl.z = s->layer;
            pl.bd = (blk >> 8) & 15;
        } else if (s->layer)
            pl.addr += (((u64)pl.wb + 63) & ~(u64)63) * ((((u64)s->height + (8u << s->bh) - 1) >> (3 + s->bh))
                                                          << (3 + s->bh)) * s->layer;
    }
    return pl;
}
/* the blit from memory, which the renderer does on the GPU (its destination kept there): which source column and row
   each destination column and row takes, nearest sampled, worked out here */
static void f2d_blit_memory(const u32* r, const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx,
                            double sy, double du, double dv) {
    u32 bs = gpu_color_bpp(s->format), bd = gpu_color_bpp(d->format), n, i, y0, y1, x0, x1;
    GpuBlit b;
    u32* tab;
    if (bs != bd) {
        WARN_LIMIT(4, "gpu: a 2D blit from format %x to %x isn't done\n", s->format, d->format);
        return;
    }
    if (s->format != d->format)
        LOG_LIMIT(4, "gpu: a 2D blit from format %x to %x copies the bytes\n", s->format, d->format);
    if (!s->width || !s->height) return;
    x0 = dx > 0 ? (u32)dx : 0;
    y0 = dy > 0 ? (u32)dy : 0;
    x1 = (i64)dx + dw < (i64)d->width ? (u32)((i64)dx + dw) : d->width;
    y1 = (i64)dy + dh < (i64)d->height ? (u32)((i64)dy + dh) : d->height;
    if (x0 >= x1 || y0 >= y1) return;
    n = x1 - x0;
    tab = (u32*)malloc(sizeof *tab * ((size_t)n + (y1 - y0)));
    if (!tab) {
        WARN_LIMIT(1, "gpu: no memory for a 2D blit %u pixels wide\n", n);
        return;
    }
    for (i = 0; i < n; i++) { /* the source column under each destination pixel's center, clamped to the surface */
        double fx = sx + ((double)(x0 + i - dx) + 0.5) * du;
        tab[i] = fx <= 0 ? 0 : fx >= (double)s->width ? s->width - 1 : (u32)fx;
    }
    for (i = y0; i < y1; i++) {
        double fy = sy + ((double)(i - dy) + 0.5) * dv;
        tab[n + i - y0] = fy <= 0 ? 0 : fy >= (double)s->height ? s->height - 1 : (u32)fy;
    }
    b.src = f2d_plane(s, r[0x8E], bs);
    b.dst = f2d_plane(d, r[0x82], bd);
    b.bpp = bs;
    b.x0 = x0, b.x1 = x1, b.y0 = y0, b.y1 = y1;
    b.scol = tab;
    b.srow = tab + n;
    if (!g_renderer_up || !g_renderer->blit_mem)
        WARN_LIMIT(4, "gpu: a 2D blit before the renderer started, skipped\n");
    else if (!g_renderer->blit_mem(&b))
        WARN_LIMIT(4, "gpu: the GPU could not do a 2D blit, skipped\n");
    else if (g_renderer->rt_reload)
        g_renderer->rt_reload(d->addr);
    free(tab);
}
static double f2d_fixed(const u32* r, u32 lo) { /* a signed 32.32 value from two registers, the fraction first */
    return (double)(i64)(((u64)r[lo + 1] << 32) | r[lo]) / 4294967296.0;
}
static void f2d_blit(GpuChannel* ch) {
    const u32* r = ch->r2d;
    Surf2D s, d;
    int dx = (int)r[0x22C], dy = (int)r[0x22D], dw = (int)r[0x22E], dh = (int)r[0x22F];
    double du = f2d_fixed(r, 0x230), dv = f2d_fixed(r, 0x232), sx = f2d_fixed(r, 0x234), sy = f2d_fixed(r, 0x236);
    f2d_surface(r, 0x80, &d);
    f2d_surface(r, 0x8C, &s);
    if (dw <= 0 || dh <= 0) return;
    /* CORNER origin (0x223 bit 0) samples a step's start, CENTER its middle, as the renderer does: a corner's start is
       half a step back (Eden's Fermi2D::Blit) */
    if (r[0x223] & 1) {
        sx -= du / 2;
        sy -= dv / 2;
    }
    if (r[0x2AC] != 3 && r[0x2AC]) LOG_LIMIT(4, "gpu: a 2D blit's operation %u is done as a copy\n", r[0x2AC]);
    if (r3d_blit(&s, &d, dx, dy, dw, dh, sx, sy, du, dv)) return;
    f2d_blit_memory(r, &s, &d, dx, dy, dw, dh, sx, sy, du, dv);
}
void fermi2d_method(GpuChannel* ch, u32 m, u32 v) {
    if (m >= 0x400) return;
    ch->r2d[m] = v;
    if (m == 0x237) f2d_blit(ch);
}
