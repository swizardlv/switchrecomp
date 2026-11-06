/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The VIC shader's parameters, its uniform buffer (vic.comp). One declaration for three languages: GLSL reads it as
   it is, C through gpu/video/vic.h (uint defined as u32 around the include, the struct tags typedef'd before it),
   and the tests' C++ through tests/glsl/glsl.hpp. Every member is 32 bit and every struct a multiple of 16 bytes
   that starts on 16 bytes, so std140 lays it out exactly as C does.

   gpu/video/vic.c's vic_gpu_params fills it from a VicJob and moves every per-job invariant out of the per-pixel
   code: the clamp, the alpha and the background are output bytes already, the matrix's coefficients are scaled by
   4 so the 8 bit samples need no shift to 10 bit, the rectangles are ends and offsets, and the block linear
   constants are shifts and masks. */

/* a workgroup is 16x4 invocations and each owns 4x2 pixels, so a workgroup composes a 64x8 tile: one GOB of 8 bit
   luma, four of RGBA */
#define VIC_GROUP_W 16u
#define VIC_GROUP_H 4u
#define VIC_TILE_W  64u
#define VIC_TILE_H  8u

/* a plane: byte x of row y is at base + xpart(x) + ypart(y) in its buffer (vic_compose.glsl) */
struct VicGpuPlane {
    uint base;    /* the plane's byte offset in its buffer */
    uint lim;     /* bytes it may touch: 0 for none (an unmapped output chroma plane) */
    uint blk;     /* 0 pitch linear, 1 block linear */
    uint pitch;   /* pitch linear: the row pitch */
    uint bsh;     /* block linear: 9 + the block height log2, the shift of a block column (and row) of GOBs */
    uint ysh;     /* 3 + the block height log2: the row's block row */
    uint gm;      /* (block height - 1) << 9: the GOB in the block, as (y << 6) & gm */
    uint bstride; /* a block row's bytes: GOBs in a row << bsh */
};

struct VicGpuSlot {
    uint dx0, dx1, dy0, h; /* the destination rectangle: columns dx0 to dx1 - 1, h rows from dy0 */
    uint ox, oy;           /* source minus destination column and row, modulo 2^32 */
    uint mat, msh;         /* the colour matrix enabled, its shift */
    int ry, ru, rv;        /* R's row of the matrix, c0 to c2 times 4 (the samples are 8 bit, the matrix takes 10) */
    int gy, gu, gv;
    int by, bu, bv;
    int r3, g3, b3; /* c3 of each row */
    int lo, hi;     /* the soft clamp, 10 bit (lo may be above hi: vic_clamp) */
    uint aw;        /* the planar alpha's output byte in place, (alpha >> 2) << 24 */
    uint ush, vsh;  /* U's and V's bit in a chroma pair of the input: 0 and 8, or 8 and 0 for NV21 */
    uint cswap;     /* NV12 or NV21 out without the matrix: the input's chroma order isn't the output's */
    uint pad0, pad1;
    VicGpuPlane iy, ic;
};

struct VicGpuJob {
    uint nv12;             /* NV12 or NV21 out, else one RGBA word a pixel */
    uint rpos, gpos, bpos; /* RGBA: R's, G's and B's bit in the word; NV12: G's and B's (U, V) in a chroma pair */
    uint tx0, tx1, ty0, th; /* the target rectangle: columns tx0 to tx1 - 1, th rows from ty0 */
    uint gx0, gy0;          /* the dispatch's first pixel: the target's corner rounded down to the 4x2 blocks */
    uint nslots;            /* the enabled slots, in slot order, at the front of slots */
    uint bgw;               /* the background's RGBA word */
    uint bgy, bgc;          /* NV12: its luma byte in every byte, its chroma pair in both halves */
    uint pad0, pad1;
    VicGpuPlane ol, oc; /* the output planes, in the output buffer: luma (or RGBA) and chroma */
};
