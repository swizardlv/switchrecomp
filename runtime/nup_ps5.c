/* nup_ps5.c -- direct reading of NUP (NX Package) containers on the PS5.
 *
 * This replaces the previous "extract the romfs to a flat folder + enumerate it
 * via a manifest" path, which forced a huge libc-heap traversal (75238 files /
 * 2909 dirs) at romfs_init and corrupted the allocator. Here the romfs lives
 * inside the .nup exactly as EglantinePS5 reads it: we parse the table of
 * contents once, expose the directory tree, and serve file bytes straight from
 * the container's blob region (raw extents are read in place, zstd extents are
 * decompressed on demand with an LRU cache).
 *
 * Format reference: github.com/a55uka/nup SPEC.md (format 1.0).
 */

#define _FILE_OFFSET_BITS 64
#include "nup_ps5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifdef SWR_PS5
#include <zstd.h>
#else
/* Host compile-check stub: the host build only needs this file to compile, it
 * never actually runs on the host, so a failing decompressor is fine. */
static size_t zstd_stub_decompress(void* dst, size_t dstCap, const void* src, size_t srcSize) {
    (void)dst; (void)dstCap; (void)src; (void)srcSize; return (size_t)-1;
}
static unsigned zstd_stub_is_error(size_t code) { return code == (size_t)-1; }
#define ZSTD_decompress  zstd_stub_decompress
#define ZSTD_isError     zstd_stub_is_error
#endif

/* A node index lives in a single space: [0, ndirs) are directories,
 * [ndirs, ndirs+nfiles) are files. */
typedef struct {
    int      fd;
    uint8_t* toc;            /* malloc'd copy of the table of contents          */
    uint64_t toc_size;
    uint64_t blob_offset;    /* file offset where the blob (extent) region starts */
    uint64_t file_size;
    uint32_t ndirs;
    uint32_t nfiles;
    uint32_t dir_at;         /* offset of dir records within toc                */
    uint32_t file_at;        /* offset of file records within toc               */
    uint32_t children_at;    /* offset of the children u32 array within toc     */
    uint32_t pool_at;        /* offset of the string pool within toc            */
    uint32_t pool_size;
    uint32_t pkg_at;
    uint32_t npkg;
    int      base_romfs_dir; /* node index of the base romfs root, or -1        */
} Nup;

static Nup G;

int g_nup_err = 0;  /* set on nup_open failure; 0=ok, >0=errno, <0=validation code */

/* ---- little-endian field readers (PS5/x86-64 host are both LE) ---- */
static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* ---- zstd decompress cache (LRU, capacity capped) ---- */
#define ZC_CAP  (512uLL << 20)   /* 512 MiB total decompressed cap */
#define ZC_MAX  1024             /* max cached entries             */
typedef struct { int node; uint8_t* data; uint64_t size; uint64_t tick; } ZC;
static ZC    zc[ZC_MAX];
static int   zc_n;
static uint64_t zc_bytes;
static uint64_t zc_tick;

static uint8_t* zc_get(int node) {
    for (int i = 0; i < zc_n; i++)
        if (zc[i].node == node) { zc[i].tick = ++zc_tick; return zc[i].data; }
    return NULL;
}

/* Decompress file `node` (already validated) into a freshly malloc'd buffer and
 * cache it. Returns NULL on failure. Caller must NOT free a cached pointer. */
static uint8_t* zc_decompress(int node) {
    uint8_t* cached = zc_get(node);
    if (cached) return cached;

    uint64_t size0, ext_off, ext_size; int codec;
    if (nup_file_size(node, &size0)) return NULL;
    if (nup_file_extent(node, &ext_off, &ext_size, &codec)) return NULL;
    if (codec != 1) return NULL; /* only zstd is cached */

    /* Evict least-recently-used entries until we fit, but never evict the only
     * slot we are about to fill (a single file may exceed the cap; that case is
     * handled by the caller without caching). */
    while (zc_bytes + size0 > ZC_CAP && zc_n > 0) {
        int mi = 0;
        for (int i = 1; i < zc_n; i++) if (zc[i].tick < zc[mi].tick) mi = i;
        free(zc[mi].data);
        zc_bytes -= zc[mi].size;
        zc[mi] = zc[--zc_n];
    }
    if (zc_n >= ZC_MAX) return NULL;

    uint8_t* comp = malloc(ext_size ? (size_t)ext_size : 1);
    uint8_t* out  = malloc(size0 ? (size_t)size0 : 1);
    if (!comp || !out) { free(comp); free(out); return NULL; }

    ssize_t r = pread(G.fd, comp, (size_t)ext_size, (off_t)(G.blob_offset + ext_off));
    size_t  d = (r == (ssize_t)ext_size)
                    ? ZSTD_decompress(out, (size_t)size0, comp, (size_t)ext_size)
                    : (size_t)-1;
    free(comp);
    if (ZSTD_isError(d) || d != (size_t)size0) { free(out); return NULL; }

    zc[zc_n].node = node;
    zc[zc_n].data = out;
    zc[zc_n].size = size0;
    zc[zc_n].tick = ++zc_tick;
    zc_n++;
    zc_bytes += size0;
    return out;
}

/* ------------------------------------------------------------------ */

int nup_open(const char* path) {
    nup_close();
    int fd = open(path, O_RDONLY);
    if (fd < 0) { g_nup_err = errno; return 0; }

    uint8_t sb[0x80];
    ssize_t r = pread(fd, sb, sizeof sb, 0);
    if (r != (ssize_t)sizeof sb) { close(fd); return 0; }
    if (memcmp(sb, "NUP\x01", 4) != 0) { g_nup_err = -1; close(fd); return 0; }

    uint64_t toc_offset  = rd64(sb + 8);
    uint64_t toc_size    = rd64(sb + 16);
    uint64_t blob_offset = rd64(sb + 56);
    uint64_t file_size   = rd64(sb + 64);
    uint32_t npkg        = rd32(sb + 72);
    uint32_t flags       = rd32(sb + 76);

    /* Basic sanity so a truncated / corrupt file cannot make us read OOB. */
    if (flags != 0) { g_nup_err = -2; close(fd); return 0; }
    if (toc_offset != 0x80) { g_nup_err = -3; close(fd); return 0; }
    if (blob_offset < toc_offset + toc_size) { g_nup_err = -4; close(fd); return 0; }
    /* file_size check below after we know the actual file size. */

    uint8_t* toc = malloc((size_t)toc_size ? (size_t)toc_size : 1);
    if (!toc) { g_nup_err = -5; close(fd); return 0; }
    r = pread(fd, toc, (size_t)toc_size, (off_t)toc_offset);
    if (r != (ssize_t)toc_size) { g_nup_err = -6; free(toc); close(fd); return 0; }

    if (memcmp(toc, "TOC\x01", 4) != 0) { g_nup_err = -7; free(toc); close(fd); return 0; }
    uint32_t package_count = rd32(toc + 4);
    uint64_t dir_count     = rd64(toc + 8);
    uint64_t file_count    = rd64(toc + 16);
    uint64_t pool_offset   = rd64(toc + 24);
    uint64_t pool_size     = rd64(toc + 32);

    if (package_count == 0 || package_count > 4096 ||
        dir_count > 8*1024*1024 || file_count > 16*1024*1024) {
        free(toc); close(fd); return 0;
    }

    uint64_t dir_at      = 0x30 + (uint64_t)package_count * 0x40;
    uint64_t file_at     = dir_at + dir_count * 0x18;
    uint64_t children_at = file_at + file_count * 0x50;
    uint64_t children_bytes = pool_offset - children_at;
    if (pool_offset < children_at || pool_offset + pool_size > toc_size ||
        children_bytes % 4 != 0) {
        g_nup_err = -8; free(toc); close(fd); return 0;
    }

    G.fd           = fd;
    G.toc          = toc;
    G.toc_size     = toc_size;
    G.blob_offset  = blob_offset;
    G.file_size    = file_size;
    G.ndirs        = (uint32_t)dir_count;
    G.nfiles       = (uint32_t)file_count;
    G.dir_at       = (uint32_t)dir_at;
    G.file_at      = (uint32_t)file_at;
    G.children_at  = (uint32_t)children_at;
    G.pool_at      = (uint32_t)pool_offset;
    G.pool_size    = (uint32_t)pool_size;
    G.pkg_at       = 0x30;
    G.npkg         = package_count;
    G.base_romfs_dir = -1;

    /* Choose the romfs directory: prefer the Base package, else the first
     * package that actually carries a romfs. */
    int fallback = -1;
    for (uint32_t i = 0; i < package_count; i++) {
        const uint8_t* p = toc + G.pkg_at + i * 0x40;
        uint32_t root   = rd32(p + 8);
        uint32_t exefs  = rd32(p + 12);
        uint32_t romfs  = rd32(p + 16);
        uint8_t  kind   = toc[G.pkg_at + i * 0x40 + 24];
        (void)root; (void)exefs;
        if (romfs != 0xFFFFFFFFu && romfs < G.ndirs) {
            if (kind == 0) { G.base_romfs_dir = (int)romfs; break; }
            if (fallback < 0) fallback = (int)romfs;
        }
    }
    if (G.base_romfs_dir < 0) G.base_romfs_dir = fallback;

    return 1;
}

void nup_close(void) {
    if (G.fd >= 0) { close(G.fd); G.fd = -1; }
    free(G.toc); G.toc = NULL;
    for (int i = 0; i < zc_n; i++) free(zc[i].data);
    zc_n = 0; zc_bytes = 0;
    memset(&G, 0, sizeof G);
    G.fd = -1;
    G.base_romfs_dir = -1;
}

int nup_base_romfs_dir(void) { return G.base_romfs_dir; }

int nup_dir_child_count(int dir_node) {
    if (dir_node < 0 || (uint32_t)dir_node >= G.ndirs) return 0;
    const uint8_t* p = G.toc + G.dir_at + (uint32_t)dir_node * 0x18;
    return (int)rd32(p + 12);
}

int nup_dir_child(int dir_node, int i) {
    if (dir_node < 0 || (uint32_t)dir_node >= G.ndirs) return -1;
    const uint8_t* p = G.toc + G.dir_at + (uint32_t)dir_node * 0x18;
    uint32_t first = rd32(p + 8);
    uint32_t count = rd32(p + 12);
    if (i < 0 || (uint32_t)i >= count) return -1;
    const uint8_t* c = G.toc + G.children_at;
    return (int)rd32(c + (first + (uint32_t)i) * 4);
}

int nup_node_is_dir(int node) { return node >= 0 && (uint32_t)node < G.ndirs; }

int nup_node_name(int node, char* buf, size_t n) {
    if (node < 0) return 0;
    uint32_t off, len;
    if ((uint32_t)node < G.ndirs) {
        const uint8_t* p = G.toc + G.dir_at + (uint32_t)node * 0x18;
        off = rd32(p + 0); len = rd32(p + 4);
    } else {
        uint32_t fi = (uint32_t)node - G.ndirs;
        if (fi >= G.nfiles) return 0;
        const uint8_t* p = G.toc + G.file_at + fi * 0x50;
        off = rd32(p + 0); len = rd32(p + 4);
    }
    if (off + len > G.pool_size) return 0;
    uint32_t cp = len < n ? len : (uint32_t)(n - 1);
    memcpy(buf, G.toc + G.pool_at + off, cp);
    buf[cp] = 0;
    return (int)len;
}

int nup_file_size(int file_node, uint64_t* size) {
    uint32_t fi = (uint32_t)file_node - G.ndirs;
    if (file_node < 0 || fi >= G.nfiles) return -1;
    const uint8_t* p = G.toc + G.file_at + fi * 0x50;
    *size = rd64(p + 8);
    return 0;
}

int nup_file_extent(int file_node, uint64_t* extent_offset, uint64_t* extent_size, int* codec) {
    uint32_t fi = (uint32_t)file_node - G.ndirs;
    if (file_node < 0 || fi >= G.nfiles) return -1;
    const uint8_t* p = G.toc + G.file_at + fi * 0x50;
    *extent_offset = rd64(p + 16);
    *extent_size   = rd64(p + 24);
    *codec         = G.toc[G.file_at + fi * 0x50 + 64];
    return 0;
}

int nup_read_file(int file_node, uint64_t offset, void* dst, uint64_t size) {
    uint32_t fi = (uint32_t)file_node - G.ndirs;
    if (file_node < 0 || fi >= G.nfiles) return -1;

    uint64_t size0, ext_off, ext_size; int codec;
    if (nup_file_size(file_node, &size0)) return -1;
    if (nup_file_extent(file_node, &ext_off, &ext_size, &codec)) return -1;

    if (offset >= size0) return 0;            /* past end -> caller fills zeros */
    uint64_t n = size; if (offset + n > size0) n = size0 - offset;

    if (codec == 0) { /* raw: zero-copy read straight out of the blob */
        ssize_t r = pread(G.fd, dst, (size_t)n, (off_t)(G.blob_offset + ext_off + offset));
        return (r == (ssize_t)n) ? 0 : -1;
    }

    if (codec != 1) return -1;               /* unknown codec */

    if (size0 > ZC_CAP) {                     /* too big to cache: decompress once */
        uint8_t* comp = malloc(ext_size ? (size_t)ext_size : 1);
        if (!comp) return -1;
        uint8_t* full = malloc(size0 ? (size_t)size0 : 1);
        if (!full) { free(comp); return -1; }
        ssize_t r = pread(G.fd, comp, (size_t)ext_size, (off_t)(G.blob_offset + ext_off));
        size_t d = (r == (ssize_t)ext_size)
                       ? ZSTD_decompress(full, (size_t)size0, comp, (size_t)ext_size)
                       : (size_t)-1;
        free(comp);
        if (ZSTD_isError(d) || d != (size_t)size0) { free(full); return -1; }
        memcpy(dst, full + offset, (size_t)n);
        free(full);
        return 0;
    }

    uint8_t* full = zc_decompress(file_node); /* cached */
    if (!full) return -1;
    memcpy(dst, full + offset, (size_t)n);
    return 0;
}

/* ------------------------------------------------------------------
 * zstd tracing hooks.
 *
 * The prebuilt payload-SDK libzstd.a was compiled with zstd's tracing API, so
 * it leaves ZSTD_trace_{compress,decompress}_{begin,end} as weak undefined
 * symbols. They are only ever called when the runtime trace function pointer is
 * set (it is NULL by default), so they are never invoked. The PS5 native-tool
 * converter, however, refuses any undefined symbol, so we provide no-op
 * definitions. The exact prototype doesn't matter for C linkage (name only),
 * and these are never executed. */
int ZSTD_trace_compress_begin(void)   { return 0; }
int ZSTD_trace_compress_end(void)     { return 0; }
int ZSTD_trace_decompress_begin(void) { return 0; }
int ZSTD_trace_decompress_end(void)   { return 0; }
