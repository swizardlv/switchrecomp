/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The body of texconv.comp: texels of a format the host can't take as they are, converted on the GPU from linear rows
   (what blocklinear.comp made of a block linear level, or a pitch linear level's rows staged as they are) into rows
   that copy into the image. GLSL that is also C++ (tests/glsl/glsl.hpp): tests/glsl/texconv_glsl.cpp runs it against
   the CPU's conversions (gpu/texture/etc2.c for ETC2 and EAC).

   It also converts the frames vi presents from memory to RGBA8 (vk/renderer.c).

   An invocation owns whole output words: a pair of 2 byte texels, a 4 byte or RGB32F texel, four depth texels
   (whose four stencil bytes are one word) or a 4x4 block. Output rows are padded to that (vk/texconv.c gives the
   copies a bufferRowLength), so no invocation writes part of a word another one writes. Everything is integer arithmetic,
   with the CPU's results to the bit: the one float, a 24 bit depth as a float, is built from its bits. */

uint tc_bswap(uint w) {
    return (w << 24) | ((w & 0xFF00u) << 8) | ((w >> 8) & 0xFF00u) | (w >> 24);
}
/* bits h down to l of the 64 bit word hi:lo (as etc2.c's bits) */
uint tc_bits(uint hi, uint lo, uint h, uint l) {
    uint v = l >= 32u ? hi >> (l - 32u) : l == 0u ? lo : (lo >> l) | (hi << (32u - l));
    return v & ((2u << (h - l)) - 1u);
}
int tc_ext4(uint v) { return int((v << 4) | v); }
int tc_ext5(uint v) { return int((v << 3) | (v >> 2)); }
int tc_ext6(uint v) { return int((v << 2) | (v >> 4)); }
int tc_ext7(uint v) { return int((v << 1) | (v >> 6)); }
int tc_sext3(uint v) { return (v & 4u) != 0u ? int(v) - 8 : int(v); }
/* byte i (0 to 7) of the table in the words w0, w1 */
int tc_byte(uint w0, uint w1, uint i) { return int(((i < 4u ? w0 : w1) >> ((i & 3u) * 8u)) & 255u); }
uint tc_rgba(int r, int g, int b, uint a) {
    return uint(clamp(r, 0, 255)) | (uint(clamp(g, 0, 255)) << 8) | (uint(clamp(b, 0, 255)) << 16) | (a << 24);
}

/* the ETC1 modifiers a and b of each table codeword (indices 0 to 3 are a, b, -a, -b), and the T and H modes'
   distances, a byte each */
#define ETC1_A0 0x0D090502u
#define ETC1_A1 0x2F211812u
#define ETC1_B0 0x2A1D1108u
#define ETC1_B1 0xB76A503Cu
#define ETC2_D0 0x100B0603u
#define ETC2_D1 0x40292017u
/* the EAC modifiers of each table, a nibble each for indices 4 to 7; index k below 4 is -(that of k + 4) - 1. Two
   tables a word, the even one in the low half */
const uint EAC_P[8] = {0xC962E852u, 0xC531C741u, 0xA862B752u, 0xA742A763u,
                       0x97419751u, 0x96419731u, 0x92109632u, 0x86428753u};

/* pixel (x, y) of an ETC2 RGB block hi:lo as RGBA8. punch: punch-through alpha, whose differential bit is the opaque
   bit: a block without it has a transparent index 2, and its differential modifiers for index 0 are 0 */
uint etc2_rgb_pixel(uint hi, uint lo, bool punch, uint x, uint y) {
    uint i = x * 4u + y;
    bool diff = (hi & 2u) != 0u, flip = (hi & 1u) != 0u, see = punch && !diff;
    uint k = (((lo >> (16u + i)) & 1u) << 1) | ((lo >> i) & 1u);
    /* a see-through block's index 2 is transparent, except in the planar mode, which is always opaque */
    bool clear = see && k == 2u;
    uint s = flip ? uint(y >= 2u) : uint(x >= 2u);
    int r0, g0, b0;
    if (!punch && !diff) { /* individual: two 4 bit colors */
        r0 = tc_ext4((hi >> (28u - 4u * s)) & 15u);
        g0 = tc_ext4((hi >> (20u - 4u * s)) & 15u);
        b0 = tc_ext4((hi >> (12u - 4u * s)) & 15u);
    } else { /* differential, unless the second color overflows: then T, H or planar */
        uint r = (hi >> 27) & 31u, g = (hi >> 19) & 31u, b = (hi >> 11) & 31u;
        int dr = tc_sext3((hi >> 24) & 7u), dg = tc_sext3((hi >> 16) & 7u), db = tc_sext3((hi >> 8) & 7u);
        if (int(r) + dr < 0 || int(r) + dr > 31) { /* T */
            if (clear) return 0u;
            int d = tc_byte(ETC2_D0, ETC2_D1, (tc_bits(hi, lo, 35u, 34u) << 1) | (hi & 1u));
            if (k == 0u)
                return tc_rgba(tc_ext4((tc_bits(hi, lo, 60u, 59u) << 2) | tc_bits(hi, lo, 57u, 56u)),
                               tc_ext4(tc_bits(hi, lo, 55u, 52u)), tc_ext4(tc_bits(hi, lo, 51u, 48u)), 255u);
            int m = k == 1u ? d : k == 3u ? -d : 0;
            return tc_rgba(tc_ext4(tc_bits(hi, lo, 47u, 44u)) + m, tc_ext4(tc_bits(hi, lo, 43u, 40u)) + m,
                           tc_ext4(tc_bits(hi, lo, 39u, 36u)) + m, 255u);
        }
        if (int(g) + dg < 0 || int(g) + dg > 31) { /* H */
            if (clear) return 0u;
            uint r1 = tc_bits(hi, lo, 62u, 59u), g1 = (tc_bits(hi, lo, 58u, 56u) << 1) | tc_bits(hi, lo, 52u, 52u);
            uint b1 = (tc_bits(hi, lo, 51u, 51u) << 3) | tc_bits(hi, lo, 49u, 47u);
            uint r2 = tc_bits(hi, lo, 46u, 43u), g2 = tc_bits(hi, lo, 42u, 39u), b2 = tc_bits(hi, lo, 38u, 35u);
            /* the distance index's low bit is which base color is the larger */
            uint da = (tc_bits(hi, lo, 34u, 34u) << 2) | ((hi & 1u) << 1) |
                      uint(((r1 << 8) | (g1 << 4) | b1) >= ((r2 << 8) | (g2 << 4) | b2));
            int d = tc_byte(ETC2_D0, ETC2_D1, da);
            int m = (k & 1u) != 0u ? -d : d;
            if (k < 2u) return tc_rgba(tc_ext4(r1) + m, tc_ext4(g1) + m, tc_ext4(b1) + m, 255u);
            return tc_rgba(tc_ext4(r2) + m, tc_ext4(g2) + m, tc_ext4(b2) + m, 255u);
        }
        if (int(b) + db < 0 || int(b) + db > 31) { /* planar: three colors at the corners, interpolated; opaque */
            int ox = int(x), oy = int(y);
            int o0 = tc_ext6(tc_bits(hi, lo, 62u, 57u));
            int o1 = tc_ext7((tc_bits(hi, lo, 56u, 56u) << 6) | tc_bits(hi, lo, 54u, 49u));
            int o2 = tc_ext6((tc_bits(hi, lo, 48u, 48u) << 5) | (tc_bits(hi, lo, 44u, 43u) << 3) |
                             tc_bits(hi, lo, 41u, 39u));
            int h0 = tc_ext6((tc_bits(hi, lo, 38u, 34u) << 1) | (hi & 1u));
            int h1 = tc_ext7(tc_bits(hi, lo, 31u, 25u)), h2 = tc_ext6(tc_bits(hi, lo, 24u, 19u));
            int v0 = tc_ext6(tc_bits(hi, lo, 18u, 13u)), v1 = tc_ext7(tc_bits(hi, lo, 12u, 6u));
            int v2 = tc_ext6(tc_bits(hi, lo, 5u, 0u));
            return tc_rgba((ox * (h0 - o0) + oy * (v0 - o0) + 4 * o0 + 2) >> 2,
                           (ox * (h1 - o1) + oy * (v1 - o1) + 4 * o1 + 2) >> 2,
                           (ox * (h2 - o2) + oy * (v2 - o2) + 4 * o2 + 2) >> 2, 255u);
        }
        if (clear) return 0u;
        r0 = tc_ext5(uint(int(r) + (s != 0u ? dr : 0)));
        g0 = tc_ext5(uint(int(g) + (s != 0u ? dg : 0)));
        b0 = tc_ext5(uint(int(b) + (s != 0u ? db : 0)));
    }
    uint cw = s != 0u ? tc_bits(hi, lo, 36u, 34u) : tc_bits(hi, lo, 39u, 37u);
    int a = tc_byte(ETC1_A0, ETC1_A1, cw), b = tc_byte(ETC1_B0, ETC1_B1, cw);
    int m = k == 0u ? a : k == 1u ? b : k == 2u ? -a : -b;
    if (see && k == 0u) m = 0;
    return tc_rgba(r0 + m, g0 + m, b0 + m, 255u);
}

/* the modifier of pixel i of an EAC block hi:lo */
int eac_mod(uint hi, uint lo, uint i) {
    uint k = tc_bits(hi, lo, 47u - 3u * i, 45u - 3u * i), t = (hi >> 16) & 15u;
    int p = int((EAC_P[t >> 1] >> ((t & 1u) * 16u + (k & 3u) * 4u)) & 15u);
    return k < 4u ? -p - 1 : p;
}
/* pixel i's 8 bit alpha of an EAC block (ETC2_RGBA) */
uint eac_alpha(uint hi, uint lo, uint i) {
    return uint(clamp(int(hi >> 24) + eac_mod(hi, lo, i) * int((hi >> 20) & 15u), 0, 255));
}
/* pixel i of an 11 bit EAC block as 16 bit unorm or (sn) snorm */
uint eac11(uint hi, uint lo, bool sn, uint i) {
    int m = eac_mod(hi, lo, i), mult = int((hi >> 20) & 15u);
    int step = mult != 0 ? m * mult * 8 : m;
    if (!sn) {
        int v = clamp(int(hi >> 24) * 8 + 4 + step, 0, 2047);
        return (uint(v) << 5) | (uint(v) >> 6);
    }
    int base = int(hi >> 24) - ((hi >> 31) != 0u ? 256 : 0);
    if (base == -128) base = -127;
    int v = clamp(base * 8 + step, -1023, 1023);
    int a = v < 0 ? -v : v;
    int e = (a << 5) | (a >> 5);
    return uint(v < 0 ? -e : e) & 0xFFFFu;
}

/* the float nearest z / (2^24 - 1), the bits the CPU's (float)z / 16777215.0f gives. In binary the quotient is z's
   24 bits repeated after the point: from z's top bit p on, the significand is z << (23 - p), and the bits after it
   are the zeros above p in the next copy, then that copy's bit p, a 1, with more ones after it (z repeats): the
   remainder is over half an ulp and never a tie, so the nearest float is one ulp up */
uint tc_d24_float(uint z) {
    if (z == 0u) return 0u;
    uint p = uint(findMSB(z));
    uint m = (z << (23u - p)) + 1u;
    if (m == 0x1000000u) { /* rounded up to the next power of two */
        m = 0x800000u;
        p++;
    }
    return ((p + 103u) << 23) | (m & 0x7FFFFFu); /* exponent p - 24, biased by 127 */
}

/* present.c's expansion of a vi pixel's field of 4, 5 or 6 bits to 8 */
uint tc_expand(uint v, uint bits) {
    return bits == 4u ? v * 17u : bits == 5u ? (v << 3) | (v >> 2) : (v << 2) | (v >> 4);
}
uint tc_16to32(uint v) {
    if (mode == TC_VI_RGB565)
        return tc_expand((v >> 11) & 31u, 5u) | (tc_expand((v >> 5) & 63u, 6u) << 8) |
               (tc_expand(v & 31u, 5u) << 16) | 0xFF000000u;
    if (mode == TC_VI_RGBA5551)
        return tc_expand((v >> 11) & 31u, 5u) | (tc_expand((v >> 6) & 31u, 5u) << 8) |
               (tc_expand((v >> 1) & 31u, 5u) << 16) | ((v & 1u) != 0u ? 0xFF000000u : 0u);
    if (mode == TC_VI_RGBA4444)
        return tc_expand((v >> 12) & 15u, 4u) | (tc_expand((v >> 8) & 15u, 4u) << 8) |
               (tc_expand((v >> 4) & 15u, 4u) << 16) | (tc_expand(v & 15u, 4u) << 24);
    if (mode == TC_RGBA4)
        return (v & 15u) * 17u | (((v >> 4) & 15u) * 17u << 8) | (((v >> 8) & 15u) * 17u << 16) |
               ((v >> 12) * 17u << 24);
    if (mode == TC_RGB5A1)
        return (v & 31u) * 255u / 31u | (((v >> 5) & 31u) * 255u / 31u << 8) |
               (((v >> 10) & 31u) * 255u / 31u << 16) | ((v >> 15) * 255u << 24);
    return (v & 1u) * 255u | (((v >> 1) & 31u) * 255u / 31u << 8) | (((v >> 6) & 31u) * 255u / 31u << 16) |
           (((v >> 11) & 31u) * 255u / 31u << 24);
}

/* byte b of the input buffer (byte addressed) */
uint tc_src_byte(uint b) { return (src[b >> 2] >> (8u * (b & 3u))) & 0xFFu; }
/* TC_DMA_REMAP: byte ob of a line out, with the line in from byte in_line on and the byte now there in old */
uint tc_remap_byte(uint ob, uint in_line, uint old) {
    uint cs = remap & 15u, ns = (remap >> 4) & 15u, nd = (remap >> 8) & 15u, eo = cs * nd;
    uint e = ob / eo, within = ob - e * eo, k = within / cs, j = within - k * cs, sel = (remap >> (12u + 4u * k)) & 7u;
    if (sel < 4u) return sel < ns ? tc_src_byte(in_line + e * cs * ns + sel * cs + j) : 0u;
    if (sel < 6u) return ((sel == 4u ? const_a : const_b) >> (8u * j)) & 0xFFu;
    return old;
}

void main() {
    uint u = gl_GlobalInvocationID.x;
    if (u >= units) return;
    if (mode == TC_BLIT) {
        uint line = gl_GlobalInvocationID.y, at = out_bias + line * out_pitch + u, w = dst[at], r = 0u;
        uint row = in_bias + src[const_b + line] * in_pitch;
        for (uint b = 0u; b < 4u; b++) {
            uint ob = 4u * u + b, v = (w >> (8u * b)) & 0xFFu;
            if (ob < line_out) { /* a branch, not a select: the tables end at the row's last pixel */
                uint i = ob / remap;
                v = tc_src_byte(row + src[const_a + i] * remap + (ob - i * remap));
            }
            r |= v << (8u * b);
        }
        dst[at] = r;
        return;
    }
    if (mode == TC_DMA_REMAP) {
        uint line = gl_GlobalInvocationID.y, at = out_bias + line * out_pitch + u, w = dst[at], r = 0u;
        for (uint b = 0u; b < 4u; b++) {
            uint ob = 4u * u + b, v = (w >> (8u * b)) & 0xFFu;
            if (ob < line_out) v = tc_remap_byte(ob, in_bias + line * in_pitch, v); /* not past the line's input */
            r |= v << (8u * b);
        }
        dst[at] = r;
        return;
    }
    /* the row of units (texels, or blocks for ETC2) of all the dispatch's slices */
    uint row = gl_GlobalInvocationID.z * rows + gl_GlobalInvocationID.y;
    uint in_at = in_bias + row * in_pitch, out_at = out_bias + row * out_pitch;
    if (mode == TC_NV12_UV) {
        uint u4 = src[in_at + u], v4 = src[st_bias + row * st_pitch + u];
        dst[out_at + 2u * u] = (u4 & 0xFFu) | ((v4 & 0xFFu) << 8) | ((u4 & 0xFF00u) << 8) | ((v4 & 0xFF00u) << 16);
        dst[out_at + 2u * u + 1u] =
            ((u4 >> 16) & 0xFFu) | ((v4 >> 8) & 0xFF00u) | ((u4 >> 8) & 0xFF0000u) | (v4 & 0xFF000000u);
    } else if (mode >= TC_VI_RGBX8888) {
        uint p = src[in_at + u];
        dst[out_at + u] = mode == TC_VI_RGBX8888 ? p | 0xFF000000u
                                                 : (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
    } else if (mode <= TC_A5B5G5R1 || mode >= TC_VI_RGB565) {
        uint w = src[in_at + u];
        dst[out_at + 2u * u] = tc_16to32(w & 0xFFFFu);
        dst[out_at + 2u * u + 1u] = tc_16to32(w >> 16);
    } else if (mode == TC_RGB32F) {
        dst[out_at + 4u * u] = src[in_at + 3u * u];
        dst[out_at + 4u * u + 1u] = src[in_at + 3u * u + 1u];
        dst[out_at + 4u * u + 2u] = src[in_at + 3u * u + 2u];
        dst[out_at + 4u * u + 3u] = 0x3F800000u; /* 1.0 */
    } else if (mode <= TC_D32S8) {
        uint st = 0u;
        for (uint i = 0u; i < 4u; i++) {
            uint d, s;
            if (mode == TC_D32S8) {
                d = src[in_at + 8u * u + 2u * i];
                s = src[in_at + 8u * u + 2u * i + 1u] & 255u;
            } else {
                uint v = src[in_at + 4u * u + i];
                d = mode == TC_S8D24 ? v >> 8 : v & 0xFFFFFFu;
                s = mode == TC_D24X8 ? 0u : mode == TC_S8D24 ? v & 255u : v >> 24;
                if (zfloat != 0u) d = tc_d24_float(d);
            }
            dst[out_at + 4u * u + i] = d;
            st |= s << (8u * i);
        }
        dst[st_bias + row * st_pitch + u] = st;
    } else {
        /* a block: its 8 byte halves big endian, the first (alpha, or red) in hi:lo, the second in hi2:lo2 */
        bool two = mode == TC_ETC2_RGBA || mode >= TC_EAC_RG, sn = mode == TC_EAC_R_SNORM || mode == TC_EAC_RG_SNORM;
        uint at = in_at + u * (two ? 4u : 2u);
        uint hi = tc_bswap(src[at]), lo = tc_bswap(src[at + 1u]), hi2 = 0u, lo2 = 0u;
        if (two) {
            hi2 = tc_bswap(src[at + 2u]);
            lo2 = tc_bswap(src[at + 3u]);
        }
        /* four rows of pixels a row of blocks */
        uint out_row = out_bias + row * 4u * out_pitch;
        for (uint y = 0u; y < 4u; y++) {
            uint o = out_row + y * out_pitch;
            if (mode == TC_EAC_R || mode == TC_EAC_R_SNORM) {
                dst[o + 2u * u] = eac11(hi, lo, sn, y) | (eac11(hi, lo, sn, 4u + y) << 16);
                dst[o + 2u * u + 1u] = eac11(hi, lo, sn, 8u + y) | (eac11(hi, lo, sn, 12u + y) << 16);
                continue;
            }
            for (uint x = 0u; x < 4u; x++) {
                uint i = x * 4u + y, c;
                if (mode == TC_ETC2_RGBA)
                    c = (etc2_rgb_pixel(hi2, lo2, false, x, y) & 0xFFFFFFu) | (eac_alpha(hi, lo, i) << 24);
                else if (mode <= TC_ETC2_PTA)
                    c = etc2_rgb_pixel(hi, lo, mode == TC_ETC2_PTA, x, y);
                else
                    c = eac11(hi, lo, sn, i) | (eac11(hi2, lo2, sn, i) << 16);
                dst[o + 4u * u + x] = c;
            }
        }
    }
}
