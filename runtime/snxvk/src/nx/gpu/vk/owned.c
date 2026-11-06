/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest memory the GPU holds: the renderer's side of memory/own.c (docs/GPU_OWNERSHIP.md). A resource is a
   device-local buffer holding guest bytes in their own layout; an engine that wrote it publishes it over those bytes,
   and the trap lands it when the CPU touches them: copied by the GPU into host-cached memory, waited for (that
   submission only), and copied into the pool's mirror. On a host without traps a publish lands at once. Landing runs
   on whatever thread faulted or acquired, under vk_lock, which own.c takes before it marks what lands (OwnHooks'
   lock) and own_land again for itself (it is reentrant, and a publish without traps lands with no landing of own.c's) */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"

static int g_own_traps; /* own_init found traps: publishing owns, else it lands */

VkOwnRes* vk_own_res_new(u64 size) {
    VkOwnRes* r = (VkOwnRes*)calloc(1, sizeof *r);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size ? size : 16,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!r) return NULL;
    if (size > g_vk.alloc_cap || !VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &r->buf, &r->mem, NULL))) {
        free(r);
        return NULL;
    }
    r->size = size;
    r->refs = 1;
    return r;
}
static void res_retain(void* res) { __atomic_add_fetch(&((VkOwnRes*)res)->refs, 1, __ATOMIC_RELAXED); }
static void res_release(void* res) {
    VkOwnRes* r = (VkOwnRes*)res;
    if (__atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL)) return;
    vk_lock();
    vk_defer_buffer(r->buf, r->mem); /* the frame recording may still copy from it */
    vk_unlock();
    free(r);
}
void vk_own_res_unref(VkOwnRes* r) { res_release(r); }

/* every write the GPU did so far visible to copies (before), the copies' writes to the host (after) */
static void land_barrier(VkCommandBuffer cb, int before) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    if (before) {
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    } else {
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    }
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}

/* a piece of a landing: res's bytes from src land at dst, staged at `at` in the batch's host-cached copy */
typedef struct {
    const VkOwnRes* res;
    u64 src, at, size;
    u8* dst;
} Piece;

/* own.c's land: the ranges in batches that host-cached staging takes (a range bigger than that in pieces), each batch
   one submission, waited for */
static void own_land(const OwnRange* r, u8* const* dst, u32 n) {
    u64 cap = g_vk.alloc_cap / 4, done = 0;
    u32 i = 0;
    vk_lock();
    vk_render_end();
    while (i < n) {
        Piece pc[64];
        u32 np = 0, j;
        u64 total = 0, off;
        VkBuffer back;
        VmaAllocation mem;
        VkCommandBuffer cb;
        u8* p;
        while (i < n && np < sizeof pc / sizeof *pc && total < cap) {
            u64 len = r[i].size - done < cap - total ? r[i].size - done : cap - total;
            pc[np].res = (const VkOwnRes*)r[i].res;
            pc[np].src = r[i].res_off + done;
            pc[np].at = total;
            pc[np].size = len;
            pc[np].dst = dst[i] + done;
            np++;
            total += len;
            done += len;
            if (done == r[i].size) {
                i++;
                done = 0;
            }
        }
        p = vk_stage_back(total, &back, &off, &mem);
        if (!p)
            fatal("vulkan: no host memory to land %llu KiB of GPU-owned guest memory",
                  (unsigned long long)(total >> 10));
        cb = vk_cmd();
        land_barrier(cb, 1);
        for (j = 0; j < np; j++) {
            VkBufferCopy c = {pc[j].src, off + pc[j].at, pc[j].size};
            rec_CmdCopyBuffer(cb, pc[j].res->buf, back, 1, &c);
        }
        land_barrier(cb, 0);
        vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
        vk_wait_submit(vk_last_submit());
        vmaInvalidateAllocation(g_vk.vma, mem, off, total);
        for (j = 0; j < np; j++) memcpy(pc[j].dst, p + pc[j].at, pc[j].size);
    }
    vk_unlock();
}

static u32 lock_owner(void) { return vk_lock_owner(); }

int vk_own_init(void) {
    /* vk_lock and vk_unlock themselves, not g_vklock: the outermost unlock also signals what the hold's releases
       reached (vk_release_events) */
    OwnHooks h = {.land = own_land,
                  .retain = res_retain,
                  .release = res_release,
                  .busy_tid = lock_owner,
                  .lock = vk_lock,
                  .unlock = vk_unlock};
    g_own_traps = !own_init(&h);
    return g_own_traps ? 0 : -1;
}

void vk_own_publish(u64 cpu, u64 size, VkOwnRes* r, u64 off) {
    OwnRange o;
    u8* d;
    if (!size) return;
    gpu_mem_written_cpu(cpu, size); /* what is built from the bytes (textures, buffers) takes the new ones */
    if (g_own_traps && !own_begin(cpu, size, r, off)) return;
    /* not trapped here: older owners of the bytes let go of them, and the result lands now */
    own_drop(cpu, size);
    o.addr = cpu;
    o.size = size;
    o.res = r;
    o.res_off = off;
    d = GP(cpu);
    own_land(&o, &d, 1);
}

/* ---------------------------------------------------------------- reading guest memory without landing it */
/* GPU writes before reading what they wrote (before), the reads' copies before what consumes them (after) */
static void read_barrier(VkCommandBuffer cb, int before) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = before ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_COPY_BIT,
                          .srcAccessMask = before ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          .dstStageMask = before ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = before ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_MEMORY_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}

/* guest CPU bytes [cpu, cpu + size) into host, and the owned parts copied by the GPU into buf at at (host's place
   there) over them: the copies recorded so far, their barriers left to the caller (*any set when there were some) */
static void cpu_read(u64 cpu, u64 size, u8* host, VkBuffer buf, u64 at, int* any) {
    OwnRange o[16];
    u32 n, i;
    if (!__atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE)) {
        memcpy(host, GP(cpu), (size_t)size);
        return;
    }
    n = own_stage(cpu, size, host, o, sizeof o / sizeof *o);
    if (n > sizeof o / sizeof *o) { /* more owners than noted: they land, and the bytes are read as they are then */
        own_acquire(cpu, size);
        memcpy(host, GP(cpu), (size_t)size);
        return;
    }
    for (i = 0; i < n; i++) {
        VkBufferCopy c = {o[i].res_off, at + (o[i].addr - cpu), o[i].size};
        if (!*any) read_barrier(vk_cmd(), 1);
        *any = 1;
        rec_CmdCopyBuffer(vk_cmd(), ((const VkOwnRes*)o[i].res)->buf, buf, 1, &c);
    }
}
void vk_cpu_read(u64 cpu, u64 size, u8* host, VkBuffer buf, u64 at) {
    int any = 0;
    cpu_read(cpu, size, host, buf, at, &any);
    if (any) read_barrier(vk_cmd(), 0);
}
void vk_guest_read(u64 gva, u64 size, u8* host, VkBuffer buf, u64 at) {
    u64 o = 0;
    int any = 0;
    if (!size) return;
    gmem_need(gva, size); /* results the renderer has pending there land first, as gmem_read has them */
    while (o < size) {
        u64 cpu = 0, n = gmmu_run(gva + o, size - o, &cpu);
        if (!n) { /* an unmapped page reads as zero */
            n = 0x1000 - ((gva + o) & 0xFFF);
            if (n > size - o) n = size - o;
            memset(host + o, 0, (size_t)n);
        } else
            cpu_read(cpu, n, host + o, buf, at + o, &any);
        o += n;
    }
    if (any) read_barrier(vk_cmd(), 0);
}

void vk_guest_peek(u64 gva, void* dst, u64 size) {
    u8* d = (u8*)dst;
    u64 o = 0;
    if (!size) return;
    gmem_need(gva, size);
    while (o < size) {
        u64 cpu = 0, n = gmmu_run(gva + o, size - o, &cpu);
        if (!n) {
            n = 0x1000 - ((gva + o) & 0xFFF);
            if (n > size - o) n = size - o;
            memset(d + o, 0, (size_t)n);
        } else if (!__atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE))
            memcpy(d + o, GP(cpu), (size_t)n);
        else {
            OwnRange r[16];
            u32 k = own_stage(cpu, n, d + o, r, sizeof r / sizeof *r), i;
            if (k > sizeof r / sizeof *r) {
                own_acquire(cpu, n);
                memcpy(d + o, GP(cpu), (size_t)n);
            } else
                for (i = 0; i < k; i++) { /* the owner's identity in place of its stale bytes */
                    u64 tag = ((u64)(uintptr_t)r[i].res * 0x9E3779B97F4A7C15ull) ^ (r[i].res_off - (r[i].addr - cpu));
                    u8* t = d + o + (r[i].addr - cpu);
                    for (u64 b = 0; b < r[i].size; b++) t[b] = (u8)(tag >> (8 * ((r[i].addr + b) & 7)));
                }
        }
        o += n;
    }
}

void vk_all_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}
void vk_copy_rows(VkCommandBuffer cb, VkBuffer src, u64 src_at, u32 src_pitch, VkBuffer dst, u64 dst_at,
                  u32 dst_pitch, u32 row_bytes, u32 rows) {
    VkBufferCopy c[64];
    u32 y = 0;
    if (!row_bytes) return;
    while (y < rows) {
        u32 n = 0;
        for (; y < rows && n < sizeof c / sizeof *c; y++, n++) {
            c[n].srcOffset = src_at + (u64)y * src_pitch;
            c[n].dstOffset = dst_at + (u64)y * dst_pitch;
            c[n].size = row_bytes;
        }
        rec_CmdCopyBuffer(cb, src, dst, n, c);
    }
}
