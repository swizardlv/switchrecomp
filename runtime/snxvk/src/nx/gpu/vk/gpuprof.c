/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_gpu_profile=1: where the GPU's time goes. A scene can be bound by the GPU while every CPU thread idles (one
   game's lookout ran at 6 frames a second with the GPU 91% busy and no thread past 20%), and the CPU's profilers can't
   say which work it is. Each rendering pass of the frame's command buffers gets a timestamp at its start and its end,
   with its first color target (or its depth target), its draws, and what was recorded between it and the pass before
   (copies by kind and bytes, barriers, dispatches: record.c counts them). When the frame's work is done
   (frame_recycle) the times are read, and every 10 s (a slow scene takes minutes for 600 presents) the log gets the
   frame's span, the time in passes, the time in the gaps between them by what they held, and the targets that took the
   most. That is how 955 MB of buffer copies a frame between passes were found. Off, it costs a branch at each pass */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

#define GP_QUERIES 8192 /* timestamps per frame slot: two per pass */
#define GP_KEYS    1024 /* targets summed over 600 presents */

typedef struct {
    u64 gva;
    u32 w, h, fmt, draws;
    u32 xfers, barriers, dispatches; /* recorded since the pass before it ended: what the gap before it holds */
    u32 kind[3];
    u64 bytes;
} GpPass;
typedef struct {
    VkQueryPool pool;
    u32 used; /* timestamps written in this slot since its reset */
    int reset; /* the reset is recorded for this use of the slot */
    GpPass pass[GP_QUERIES / 2];
} GpSlot;
typedef struct {
    u64 gva;
    u32 w, h, fmt;
    u64 ns, passes, draws;
} GpKey;

static int g_on = -1;
static GpSlot* g_slot[VK_FRAMES];
static GpKey g_key[GP_KEYS];
static u64 g_pass_ns, g_span_ns, g_frames, g_lost;
static u64 g_draws_at_begin;
/* the gaps between passes by what was recorded in them: dispatches, else copies, else barriers, else nothing (the
   GPU waiting for the next submission or a semaphore) */
static const char* const g_gap_name[4] = {"with dispatches", "with copies", "with barriers only", "with nothing"};
static u64 g_gap_ns[4], g_gaps[4];
static u64 g_x_at_end, g_b_at_end, g_d_at_end, g_k_at_end[3], g_bytes_at_end;
/* the copy gaps by their mix: buffer copies only, with buffer-image copies, with image copies or blits */
static const char* const g_mix_name[3] = {"buffer copies only", "with buffer-image copies", "with image copies"};
static u64 g_mix_ns[3], g_mixes[3], g_mix_bytes[3], g_mix_n[3];
static int g_in_pass;

static int gp_on(void) {
    if (g_on < 0) {
        g_on = g_settings.gpu_profile && g_vk.props.limits.timestampComputeAndGraphics;
        if (g_settings.gpu_profile && !g_on) WARN("vulkan: vk_gpu_profile: the device has no timestamps on its graphics queue\n");
    }
    return g_on;
}
static GpSlot* gp_slot(void) {
    u32 s = vk_frame_slot();
    if (!g_slot[s]) {
        VkQueryPoolCreateInfo qi = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GP_QUERIES};
        GpSlot* g = calloc(1, sizeof *g);
        if (!g || !VK_OK(vkCreateQueryPool(g_vk.dev, &qi, NULL, &g->pool))) {
            free(g);
            g_on = 0;
            return NULL;
        }
        g_slot[s] = g;
    }
    return g_slot[s];
}
/* before vkCmdBeginRendering, outside any pass: the reset of the slot's pool if this is its first pass since recycled */
void vk_gpuprof_pass_begin(VkCommandBuffer cb, const Surf* rt, const Surf* zt) {
    GpSlot* g;
    if (!gp_on() || !(g = gp_slot())) return;
    if (!g->reset) {
        rec_CmdResetQueryPool(cb, g->pool, 0, GP_QUERIES);
        g->reset = 1;
        g->used = 0;
    }
    if (g->used + 2 > GP_QUERIES) {
        g_lost++;
        return;
    }
    const Surf* s = rt ? rt : zt;
    GpPass* p = &g->pass[g->used / 2];
    p->gva = s ? s->gva : 0;
    p->w = s ? s->w : 0;
    p->h = s ? s->h : 0;
    p->fmt = s ? (u32)s->f.vk : 0;
    p->xfers = (u32)(g_rec_xfers - g_x_at_end);
    p->barriers = (u32)(g_rec_barriers - g_b_at_end);
    p->dispatches = (u32)(g_vk_dispatches - g_d_at_end);
    for (int i = 0; i < 3; i++) p->kind[i] = (u32)(g_rec_xfer_kind[i] - g_k_at_end[i]);
    p->bytes = g_rec_buf_bytes - g_bytes_at_end;
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g->pool, g->used);
    g_draws_at_begin = g_vk_drawn;
    g_in_pass = 1;
}
/* after vkCmdEndRendering */
void vk_gpuprof_pass_end(VkCommandBuffer cb) {
    GpSlot* g;
    if (!g_in_pass) return;
    g_in_pass = 0;
    if (!(g = gp_slot())) return;
    g->pass[g->used / 2].draws = (u32)(g_vk_drawn - g_draws_at_begin);
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g->pool, g->used + 1);
    g->used += 2;
    g_x_at_end = g_rec_xfers;
    g_b_at_end = g_rec_barriers;
    g_d_at_end = g_vk_dispatches;
    for (int i = 0; i < 3; i++) g_k_at_end[i] = g_rec_xfer_kind[i];
    g_bytes_at_end = g_rec_buf_bytes;
}
static void gp_report(void);
static GpKey* gp_key(const GpPass* p) {
    u64 h = (p->gva >> 8) ^ ((u64)p->w << 40) ^ ((u64)p->h << 20) ^ p->fmt;
    for (u32 i = 0; i < GP_KEYS; i++) {
        GpKey* k = &g_key[(h + i) & (GP_KEYS - 1)];
        if (!k->passes) {
            k->gva = p->gva, k->w = p->w, k->h = p->h, k->fmt = p->fmt;
            return k;
        }
        if (k->gva == p->gva && k->w == p->w && k->h == p->h && k->fmt == p->fmt) return k;
    }
    return NULL;
}
/* the slot's work is done (frame_recycle, before the slot records again): its times into the sums */
void vk_gpuprof_frame_done(u32 slot) {
    GpSlot* g = g_on > 0 ? g_slot[slot] : NULL;
    if (!g || !g->reset) return;
    g->reset = 0;
    if (!g->used) return;
    static u64 ts[GP_QUERIES];
    if (vkGetQueryPoolResults(g_vk.dev, g->pool, 0, g->used, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    double period = g_vk.props.limits.timestampPeriod;
    for (u32 i = 0; i + 1 < g->used; i += 2) {
        u64 ns = (u64)((double)(ts[i + 1] - ts[i]) * period);
        GpKey* k = gp_key(&g->pass[i / 2]);
        g_pass_ns += ns;
        if (!k) continue;
        k->ns += ns;
        k->passes++;
        k->draws += g->pass[i / 2].draws;
    }
    for (u32 i = 2; i + 1 < g->used; i += 2) {
        const GpPass* p = &g->pass[i / 2];
        int c = p->dispatches ? 0 : p->xfers ? 1 : p->barriers ? 2 : 3;
        u64 gns = (u64)((double)(ts[i] - ts[i - 1]) * period);
        g_gap_ns[c] += gns;
        g_gaps[c]++;
        if (c == 1) {
            int m = p->kind[2] ? 2 : p->kind[1] ? 1 : 0;
            g_mix_ns[m] += gns;
            g_mixes[m]++;
            g_mix_bytes[m] += p->bytes;
            g_mix_n[m] += p->kind[0] + p->kind[1] + p->kind[2];
        }
    }
    g_span_ns += (u64)((double)(ts[g->used - 1] - ts[0]) * period);
    g_frames++;
    static u64 last;
    u64 now = plat_ns();
    if (!last) last = now;
    if (now - last >= 10000000000ull) {
        last = now;
        gp_report();
    }
}
static int key_cmp(const void* a, const void* b) {
    u64 x = ((const GpKey*)a)->ns, y = ((const GpKey*)b)->ns;
    return x < y ? 1 : x > y ? -1 : 0;
}
/* the targets that took the most GPU time, then a fresh sum */
static void gp_report(void) {
    if (g_on <= 0 || !g_frames) return;
    static GpKey sorted[GP_KEYS];
    memcpy(sorted, g_key, sizeof sorted);
    qsort(sorted, GP_KEYS, sizeof *sorted, key_cmp);
    LOG("vulkan: GPU over %llu frames: %.2f ms a frame in passes, %.2f ms from the first pass to the last, %llu passes "
        "untimed\n",
        (unsigned long long)g_frames, (double)g_pass_ns / 1e6 / (double)g_frames,
        (double)g_span_ns / 1e6 / (double)g_frames, (unsigned long long)g_lost);
    for (int c = 0; c < 4; c++)
        if (g_gaps[c])
            LOG("vulkan: GPU   gaps %s: %.2f ms a frame, %llu of them\n", g_gap_name[c],
                (double)g_gap_ns[c] / 1e6 / (double)g_frames, (unsigned long long)g_gaps[c]);
    for (int m = 0; m < 3; m++)
        if (g_mixes[m])
            LOG("vulkan: GPU     copy gaps %s: %.2f ms a frame, %llu gaps, %llu copies, %.2f MB of buffer copies a frame\n",
                g_mix_name[m], (double)g_mix_ns[m] / 1e6 / (double)g_frames, (unsigned long long)g_mixes[m],
                (unsigned long long)g_mix_n[m], (double)g_mix_bytes[m] / 1048576.0 / (double)g_frames);
    memset(g_mix_ns, 0, sizeof g_mix_ns);
    memset(g_mixes, 0, sizeof g_mixes);
    memset(g_mix_bytes, 0, sizeof g_mix_bytes);
    memset(g_mix_n, 0, sizeof g_mix_n);
    memset(g_gap_ns, 0, sizeof g_gap_ns);
    memset(g_gaps, 0, sizeof g_gaps);
    for (int i = 0; i < 12 && sorted[i].passes; i++)
        LOG("vulkan: GPU   %.2f ms a frame: %llu passes, %llu draws, target %llx %ux%u format %u\n",
            (double)sorted[i].ns / 1e6 / (double)g_frames, (unsigned long long)sorted[i].passes,
            (unsigned long long)sorted[i].draws, (unsigned long long)sorted[i].gva, sorted[i].w, sorted[i].h,
            sorted[i].fmt);
    memset(g_key, 0, sizeof g_key);
    g_pass_ns = g_span_ns = g_frames = g_lost = 0;
}
