/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Hashes from Eden's HLE macro table (src/video_core/macro.cpp at de7e6e01, and at 989768aa for the constant buffer
   and memory clears, the raster bounding box, the multi-draw with a count and the byte count draw):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project (GPL-3.0-or-later)
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project (GPL-2.0-or-later) */
/* Native implementations of the macros NVN uploads, chosen by the hash of their code (mme.c's mme_choose). Each one
   sends the methods the macro's code sends, in the same order with the same values, through mme_out, as the
   interpreter does: what it reads (its parameters and the shadow scratch registers, 0xD00 on) it reads before it
   sends anything, as the code does. They were derived from the code, and mme_verify compares them with the
   interpreter call by call.
   The parameters are the macro's: a fetch past the last one reads 0, as in the interpreter. An implementation leaves a
   call to the interpreter (returns 0, before sending) where the interpreter would do something it doesn't: a loop
   long enough to reach the interpreter's 5,000,000 instruction limit, or a method computed from a parameter that
   could make the stream wait (0x6C3) or rewrite the macros (0x45-0x48) in the middle of the macro.
   Eden's names are given where its table has the hash. Its implementations call its rasterizer directly; these send
   methods, so the 3D engine (maxwell3d_method) does what it does for the interpreted macro */
#include "nx/gpu/maxwell/engines_int.h"

static u32 arg(const u32* p, u32 np, u32 i) { return i < np ? p[i] : 0; }

/* the instances of one draw: BEGIN (0x586) with the topology word, the count, END (0x585). From the second instance
   on, BEGIN's instance bits (26-27) say "next instance" (1) */
static void draw_instances(GpuChannel* ch, MmeRec* rec, u32 begin, u32 count_method, u32 count, u32 n) {
    u32 i;
    for (i = 0; i < n; i++) {
        mme_out(ch, rec, 0x586, begin);
        mme_out(ch, rec, count_method, count);
        mme_out(ch, rec, 0x585, 0);
        begin = (begin & ~(3u << 26)) | (1u << 26);
    }
}

/* Indexed instanced draw (Eden: HLE_DrawIndexedIndirect, false; one game's busiest macro). Parameters: topology, index
   count, instance count (masked with scratch 0xD1B), first index, base vertex, base instance. The base vertex goes to
   0x446 and 0x50D and the base instance to 0x50E for the draw, and they are zeroed after it */
static int draw_indexed(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 n = arg(p, np, 2) & mme_reg(ch, rec, 0xD1B);
    if (n > MME_LOOP_MAX) return 0;
    mme_out(ch, rec, 0x5F7, arg(p, np, 3));
    mme_out(ch, rec, 0x446, arg(p, np, 4));
    mme_out(ch, rec, 0x50D, arg(p, np, 4));
    mme_out(ch, rec, 0x50E, arg(p, np, 5));
    draw_instances(ch, rec, arg(p, np, 0), 0x5F8, arg(p, np, 1), n);
    mme_out(ch, rec, 0x446, 0);
    mme_out(ch, rec, 0x50D, 0);
    mme_out(ch, rec, 0x50E, 0);
    return 1;
}
/* The same draw, which also writes the base vertex and base instance into the bound constant buffer at offset 0x640
   for the shaders, and zeroes them after (Eden: HLE_DrawIndexedIndirect, true) */
static int draw_indexed_cb(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 n = arg(p, np, 2) & mme_reg(ch, rec, 0xD1B);
    if (n > MME_LOOP_MAX) return 0;
    mme_out(ch, rec, 0x5F7, arg(p, np, 3));
    mme_out(ch, rec, 0x446, arg(p, np, 4));
    mme_out(ch, rec, 0x50D, arg(p, np, 4));
    mme_out(ch, rec, 0x50E, arg(p, np, 5));
    mme_out(ch, rec, 0x8E3, 0x640);
    mme_out(ch, rec, 0x8E4, arg(p, np, 4));
    mme_out(ch, rec, 0x8E5, arg(p, np, 5));
    draw_instances(ch, rec, arg(p, np, 0), 0x5F8, arg(p, np, 1), n);
    mme_out(ch, rec, 0x446, 0);
    mme_out(ch, rec, 0x50D, 0);
    mme_out(ch, rec, 0x50E, 0);
    mme_out(ch, rec, 0x8E3, 0x640);
    mme_out(ch, rec, 0x8E4, 0);
    mme_out(ch, rec, 0x8E5, 0);
    return 1;
}
/* Non-indexed instanced draw (Eden: HLE_DrawArraysIndirect, false). Parameters: topology, vertex count, instance
   count (masked with 0xD1B), first vertex (0x35D), base instance (0x50E, zeroed after) */
static int draw_arrays(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 n = arg(p, np, 2) & mme_reg(ch, rec, 0xD1B);
    if (n > MME_LOOP_MAX) return 0;
    mme_out(ch, rec, 0x35D, arg(p, np, 3));
    mme_out(ch, rec, 0x50E, arg(p, np, 4));
    draw_instances(ch, rec, arg(p, np, 0), 0x35E, arg(p, np, 1), n);
    mme_out(ch, rec, 0x50E, 0);
    return 1;
}
/* The same draw, which also writes the base instance into the bound constant buffer at offset 0x644 (Eden:
   HLE_DrawArraysIndirect, true) */
static int draw_arrays_cb(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 n = arg(p, np, 2) & mme_reg(ch, rec, 0xD1B);
    if (n > MME_LOOP_MAX) return 0;
    mme_out(ch, rec, 0x35D, arg(p, np, 3));
    mme_out(ch, rec, 0x50E, arg(p, np, 4));
    mme_out(ch, rec, 0x8E3, 0x644);
    mme_out(ch, rec, 0x8E4, arg(p, np, 4));
    draw_instances(ch, rec, arg(p, np, 0), 0x35E, arg(p, np, 1), n);
    mme_out(ch, rec, 0x50E, 0);
    mme_out(ch, rec, 0x8E3, 0x644);
    mme_out(ch, rec, 0x8E4, 0);
    return 1;
}
/* Indexed draws from a list of draw records with a count (Eden: HLE_MultiDrawIndexedIndirectCount; NVN's
   MultiDrawElementsIndirectCount). Parameters: the first draw p0, the most draws p1, the topology p2, padding words
   after each record p3, the count p4, then the records from p5 on: index count, instance count, first index, base
   vertex, base instance, and p3 words of padding between them. Scratch 0xD08 takes the topology, 0xD0A the count
   (then p1 when that is smaller, by the sign of their difference) and 0xD09 the topology with BEGIN's "next instance"
   bits. When scratch 0xD1B is zero, or p0 isn't below the end (by the sign of p0 minus the end), nothing is drawn.
   Each draw from p0 to the end sends its first index (0x5F7), base vertex (0x446, 0x50D) and base instance (0x50E),
   writes them and the draw's index into the bound constant buffer at 0x640 (0x8E3-0x8E6), and draws its instances
   (BEGIN, the count to 0x5F8, END). The macro ends zeroing those registers and constants.
   The scratch registers are kept here as they are written, which the code reads back. Past the draws the code goes on
   fetching the records up to the most draws, which only takes instructions: a call whose loops could reach the
   interpreter's limit (more than MME_STEPS_MAX by the bound counted here, short of its 5,000,000) is the
   interpreter's */
#define MME_STEPS_MAX 4000000u
static int multi_draw_indexed_count(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 first = arg(p, np, 0), most = arg(p, np, 1), topo = arg(p, np, 2), pad = arg(p, np, 3), end = arg(p, np, 4);
    u32 next = (topo & ~(3u << 26)) | (1u << 26), left, i, k;
    int on = mme_reg(ch, rec, 0xD1B) != 0, smaller = on && ((most - end) & 0x80000000u), draws;
    u64 steps = 64, at;
    if (smaller) end = most;
    draws = on && ((first - end) & 0x80000000u);
    if (draws) {
        /* the instructions the draws take, at most: 40 a draw, 8 an instance, 4 a padding word */
        for (i = first, at = 5; i != end; i++, at += 5 + (u64)pad) {
            u32 n = at + 1 < np ? p[at + 1] : 0;
            steps += 40 + 8 * (u64)n + 4 * (u64)pad;
            if (steps > MME_STEPS_MAX) return 0;
        }
        left = most - end;
    } else
        left = most - first;
    /* then the records left: at most 16 instructions each, 4 a padding word */
    if (left && ((u64)left > MME_STEPS_MAX / 16 || pad > MME_STEPS_MAX / 4)) return 0;
    if (left && steps + (u64)left * (16 + 4 * (u64)pad) > MME_STEPS_MAX) return 0;
    mme_out(ch, rec, 0xD08, topo);
    mme_out(ch, rec, 0xD0A, arg(p, np, 4));
    if (smaller) mme_out(ch, rec, 0xD0A, end);
    if (draws) {
        mme_out(ch, rec, 0xD09, next);
        for (i = first, at = 5; i != end; i++, at += 5 + (u64)pad) {
            u32 count = at < np ? p[at] : 0, n = at + 1 < np ? p[at + 1] : 0, base_vertex = at + 3 < np ? p[at + 3] : 0;
            u32 base_instance = at + 4 < np ? p[at + 4] : 0;
            mme_out(ch, rec, 0x5F7, at + 2 < np ? p[at + 2] : 0);
            mme_out(ch, rec, 0x446, base_vertex);
            mme_out(ch, rec, 0x50D, base_vertex);
            mme_out(ch, rec, 0x50E, base_instance);
            mme_out(ch, rec, 0x8E3, 0x640);
            mme_out(ch, rec, 0x8E4, base_vertex);
            mme_out(ch, rec, 0x8E5, base_instance);
            mme_out(ch, rec, 0x8E6, i);
            for (k = 0; k < n; k++) {
                mme_out(ch, rec, 0x586, k ? next : topo);
                mme_out(ch, rec, 0x5F8, count);
                mme_out(ch, rec, 0x585, 0);
            }
        }
    }
    mme_out(ch, rec, 0x446, 0);
    mme_out(ch, rec, 0x50D, 0);
    mme_out(ch, rec, 0x50E, 0);
    mme_out(ch, rec, 0x8E3, 0x640);
    mme_out(ch, rec, 0x8E4, 0);
    mme_out(ch, rec, 0x8E5, 0);
    mme_out(ch, rec, 0x8E6, 0);
    return 1;
}
/* A draw of what transform feedback wrote (Eden: HLE_DrawIndirectByteCount; NVN's DrawTransformFeedback): BEGIN
   (0x586) with p0, the stride (0x4C6) and the byte count (0x48F), END */
static int draw_byte_count(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    mme_out(ch, rec, 0x586, arg(p, np, 0));
    mme_out(ch, rec, 0x4C6, arg(p, np, 1));
    mme_out(ch, rec, 0x48F, arg(p, np, 2));
    mme_out(ch, rec, 0x585, 0);
    return 1;
}
/* Selects constant buffer i for uploads (Eden: HLE_D7333D26E0A93EDE): its size from scratch 0xD2F + i and its
   address (in 256-byte units) from 0xD2A + i, into 0x8E0-0x8E2. Parameter: i */
static int cb_select(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 i = arg(p, np, 0), addr = mme_reg(ch, rec, i + 0xD2A), size = mme_reg(ch, rec, i + 0xD2F);
    mme_out(ch, rec, 0x8E0, size);
    mme_out(ch, rec, 0x8E1, addr >> 24);
    mme_out(ch, rec, 0x8E2, addr << 8);
    return 1;
}
/* Selects the constant buffer at scratch 0xD18, of the given size, for uploads at word offset p0 */
static void cb_select_at(GpuChannel* ch, MmeRec* rec, u32 size, const u32* p, u32 np) {
    u32 addr = mme_reg(ch, rec, 0xD18);
    mme_out(ch, rec, 0x8E0, size);
    mme_out(ch, rec, 0x8E1, addr >> 24);
    mme_out(ch, rec, 0x8E2, addr << 8);
    mme_out(ch, rec, 0x8E3, arg(p, np, 0) << 2);
}
/* 0x7000 bytes (Eden: HLE_C713C83D8F63CCF3) */
static int cb_select_offset(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    cb_select_at(ch, rec, 0x7000, p, np);
    return 1;
}
/* 0x5F00 bytes (one game's; not in Eden's table) */
static int cb_select_offset_5f00(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    cb_select_at(ch, rec, 0x5F00, p, np);
    return 1;
}
/* Zeroes the start of a constant buffer (Eden: HLE_ClearConstBuffer): the buffer of the given size at p0 (high) and
   p1 (low) is selected at offset 0 (0x8E0-0x8E3), then p2 groups of four zero words go to 0x8E4. A loop of about six
   instructions a group: past MME_LOOP_MAX groups the call is the interpreter's */
static int cb_clear(GpuChannel* ch, MmeRec* rec, u32 size, const u32* p, u32 np) {
    u32 n = arg(p, np, 2), i;
    if (n > MME_LOOP_MAX) return 0;
    mme_out(ch, rec, 0x8E0, size);
    mme_out(ch, rec, 0x8E1, arg(p, np, 0));
    mme_out(ch, rec, 0x8E2, arg(p, np, 1));
    mme_out(ch, rec, 0x8E3, 0);
    for (i = 0; i < 4 * n; i++) mme_out(ch, rec, 0x8E4, 0);
    return 1;
}
static int cb_clear_5f00(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) { return cb_clear(ch, rec, 0x5F00, p, np); }
/* Binds a shader program (Eden: HLE_BindShader). Parameters: program slot i, program id, code offset, bind group,
   constant buffer address. When the id equals the one scratch 0xD1C + i keeps for the slot, nothing is sent.
   Otherwise the offset goes to the slot's program (0x801 + 16 * i), the id and the offset to scratch 0xD1C + i and
   0xD22 + i, a 0x10000-byte constant buffer at the address is selected (0x8E0-0x8E2), and bound as bank 1 of the bind
   group (0x904 + 8 * group, its low 7 bits). A slot above 15 is the interpreter's: its scratch methods would leave
   0xD1C-0xD31 and could reach the methods that wait or upload macros */
static int bind_program(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 i = arg(p, np, 0), cb;
    if (i > 15) return 0;
    if (arg(p, np, 1) == mme_reg(ch, rec, i + 0xD1C)) return 1;
    mme_out(ch, rec, 0x801 | i << 4, arg(p, np, 2));
    mme_out(ch, rec, 0xD1C + i, arg(p, np, 1));
    mme_out(ch, rec, 0xD22 + i, arg(p, np, 2));
    cb = arg(p, np, 4);
    mme_out(ch, rec, 0x8E0, 0x10000);
    mme_out(ch, rec, 0x8E1, cb >> 24);
    mme_out(ch, rec, 0x8E2, cb << 8);
    mme_out(ch, rec, 0x904 + ((arg(p, np, 3) & 0x7F) << 3), 0x11);
    return 1;
}
/* Writes p0 and bits 8-16 of p1 to scratch 0xD38-0xD39, then p0 to 0x0D8 and the low byte of p1 to 0x0D9. When bit 8
   of scratch 0xD3A is set, p0's top byte becomes the low byte of the smaller of p1's bits 8-16 and 0xD3A (by the sign
   of their difference). Not in Eden's table: the hash is of the code up to its last exit's delay slot */
static int scratch_d38(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 cfg = mme_reg(ch, rec, 0xD3A), a = arg(p, np, 1), hi = (a >> 8) & 0x1FF, v = arg(p, np, 0);
    mme_out(ch, rec, 0xD38, v);
    mme_out(ch, rec, 0xD39, hi);
    if ((cfg >> 8) & 1) v = (v & 0x00FFFFFFu) | ((((hi - cfg) & 0x80000000u) ? hi : cfg) & 0xFF) << 24;
    mme_out(ch, rec, 0x0D8, v);
    mme_out(ch, rec, 0x0D9, a & 0xFF);
    return 1;
}
/* Sends 0 to 0x671, then bits 8-15 of p0 one by one to the eight render targets' blend enables (0x4D8-0x4DF). Not in
   Eden's table */
static int blend_enables(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 v = arg(p, np, 0), i;
    mme_out(ch, rec, 0x671, 0);
    for (i = 0; i < 8; i++) mme_out(ch, rec, 0x4D8 + i, (v >> (8 + i)) & 1);
    return 1;
}
/* Writes the eight render targets' color masks (0x680-0x687) from four parameters, two 13-bit halves each (bits 0-12
   and 16-28). Not in Eden's table */
static int color_masks(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 i;
    for (i = 0; i < 4; i++) {
        mme_out(ch, rec, 0x680 + 2 * i, arg(p, np, i) & 0x1FFF);
        mme_out(ch, rec, 0x681 + 2 * i, (arg(p, np, i) >> 16) & 0x1FFF);
    }
    return 1;
}

/* A word kept in a scratch register: when p0 differs from it, method m gets value v and the scratch takes p0 */
static void scratch_word(GpuChannel* ch, MmeRec* rec, u32 scratch, u32 m, u32 v, const u32* p, u32 np) {
    if (arg(p, np, 0) == mme_reg(ch, rec, scratch)) return;
    mme_out(ch, rec, m, v);
    mme_out(ch, rec, scratch, arg(p, np, 0));
}
/* scratch 0xD29, 0x4B2 getting 25. Not in Eden's table */
static int scratch_d29(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    scratch_word(ch, rec, 0xD29, 0x4B2, 25, p, np);
    return 1;
}
/* Indexed draw with its index buffer given in the call (one game's most called interpreted one).
   Parameters: topology, index buffer address high and low, index format, count. BEGIN, the buffer's address and
   format, first index 0 and the count, END */
static int draw_indexed_inline(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    mme_out(ch, rec, 0x586, arg(p, np, 0));
    mme_out(ch, rec, 0x5F2, arg(p, np, 1));
    mme_out(ch, rec, 0x5F3, arg(p, np, 2));
    mme_out(ch, rec, 0x5F6, arg(p, np, 3));
    mme_out(ch, rec, 0x5F7, 0);
    mme_out(ch, rec, 0x5F8, arg(p, np, 4));
    mme_out(ch, rec, 0x585, 0);
    return 1;
}
/* The three words kept in scratch 0xD3B-0xD3D: when the parameters differ from them, 0x4B2 gets 25 and the scratch
   takes the parameters (only the last two when the first is the same) */
static int scratch_d3b(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 a = mme_reg(ch, rec, 0xD3B), b = mme_reg(ch, rec, 0xD3C), c = mme_reg(ch, rec, 0xD3D);
    if (arg(p, np, 0) != a) {
        mme_out(ch, rec, 0x4B2, 25);
        mme_out(ch, rec, 0xD3B, arg(p, np, 0));
    } else if (!((b ^ arg(p, np, 1)) | (c ^ arg(p, np, 2))))
        return 1;
    else
        mme_out(ch, rec, 0x4B2, 25);
    mme_out(ch, rec, 0xD3C, arg(p, np, 1));
    mme_out(ch, rec, 0xD3D, arg(p, np, 2));
    return 1;
}
/* When scratch 0xD28 is 1: 0x0BF and 0x0DB set, 0x54C 2, 0x0DB cleared. No parameters */
static int scratch_d28(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    (void)p, (void)np;
    if (mme_reg(ch, rec, 0xD28) != 1) return 1;
    mme_out(ch, rec, 0x0BF, 1);
    mme_out(ch, rec, 0x0DB, 1);
    mme_out(ch, rec, 0x54C, 2);
    mme_out(ch, rec, 0x0DB, 0);
    return 1;
}
/* Clears every layer of a render target (Eden: HLE_MultiLayerClear). Parameter: the CLEAR_SURFACE word, whose bits
   6-9 name the target: as many clears as its layer count (register 0x206 of its eight), the layer in bits 10 on */
static void clear_layers(GpuChannel* ch, MmeRec* rec, u32 w, u32 n) {
    u32 i;
    for (i = 0; i < n; i++) mme_out(ch, rec, 0x674, w + 0x400 * i);
}
static int multi_layer_clear(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 w = arg(p, np, 0), n = mme_reg(ch, rec, 0x206 + (((w >> 6) & 0xF) << 4));
    if (n > MME_LOOP_MAX) return 0;
    clear_layers(ch, rec, w, n);
    return 1;
}
/* The same with the depth target's layer count (register 0x48C) */
static int multi_layer_clear_depth(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 n = mme_reg(ch, rec, 0x48C);
    if (n > MME_LOOP_MAX) return 0;
    clear_layers(ch, rec, arg(p, np, 0), n);
    return 1;
}

/* The raster bounding box (0x0BB; Eden: HLE_SetRasterBoundingBox): bits 4-11 are scratch 0xD34 ANDed with the
   conservative raster enable (0x452). The code clears the register holding its parameter before it inserts them, so
   the other bits are 0 (Eden's keeps the parameter's) */
static int raster_bounding_box(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 v = mme_reg(ch, rec, 0x452) & mme_reg(ch, rec, 0xD34);
    (void)p, (void)np;
    mme_out(ch, rec, 0x0BB, (v & 0xFF) << 4);
    return 1;
}
/* Enables stencil (0x4E0) and sets both faces' operations from p0's nibbles: the front's fail, depth fail, pass and
   function (0x4E1-0x4E4) from nibbles 1, 3, 2 and 0, the back's (0x566-0x569) from 5, 7, 6 and 4 */
static int stencil_ops(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    static const u8 nibble[8] = {1, 3, 2, 0, 5, 7, 6, 4};
    u32 v = arg(p, np, 0), i;
    mme_out(ch, rec, 0x4E0, 1);
    for (i = 0; i < 8; i++) mme_out(ch, rec, (i < 4 ? 0x4E1 : 0x566 - 4) + i, (v >> (4 * nibble[i])) & 0xF);
    return 1;
}
/* A draw of one instance: BEGIN (0x586) with the topology word p0, first vertex p1 (0x35D), count p2 (0x35E), END */
static int draw_once(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    mme_out(ch, rec, 0x586, arg(p, np, 0));
    mme_out(ch, rec, 0x35D, arg(p, np, 1));
    mme_out(ch, rec, 0x35E, arg(p, np, 2));
    mme_out(ch, rec, 0x585, 0);
    return 1;
}
/* 0x376 set to p0, and p0 kept in scratch 0xD36, when it differs from what 0xD36 holds */
static int scratch_d36(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    if (arg(p, np, 0) == mme_reg(ch, rec, 0xD36)) return 1;
    mme_out(ch, rec, 0x376, arg(p, np, 0));
    mme_out(ch, rec, 0xD36, arg(p, np, 0));
    return 1;
}
/* Zeroes the start of the constant buffer at p0:p1 (Eden: HLE_ClearConstBuffer, 0x7000): selected with size 0x7000
   and offset 0 (0x8E0-0x8E3), then 4 * p2 zero words uploaded (0x8E4) */
static int clear_const_buffer(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 n = arg(p, np, 2), i;
    if (n > MME_LOOP_MAX) return 0;
    mme_out(ch, rec, 0x8E0, 0x7000);
    mme_out(ch, rec, 0x8E1, arg(p, np, 0));
    mme_out(ch, rec, 0x8E2, arg(p, np, 1));
    mme_out(ch, rec, 0x8E3, 0);
    for (i = 0; i < 4 * n; i++) mme_out(ch, rec, 0x8E4, 0);
    return 1;
}
/* Zeroes p2 bytes at p0:p1 with an inline upload (Eden: HLE_ClearMemory): line length p2, one line, the address,
   launched (0x06C = 0x1011), then p2 / 4 zero words of data (0x06D). The code sends 16 bytes a turn until its count
   reaches 0 exactly, so a p2 that isn't a multiple of 16 loops until the interpreter's limit: left to it */
static int clear_memory(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 n = arg(p, np, 2), i;
    if ((n & 15) || n / 16 > MME_LOOP_MAX) return 0;
    mme_out(ch, rec, 0x060, n);
    mme_out(ch, rec, 0x061, 1);
    mme_out(ch, rec, 0x062, arg(p, np, 0));
    mme_out(ch, rec, 0x063, arg(p, np, 1));
    mme_out(ch, rec, 0x06C, 0x1011);
    for (i = 0; i < n / 4; i++) mme_out(ch, rec, 0x06D, 0);
    return 1;
}
/* Starts transform feedback (Eden: HLE_TransformFeedbackSetup): enabled (0x1D1), the four buffers' offsets zeroed
   (0x0E4 + 8 * i), and the first buffer's stride (0x1C2) uploaded inline to p0:p1 as one word */
static int transform_feedback_setup(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 stride = mme_reg(ch, rec, 0x1C2), i;
    mme_out(ch, rec, 0x1D1, 1);
    for (i = 0; i < 4; i++) mme_out(ch, rec, 0x0E4 + 8 * i, 0);
    mme_out(ch, rec, 0x060, 4);
    mme_out(ch, rec, 0x061, 1);
    mme_out(ch, rec, 0x062, arg(p, np, 0));
    mme_out(ch, rec, 0x063, arg(p, np, 1));
    mme_out(ch, rec, 0x06C, 0x1011);
    mme_out(ch, rec, 0x06D, stride);
    return 1;
}
/* Firmware call 4 (0x8C4) with p0, after a wait for idle (0x044) and its arguments p1 and p2 in scratch 0xD01-0xD02,
   with 0xD00 cleared. When the low byte of scratch 0xD1A is 2, ten no-operations (0x040) follow; otherwise the code
   sends no-operations until 0xD00 reads 1, which the call itself writes here (engines.c), so it sends none. Nothing it
   sends changes 0xD1A, so it is read first */
static int firmware_call4(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    u32 delay = (mme_reg(ch, rec, 0xD1A) & 0xFF) == 2, i;
    mme_out(ch, rec, 0x044, 0);
    mme_out(ch, rec, 0xD00, 0);
    mme_out(ch, rec, 0xD01, arg(p, np, 1));
    mme_out(ch, rec, 0xD02, arg(p, np, 2));
    mme_out(ch, rec, 0x8C4, arg(p, np, 0));
    if (delay)
        for (i = 0; i < 10; i++) mme_out(ch, rec, 0x040, 0);
    else
        for (i = 0; i < MME_LOOP_MAX && mme_reg(ch, rec, 0xD00) != 1; i++) mme_out(ch, rec, 0x040, 0);
    return 1;
}
/* Render target 0 from the parameters (address high and low, width, height, then the format in p4's top byte and the
   memory layout in its low 24 bits, one layer), the window offset from the width and height (0x3FD-0x3FE), and the
   rest of the state reset to fixed values, in the code's order */
static int render_target_reset(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    static const u32 fixed[][2] = {
        {0x54D, 0},          {0x574, 0},      {0x1D5, 1},          {0x380, 0},          {0x300, 0xFFFF0000},
        {0x301, 0xFFFF0000}, {0x302, 0},      {0x303, 0x3F800000}, {0x64B, 0},          {0x64F, 0xC1C},
        {0x651, 1},          {0x0DF, 1},      {0x35A, 0},          {0x4B3, 0},          {0x66F, 0},
        {0x4E0, 0},          {0x4BB, 0},      {0x4D8, 0},          {0x671, 0},          {0x680, 0x1111},
        {0x084, 0},          {0x54F, 0},      {0x3EF, 0xFFFF},     {0x3F0, 0xFFFF},     {0x3F1, 0xFFFF},
        {0x3F2, 0xFFFF},     {0x36B, 0x1B02}, {0x36C, 0x1B02},     {0x372, 0},          {0x646, 0},
        {0x820, 0x20},       {0x830, 0x30},   {0x840, 0x40},       {0x1D1, 0},          {0x3D0, 0},
        {0x489, 0},          {0x452, 0},      {0x3ED, 0},          {0x374, 0},          {0x286, 0x6420},
        {0x47E, 0},          {0x3C7, 0},      {0x478, 0x88888888}, {0x479, 0x88888888}, {0x47A, 0x88888888},
        {0x47B, 0x88888888}};
    u32 i;
    mme_out(ch, rec, 0x487, 0x0FAC6881);
    mme_out(ch, rec, 0x54E, 0);
    for (i = 0; i < 4; i++) mme_out(ch, rec, 0x200 + i, arg(p, np, i));
    mme_out(ch, rec, 0x204, arg(p, np, 4) >> 24);
    mme_out(ch, rec, 0x205, arg(p, np, 4) & 0xFFFFFF);
    mme_out(ch, rec, 0x206, 1);
    mme_out(ch, rec, 0x207, 0);
    mme_out(ch, rec, 0x3FD, arg(p, np, 2) << 16);
    mme_out(ch, rec, 0x3FE, arg(p, np, 3) << 16);
    for (i = 0; i < sizeof fixed / sizeof fixed[0]; i++) mme_out(ch, rec, fixed[i][0], fixed[i][1]);
    return 1;
}

/* The native implementations by the hash of their code: X(hash, function, name). mme_native_find is a switch on the
   hash, which the compiler makes a lookup rather than a scan, and a hash listed twice is a duplicate case */
#define MME_NATIVES(X) \
    X(0x771BB18C62444DA0ull, draw_indexed, "indexed draw") \
    X(0x0217920100488FF7ull, draw_indexed_cb, "indexed draw with constants") \
    X(0x0D61FC9FAAC9FCADull, draw_arrays, "draw") \
    X(0x8A4D173EB99A8603ull, draw_arrays_cb, "draw with constants") \
    X(0xD7333D26E0A93EDEull, cb_select, "constant buffer select") \
    X(0xC713C83D8F63CCF3ull, cb_select_offset, "constant buffer select at offset") \
    X(0xEB29B2A09AA06D38ull, bind_program, "program bind") \
    X(0x42B97BFB33086347ull, scratch_d38, "scratch 0xD38 and 0x0D8") \
    X(0x176ED6F0009E577Aull, blend_enables, "blend enables") \
    X(0xE8FCEAF999995860ull, color_masks, "color masks") \
    X(0x2DB33AADB741839Cull, draw_indexed_inline, "indexed draw, buffer inline") \
    X(0x5DA7C45099120734ull, scratch_d3b, "scratch 0xD3B-0xD3D") \
    X(0x88EEF6578DA4695Dull, scratch_d28, "scratch 0xD28") \
    X(0xEAD26C3E2109B06Bull, multi_layer_clear, "clear of every layer") \
    X(0x71F722B9118625E7ull, multi_layer_clear_depth, "clear of every layer, the depth target's count") \
    X(0xDB1341DBEB4C8AF7ull, raster_bounding_box, "raster bounding box") \
    X(0x290BBC50F4AE637Full, stencil_ops, "stencil operations") \
    X(0xDD6A7FA92A7D2674ull, draw_once, "draw of one instance") \
    X(0x81C197B87D4DF426ull, scratch_d36, "scratch 0xD36 and 0x376") \
    X(0xD246FDDF3A6173D7ull, clear_const_buffer, "constant buffer clear") \
    X(0xEE4D0004BEC8ECF4ull, clear_memory, "memory clear") \
    X(0xFC0CF27F5FFAA661ull, transform_feedback_setup, "transform feedback setup") \
    X(0x9E3CA7054D12D63Cull, firmware_call4, "firmware call 4") \
    X(0xD207959154DCA521ull, render_target_reset, "render target 0 and state reset") \
    X(0x3F5E74B9C9A50164ull, multi_draw_indexed_count, "indexed draws from records, with a count") \
    X(0xB5F74EDB717278ECull, draw_byte_count, "draw of a byte count") \
    X(0x6C97861D891EDF7Eull, cb_clear_5f00, "constant buffer clear, 0x5F00 bytes") \
    X(0xDBCD08D629ABC472ull, cb_select_offset_5f00, "constant buffer select at offset, 0x5F00 bytes") \
    X(0xAE29BDE4BD4C7ED1ull, scratch_d29, "scratch 0xD29")

#define MME_ID(hash, run, name) MME_N_##run,
enum { MME_NATIVES(MME_ID) MME_N_COUNT };
#define MME_HASH(hash, run, name) hash,
static const u64 mme_native_hashes[MME_N_COUNT] = {MME_NATIVES(MME_HASH)};
#define MME_RUN(hash, run, name) run,
static int (*const mme_native_runs[MME_N_COUNT])(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) = {
    MME_NATIVES(MME_RUN)};
#define MME_NAME(hash, run, name) name,
static const char* const mme_native_names[MME_N_COUNT] = {MME_NATIVES(MME_NAME)};
#define MME_CASE(hash, run, name) \
    case hash: return MME_N_##run;
int mme_native_find(u64 hash) {
    switch (hash) {
        MME_NATIVES(MME_CASE)
    default: return -1;
    }
}
const char* mme_native_name(int k) { return mme_native_names[k]; }
u64 mme_native_hash(int k) { return mme_native_hashes[k]; }
int mme_native_run(int k, GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    return mme_native_runs[k](ch, rec, p, np);
}
