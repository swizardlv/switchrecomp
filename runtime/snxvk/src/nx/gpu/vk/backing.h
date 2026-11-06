/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_BACKING_H
#define SNX_VK_BACKING_H
#include <stdlib.h>
#include "nx/hle/services/nvdrv.h"

static int vk_backing_page_compare(const void* a, const void* b) {
    u64 x = *(const u64*)a, y = *(const u64*)b;
    return (x > y) - (x < y);
}
/* Mapping changes hold vk_lock until results land, so translations are stable.
   Page overlap is conservative: disjoint bytes on one page may land early. */
static int vk_backing_overlap(u64 a, u64 an, u64 b, u64 bn) {
    if (!an || !bn) return 0;
    if (a >= b ? a - b < bn : b - a < an) return 1;
    if (an - 1 > UINT64_MAX - a || bn - 1 > UINT64_MAX - b) return 1;
    u64 na = ((a + an - 1) >> 12) - (a >> 12) + 1;
    u64 nb = ((b + bn - 1) >> 12) - (b >> 12) + 1;
    if (na > nb) {
        u64 t = a; a = b; b = t;
        t = na; na = nb; nb = t;
    }
    u64 local[64];
    if (na > SIZE_MAX / sizeof *local) return 1;
    u64* pages = na <= 64 ? local : malloc((size_t)na * sizeof *pages);
    if (!pages) return 1; /* cannot prove disjointness */
    size_t n = 0;
    for (u64 i = 0; i < na; i++) {
        u64 cpu = gmmu_translate((a & ~4095ull) + i * 4096);
        if (cpu) pages[n++] = cpu >> 12;
    }
    qsort(pages, n, sizeof *pages, vk_backing_page_compare);
    int overlap = 0;
    for (u64 i = 0; i < nb && n; i++) {
        u64 cpu = gmmu_translate((b & ~4095ull) + i * 4096), page = cpu >> 12;
        if (cpu && bsearch(&page, pages, n, sizeof page, vk_backing_page_compare)) {
            overlap = 1;
            break;
        }
    }
    if (pages != local) free(pages);
    return overlap;
}
#endif
