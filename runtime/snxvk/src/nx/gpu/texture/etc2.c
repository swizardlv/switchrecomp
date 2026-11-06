/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ETC2 and EAC decoding, from the Khronos Data Format Specification's ETC2 chapter. A block is read as big-endian
   64 bit words; a pixel (x, y) of a 4x4 block is number x * 4 + y in the index bits (columns first). */
#include "nx/gpu/texture/etc2.h"
#include "common/utilities/endian.h"
#include <string.h>

/* the ETC1 intensity modifiers, per table codeword, for pixel indices 0 to 3 */
static const int etc1_mod[8][4] = {{2, 8, -2, -8},     {5, 17, -5, -17},   {9, 29, -9, -29},     {13, 42, -13, -42},
                                   {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}};
/* the T and H modes' distances */
static const int etc2_dist[8] = {3, 6, 11, 16, 23, 32, 41, 64};
/* the EAC modifiers, per table index, for pixel indices 0 to 7 */
static const int eac_mod[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},  {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8}};

static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static u32 bits(u64 v, int hi, int lo) { return (u32)((v >> lo) & ((1ull << (hi - lo + 1)) - 1)); }
static u64 be64(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}
static int ext4(u32 v) { return (int)(v << 4 | v); }
static int ext5(u32 v) { return (int)(v << 3 | v >> 2); }
static int ext6(u32 v) { return (int)(v << 2 | v >> 4); }
static int ext7(u32 v) { return (int)(v << 1 | v >> 6); }
static int sext3(u32 v) { return (v & 4) ? (int)v - 8 : (int)v; }

/* the pixel index (0 to 3) of pixel i from the block's low word: its MSB plane, then its LSB plane */
static int etc_index(u32 lo, int i) { return (int)(((lo >> (16 + i)) & 1) << 1 | ((lo >> i) & 1)); }

static void put(u8 out[16][4], int i, int r, int g, int b, int a) {
    out[i][0] = (u8)clamp_i(r, 0, 255);
    out[i][1] = (u8)clamp_i(g, 0, 255);
    out[i][2] = (u8)clamp_i(b, 0, 255);
    out[i][3] = (u8)a;
}

/* an ETC2 RGB block into out[pixel number], alpha 255. punch: ETC2_RGB_PTA, whose differential bit is the opaque bit:
   a block without it has a transparent index 2, and its differential modifiers for index 0 are 0 */
static void etc2_rgb_block(u64 b, int punch, u8 out[16][4]) {
    int diff = (int)bits(b, 33, 33), flip = (int)bits(b, 32, 32);
    int see_through = punch && !diff;
    u32 lo = (u32)b;
    int base[2][3], cw[2] = {(int)bits(b, 39, 37), (int)bits(b, 36, 34)};
    if (!punch && !diff) { /* individual: two 4 bit colors */
        base[0][0] = ext4(bits(b, 63, 60));
        base[1][0] = ext4(bits(b, 59, 56));
        base[0][1] = ext4(bits(b, 55, 52));
        base[1][1] = ext4(bits(b, 51, 48));
        base[0][2] = ext4(bits(b, 47, 44));
        base[1][2] = ext4(bits(b, 43, 40));
    } else { /* differential, unless the second color overflows: then T, H or planar */
        int r = (int)bits(b, 63, 59), g = (int)bits(b, 55, 51), bl = (int)bits(b, 47, 43);
        int dr = sext3(bits(b, 58, 56)), dg = sext3(bits(b, 50, 48)), db = sext3(bits(b, 42, 40));
        if (r + dr < 0 || r + dr > 31) { /* T */
            int c1[3] = {ext4(bits(b, 60, 59) << 2 | bits(b, 57, 56)), ext4(bits(b, 55, 52)), ext4(bits(b, 51, 48))};
            int c2[3] = {ext4(bits(b, 47, 44)), ext4(bits(b, 43, 40)), ext4(bits(b, 39, 36))};
            int d = etc2_dist[bits(b, 35, 34) << 1 | bits(b, 32, 32)];
            for (int i = 0; i < 16; i++) {
                int k = etc_index(lo, i);
                if (see_through && k == 2)
                    put(out, i, 0, 0, 0, 0);
                else if (k == 0)
                    put(out, i, c1[0], c1[1], c1[2], 255);
                else {
                    int m = k == 1 ? d : k == 3 ? -d : 0;
                    put(out, i, c2[0] + m, c2[1] + m, c2[2] + m, 255);
                }
            }
            return;
        }
        if (g + dg < 0 || g + dg > 31) { /* H */
            u32 r1 = bits(b, 62, 59), g1 = bits(b, 58, 56) << 1 | bits(b, 52, 52);
            u32 b1 = bits(b, 51, 51) << 3 | bits(b, 49, 47);
            u32 r2 = bits(b, 46, 43), g2 = bits(b, 42, 39), b2 = bits(b, 38, 35);
            /* the distance index's low bit is which base color is the larger */
            u32 da =
                bits(b, 34, 34) << 2 | bits(b, 32, 32) << 1 | ((r1 << 8 | g1 << 4 | b1) >= (r2 << 8 | g2 << 4 | b2));
            int d = etc2_dist[da];
            int c[2][3] = {{ext4(r1), ext4(g1), ext4(b1)}, {ext4(r2), ext4(g2), ext4(b2)}};
            for (int i = 0; i < 16; i++) {
                int k = etc_index(lo, i);
                if (see_through && k == 2) {
                    put(out, i, 0, 0, 0, 0);
                    continue;
                }
                int m = (k & 1) ? -d : d;
                put(out, i, c[k >> 1][0] + m, c[k >> 1][1] + m, c[k >> 1][2] + m, 255);
            }
            return;
        }
        if (bl + db < 0 || bl + db > 31) { /* planar: three colors at the corners, interpolated; always opaque */
            int o[3] = {ext6(bits(b, 62, 57)), ext7(bits(b, 56, 56) << 6 | bits(b, 54, 49)),
                        ext6(bits(b, 48, 48) << 5 | bits(b, 44, 43) << 3 | bits(b, 41, 39))};
            int h[3] = {ext6(bits(b, 38, 34) << 1 | bits(b, 32, 32)), ext7(bits(b, 31, 25)), ext6(bits(b, 24, 19))};
            int v[3] = {ext6(bits(b, 18, 13)), ext7(bits(b, 12, 6)), ext6(bits(b, 5, 0))};
            for (int x = 0; x < 4; x++)
                for (int y = 0; y < 4; y++) {
                    int px[3];
                    for (int c = 0; c < 3; c++) px[c] = (x * (h[c] - o[c]) + y * (v[c] - o[c]) + 4 * o[c] + 2) >> 2;
                    put(out, x * 4 + y, px[0], px[1], px[2], 255);
                }
            return;
        }
        base[0][0] = ext5((u32)r);
        base[1][0] = ext5((u32)(r + dr));
        base[0][1] = ext5((u32)g);
        base[1][1] = ext5((u32)(g + dg));
        base[0][2] = ext5((u32)bl);
        base[1][2] = ext5((u32)(bl + db));
    }
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            int i = x * 4 + y, k = etc_index(lo, i), s = flip ? y >= 2 : x >= 2;
            if (see_through && k == 2) {
                put(out, i, 0, 0, 0, 0);
                continue;
            }
            int m = see_through && k == 0 ? 0 : etc1_mod[cw[s]][k];
            put(out, i, base[s][0] + m, base[s][1] + m, base[s][2] + m, 255);
        }
}

/* an EAC block's pixel indices (0 to 7) and its fields */
static int eac_index(u64 b, int i) { return (int)((b >> (45 - 3 * i)) & 7); }

/* an 8 bit EAC alpha block (ETC2_RGBA) into out[i][3] */
static void eac_alpha_block(u64 b, u8 out[16][4]) {
    int base = (int)bits(b, 63, 56), mult = (int)bits(b, 55, 52), t = (int)bits(b, 51, 48);
    for (int i = 0; i < 16; i++) out[i][3] = (u8)clamp_i(base + eac_mod[t][eac_index(b, i)] * mult, 0, 255);
}
/* an 11 bit EAC block into out[i], as 16 bit unorm or snorm */
static void eac11_block(u64 b, int sn, u16 out[16]) {
    int mult = (int)bits(b, 55, 52), t = (int)bits(b, 51, 48);
    for (int i = 0; i < 16; i++) {
        int m = eac_mod[t][eac_index(b, i)];
        if (!sn) {
            int v = clamp_i((int)bits(b, 63, 56) * 8 + 4 + (mult ? m * mult * 8 : m), 0, 2047);
            out[i] = (u16)(v << 5 | v >> 6);
        } else {
            int base = (int)(i8)bits(b, 63, 56);
            if (base == -128) base = -127;
            int v = clamp_i(base * 8 + (mult ? m * mult * 8 : m), -1023, 1023);
            int a = v < 0 ? -v : v;
            int e = a << 5 | a >> 5;
            out[i] = (u16)(i16)(v < 0 ? -e : e);
        }
    }
}

u32 etc2_block_bytes(int kind) { return kind == ETC2_RGBA || kind == EAC_RG || kind == EAC_RG_SNORM ? 16 : 8; }
u32 etc2_pixel_bytes(int kind) { return kind == EAC_R || kind == EAC_R_SNORM ? 2 : 4; }

void etc2_decode_image(const u8* src, u32 w, u32 h, int kind, u8* dst) {
    u32 bw = (w + 3) / 4, bh = (h + 3) / 4, bb = etc2_block_bytes(kind), pb = etc2_pixel_bytes(kind);
    for (u32 by = 0; by < bh; by++)
        for (u32 bx = 0; bx < bw; bx++) {
            const u8* blk = src + ((u64)by * bw + bx) * bb;
            u8 rgba[16][4];
            u16 ch[2][16];
            switch (kind) {
                case ETC2_RGB:
                case ETC2_RGB_PTA: etc2_rgb_block(be64(blk), kind == ETC2_RGB_PTA, rgba); break;
                case ETC2_RGBA:
                    etc2_rgb_block(be64(blk + 8), 0, rgba);
                    eac_alpha_block(be64(blk), rgba);
                    break;
                case EAC_R:
                case EAC_R_SNORM: eac11_block(be64(blk), kind == EAC_R_SNORM, ch[0]); break;
                default:
                    eac11_block(be64(blk), kind == EAC_RG_SNORM, ch[0]);
                    eac11_block(be64(blk + 8), kind == EAC_RG_SNORM, ch[1]);
                    break;
            }
            for (u32 x = 0; x < 4; x++)
                for (u32 y = 0; y < 4; y++) {
                    u32 px = bx * 4 + x, py = by * 4 + y, i = x * 4 + y;
                    if (px >= w || py >= h) continue;
                    u8* o = dst + ((u64)py * w + px) * pb;
                    if (kind <= ETC2_RGBA)
                        memcpy(o, rgba[i], 4);
                    else {
                        snx_st_le16(o, ch[0][i]); /* R16 and R16G16 texels, little-endian as the GPU reads them */
                        if (pb == 4) snx_st_le16(o + 2, ch[1][i]);
                    }
                }
        }
}
