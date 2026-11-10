/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Offline End-to-End Verification Suite for:
 *   Ampere (SM86 128-bit SASS) -> Maxwell IR (MInst) -> Canonical Maxwell 64-bit -> SPIR-V
 *
 * Tests 4 complete Ampere SM86 shaders across Vertex, Fragment, Compute, and Dual-Vertex (VertexA+VertexB):
 *   1. Vertex Shader (VS): ULDC + UIADD3 + ALD + FFMA + AST (position + color) + EXIT
 *   2. Fragment Shader (FS): IPA + ULDC + UISETP + BSSY + @!UP0 BRA + FADD + BSYNC + TEX + EXIT
 *   3. Compute Shader (CS): S2UR + ULDC (64-bit SSBO desc) + IMAD.WIDE + LDGSTS + BAR.SYNC + LDS + IADD3 + STG + EXIT
 *   4. Dual Vertex Merge (VertexA + VertexB): Two SM86 128-bit shaders merged via shader_merge_dual() -> SPIR-V
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

#include "nx/gpu/shader/shader.h"
#include "nx/gpu/shader/translator.h"
#include "nx/gpu/shader/ampere_lower.h"
#include "nx/runtime/platform/log.h"

/* Provide standalone platform globals and logger for offline test binary */
Settings g_settings = { .log_level = 0 };

int vk_dlss_xfb_wanted(void) { return 0; }

void logf_(int level, const char* fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    size_t len = strlen(fmt);
    if (len == 0 || fmt[len - 1] != '\n') {
        fputc('\n', stdout);
    }
    fflush(stdout);
}

/* Constant buffer callback used by Compute Shader SSBO descriptor lookup (c[0][0x110..0x11c]) */
static int test_cb_read(u32 idx, u32 off, u32* out) {
    if (idx == 0) {
        if (off == 0x110) { *out = 0x80001000u; return 1; }
        if (off == 0x114) { *out = 0x00000000u; return 1; }
        if (off == 0x118) { *out = 0x00010000u; return 1; }
        if (off == 0x11c) { *out = 0x00000000u; return 1; }
    }
    *out = 0;
    return 1;
}

/* Helper to pack an Ampere 128-bit instruction (lo, hi) at 16-byte index `idx` after 80-byte SPH.
 * Matches ampere_lower.c bit layout:
 *   - lo[11:0]  = opc12 (base9 in [8:0], form in [11:9]: 0=Reg, 2=Imm, 3=Cbuf)
 *   - lo[14:12] = guard predicate (0..7), lo[15] = pred_inv
 *   - lo[23:16] = rd
 *   - lo[31:24] = ra
 *   - lo[63:32] = rb (in [39:32]) or imm32 or cbuf (offset in [49:32], bank in [54:50])
 *   - hi[7:0]   = rc
 *   - hi[39:8]  = subop / flags / pred dsts
 *   - hi[44:41] = stall count (1..15), hi[45] = yield (1)
 */
static void make_amp_inst(u8* code, u32 idx,
                          u16 opc12, u8 rd, u8 ra, u32 rb_or_imm,
                          u8 rc, u8 pred_inv, u8 pred, u64 hi_mods) {
    u64 lo = (u64)(opc12 & 0xfffu)
           | ((u64)(pred & 7u) << 12)
           | ((u64)(pred_inv & 1u) << 15)
           | ((u64)rd << 16)
           | ((u64)ra << 24)
           | ((u64)rb_or_imm << 32);
    u64 hi = (u64)rc
           | hi_mods
           | (5ULL << 41) /* stall = 5 in hi[44:41] */
           | (1ULL << 45) /* yield = 1 in hi[45] */;
    u8* p = code + 0x50 + idx * 16;
    memcpy(p, &lo, 8);
    memcpy(p + 8, &hi, 8);
}

/* Verify SPIR-V header and basic structure */
static int verify_spirv(const char* label, const u32* spv, size_t words, const ShaderInfo* si) {
    printf("\n[TEST-SPIRV] Verifying %s: spv=%p words=%zu unsupported=%d reason='%s'\n",
           label, (const void*)spv, words, si->unsupported, si->first_unsupported);
    if (!spv || words < 5) {
        fprintf(stderr, "[TEST-SPIRV] FAIL: %s produced NULL or truncated SPIR-V!\n", label);
        return 0;
    }
    u32 magic = spv[0];
    u32 version = spv[1];
    u32 generator = spv[2];
    u32 bound = spv[3];
    printf("[TEST-SPIRV] %s Header: magic=0x%08x version=0x%08x generator=0x%08x bound=%u total_bytes=%zu\n",
           label, magic, version, generator, bound, words * 4);
    if (magic != 0x07230203u) {
        fprintf(stderr, "[TEST-SPIRV] FAIL: invalid SPIR-V magic 0x%08x\n", magic);
        return 0;
    }
    int has_entry = 0, has_func = 0, has_ret = 0, has_fend = 0;
    u32 inst_count = 0;
    for (size_t i = 5; i < words;) {
        u32 w0 = spv[i];
        u32 wc = w0 >> 16;
        u32 op = w0 & 0xFFFFu;
        if (wc == 0 || i + wc > words) {
            fprintf(stderr, "[TEST-SPIRV] FAIL: malformed SPIR-V instruction at word %zu (wc=%u)\n", i, wc);
            return 0;
        }
        if (op == 15) has_entry = 1;
        if (op == 54) has_func = 1;
        if (op == 253 || op == 252) has_ret = 1;
        if (op == 56) has_fend = 1;
        inst_count++;
        i += wc;
    }
    printf("[TEST-SPIRV] %s SPIR-V stream valid: %u SPIR-V instructions (EntryPoint=%d, Function=%d, Return=%d, FunctionEnd=%d)\n",
           label, inst_count, has_entry, has_func, has_ret, has_fend);
    return (has_entry && has_func && has_ret && has_fend) ? 1 : 0;
}

/* ============================================================================
 * Test 1: Ampere SM86 Vertex Shader (VS)
 *   0: ULDC.64 UR4, c[0][0x20]         (Uniform constant load -> LDC R192..R193)
 *   1: UIADD3 UR6, UR4, 4, URZ         (Uniform integer add -> IADD32I R194, R192, 4)
 *   2: ALD.128 R0, a[0x80]             (Vertex attribute load vec4 generic attr 0)
 *   3: FFMA R0, R0, R192, R193         (Vector FMA using lowered UR4/UR5!)
 *   4: AST.128 a[0x70], R0             (Store gl_Position)
 *   5: AST.128 a[0x80], R0             (Store generic output attr 0)
 *   6: EXIT                            (Terminate shader)
 * ============================================================================ */
static int test_ampere_vertex_shader(void) {
    printf("\n================================================================================\n");
    printf("TEST 1: Ampere SM86 128-bit Vertex Shader -> Maxwell IR -> SPIR-V\n");
    printf("================================================================================\n");

    u8 code[4096];
    memset(code, 0, sizeof(code));
    /* SPH header: type=1, ver=3, stage=1 (VertexB) */
    u32 sph0 = 1u | (3u << 5) | (1u << 10) | (1u << 14) | (1u << 26);
    memcpy(code, &sph0, 4);

    /* 0: ULDC.64 UR4, c[0][0x20] -> base9=0x0ab, form=3(Cbuf)->opc12=0x6ab, sz=5(.64) in hi[22:20] */
    make_amp_inst(code, 0, (3u << 9) | 0x0ab, 4, 255, 0x20u, 255, 0, 7, (5ULL << 20));
    /* 1: UIADD3 UR6, UR4, 4, URZ -> base9=0x090, form=2(Imm)->opc12=0x490, rd=6, ra=4, imm=4, rc=63(URZ) */
    make_amp_inst(code, 1, (2u << 9) | 0x090, 6, 4, 4u, 63, 0, 7, 0);
    /* 2: ALD.128 R0, a[0x80] -> base9=0x121, form=2(Imm)->opc12=0x521, rd=0, ra=255, imm=0x80, count=3(.128) in hi[21:20] */
    make_amp_inst(code, 2, (2u << 9) | 0x121, 0, 255, 0x80u, 255, 0, 7, (3ULL << 20));
    /* 3: FFMA R0, R0, UR4, UR5 -> base9=0x023, form=0(Reg)->opc12=0x023, rd=0, ra=0, rb=4(UR4), rc=5(UR5), hi[37]=rb_is_ur, hi[38]=rc_is_ur */
    make_amp_inst(code, 3, 0x023, 0, 0, 4u, 5, 0, 7, (1ULL << 37) | (1ULL << 38));
    /* 4: AST.128 a[0x70], R0 -> base9=0x122, form=2(Imm)->opc12=0x522, rd=0, ra=255, imm=0x70, count=3 in hi[21:20] */
    make_amp_inst(code, 4, (2u << 9) | 0x122, 0, 255, 0x70u, 255, 0, 7, (3ULL << 20));
    /* 5: AST.128 a[0x80], R0 -> base9=0x122, form=2(Imm)->opc12=0x522, rd=0, ra=255, imm=0x80, count=3 in hi[21:20] */
    make_amp_inst(code, 5, (2u << 9) | 0x122, 0, 255, 0x80u, 255, 0, 7, (3ULL << 20));
    /* 6: EXIT -> base9=0x14d */
    make_amp_inst(code, 6, 0x14d, 255, 255, 0, 255, 0, 7, 0);

    u32 hash_len = 0;
    u64 h = shader_hash(code, sizeof(code), &hash_len);
    printf("[TEST-VS] shader_hash=0x%016llx hash_len=%u bytes (expected %u)\n",
           (unsigned long long)h, hash_len, 0x50u + 7u * 16u);
    assert(hash_len == 0x50u + 7u * 16u);

    ShaderInfo si;
    memset(&si, 0, sizeof(si));
    si.clip_enable = 0xFFu;
    size_t words = 0;
    u32* spv = shader_to_spirv(code, sizeof(code), ST_VS, &si, &words, 0, 0, 0, 0);
    int ok = verify_spirv("Ampere-VS", spv, words, &si);
    free(spv);
    return ok;
}

/* ============================================================================
 * Test 2: Ampere SM86 Fragment Shader (FS) with Reconveyance (BSSY/BSYNC/BRA) & TEX
 *   0: IPA R0, a[0x80]                 (Interpolate generic input attr 0.x)
 *   1: IPA R1, a[0x84]                 (Interpolate generic input attr 0.y)
 *   2: ULDC UR2, c[0][0x10]            (Uniform load -> R192)
 *   3: UISETP.NE.AND UP0, UPT, UR2, URZ, UPT  (Uniform predicate set -> P6)
 *   4: BSSY B0, target=inst 7          (Warp reconvergence barrier -> SSY)
 *   5: @!UP0 BRA target=inst 7         (Conditional branch over inst 6)
 *   6: FADD R0, R0, R1                 (Conditional vector FADD)
 *   7: BSYNC B0                        (Reconvergence sync -> SYNC)
 *   8: TEX.2D R0..R3, R0..R1, tid=4    (2D texture sample into R0..R3)
 *   9: EXIT                            (Fragment exit writing RT0 R0..R3)
 * ============================================================================ */
static int test_ampere_fragment_shader(void) {
    printf("\n================================================================================\n");
    printf("TEST 2: Ampere SM86 128-bit Fragment Shader (BSSY/BSYNC/BRA + TEX) -> SPIR-V\n");
    printf("================================================================================\n");

    u8 code[4096];
    memset(code, 0, sizeof(code));
    /* SPH header: type=1, ver=3, stage=5 (Fragment), RT0=0xf in sph[18] */
    u32 sph[20];
    memset(sph, 0, sizeof(sph));
    sph[0]  = 1u | (3u << 5) | (5u << 10) | (1u << 14) | (1u << 26);
    sph[18] = 0xfu; /* RT0 RGBA enabled */
    memcpy(code, sph, sizeof(sph));

    /* 0: IPA R0, a[0x80] -> base9=0x126, form=2(Imm), rd=0, imm=0x80 */
    make_amp_inst(code, 0, (2u << 9) | 0x126, 0, 255, 0x80u, 255, 0, 7, 0);
    /* 1: IPA R1, a[0x84] -> base9=0x126, form=2(Imm), rd=1, imm=0x84 */
    make_amp_inst(code, 1, (2u << 9) | 0x126, 1, 255, 0x84u, 255, 0, 7, 0);
    /* 2: ULDC.32 UR2, c[0][0x10] -> base9=0x0ab, form=3(Cbuf), rd=2(UR2), imm=0x10, sz=4(.32) in hi[22:20] */
    make_amp_inst(code, 2, (3u << 9) | 0x0ab, 2, 255, 0x10u, 255, 0, 7, (4ULL << 20));
    /* 3: UISETP.NE.AND UP0, UPT, UR2, URZ, UPT -> base9=0x08c, form=0(Reg), ra=2(UR2), rb=63(URZ),
     *    hi[29:27]=pd0(0=UP0), hi[32:30]=pd1(7=UPT), hi[35:33]=ps0(7=UPT), hi[18:16]=cmp(5=NE) */
    make_amp_inst(code, 3, 0x08c, 0, 2, 63u, 255, 0, 7,
                  (0ULL << 27) | (7ULL << 30) | (7ULL << 33) | (5ULL << 16));
    /* 4: BSSY B0, target=inst 7 -> base9=0x145, rel_bytes = (7 - 5) * 16 = +32 */
    make_amp_inst(code, 4, (2u << 9) | 0x145, 0, 255, 32u, 255, 0, 7, 0);
    /* 5: @!UP0 BRA target=inst 7 -> base9=0x147, rel_bytes = (7 - 6) * 16 = +16, pred_inv=1, pred=0(UP0), hi[39]=pred_is_up */
    make_amp_inst(code, 5, (2u << 9) | 0x147, 255, 255, 16u, 255, 1, 0, (1ULL << 39));
    /* 6: FADD R0, R0, R1 -> base9=0x021, form=0(Reg), rd=0, ra=0, rb=1 */
    make_amp_inst(code, 6, 0x021, 0, 0, 1u, 255, 0, 7, 0);
    /* 7: BSYNC B0 -> base9=0x141 */
    make_amp_inst(code, 7, 0x141, 0, 255, 0, 255, 0, 7, 0);
    /* 8: TEX.2D R0..R3, R0..R1, tid=4 -> base9=0x160, rd=0, ra=0, rb=255, imm(tex_off)=4, hi[10:8]=1(2D), hi[15:12]=0xf(RGBA) */
    make_amp_inst(code, 8, (2u << 9) | 0x160, 0, 0, 4u, 255, 0, 7, (1ULL << 8) | (0xFULL << 12));
    /* 9: EXIT -> base9=0x14d */
    make_amp_inst(code, 9, 0x14d, 255, 255, 0, 255, 0, 7, 0);

    ShaderInfo si;
    memset(&si, 0, sizeof(si));
    size_t words = 0;
    /* Pass can_subgroup=1 for fragment derivative reconvergence across BSSY/BSYNC/BRA */
    u32* spv = shader_to_spirv(code, sizeof(code), ST_FS, &si, &words, 0, 0, 0, 1);
    int ok = verify_spirv("Ampere-FS", spv, words, &si);
    free(spv);
    return ok;
}

/* ============================================================================
 * Test 3: Ampere SM86 Compute Shader (CS) with 1-to-2 Micro-Op Expansions
 *   0: S2UR UR0, SR_TID.X (33)         (Special reg to UR0 -> S2R R192, SR_TID.X)
 *   1: ULDC.64 UR4, c[0][0x110]        (Load 64-bit SSBO descriptor into UR4..UR5 -> R193..R194)
 *   2: IMAD.WIDE.U32 R2, UR0, 4, UR4   (1-to-2 expansion -> IMAD R2 + IMAD.HI R3 for 64-bit address!)
 *   3: LDGSTS.E.32 [UR0], [R2]         (1-to-2 async copy -> LDG.E R_scratch, [R2] + STS [R192], R_scratch)
 *   4: BAR.SYNC 0                      (Workgroup barrier -> BAR.SYNC 0)
 *   5: LDS.U32 R4, [UR0]               (Load from shared memory into R4)
 *   6: IADD3 R4, R4, 1, RZ             (Increment loaded value)
 *   7: STG.E.32 [R2], R4               (Store back to global SSBO memory)
 *   8: EXIT                            (Terminate compute shader)
 * ============================================================================ */
static int test_ampere_compute_shader(void) {
    printf("\n================================================================================\n");
    printf("TEST 3: Ampere SM86 128-bit Compute Shader (S2UR + IMAD.WIDE + LDGSTS) -> SPIR-V\n");
    printf("================================================================================\n");

    u8 code[4096];
    memset(code, 0, sizeof(code));
    u32 sph0 = 1u | (3u << 5) | (5u << 10) | (1u << 14) | (1u << 26);
    memcpy(code, &sph0, 4);

    /* 0: S2UR UR0, SR_TID.X (33) -> base9=0x1c9, rd=0(UR0), hi[15:8]=33 */
    make_amp_inst(code, 0, 0x1c9, 0, 255, 33u, 255, 0, 7, (33ULL << 8));
    /* 1: ULDC.64 UR4, c[0][0x110] -> base9=0x0ab, form=3(Cbuf), rd=4(UR4), ra=255, off=0x110, sz=5(.64) in hi[22:20] */
    make_amp_inst(code, 1, (3u << 9) | 0x0ab, 4, 255, 0x110u, 255, 0, 7, (5ULL << 20));
    /* 2: IMAD.WIDE.U32 R2, UR0, 4, UR4 -> base9=0x025, form=2(Imm), rd=2, ra=0(UR0), imm=4, rc=4(UR4),
     *    hi[36]=ra_is_ur, hi[38]=rc_is_ur */
    make_amp_inst(code, 2, (2u << 9) | 0x025, 2, 0, 4u, 4, 0, 7, (1ULL << 36) | (1ULL << 38));
    /* 3: LDGSTS.E.32 [UR0], [R2] -> base9=0x1ae, ra=0(UR0 shared offset), rb=2(R2 64-bit global addr), hi[36]=ra_is_ur */
    make_amp_inst(code, 3, 0x1ae, 255, 0, 2u, 255, 0, 7, (1ULL << 36));
    /* 4: BAR.SYNC 0 -> base9=0x11d */
    make_amp_inst(code, 4, 0x11d, 255, 255, 0, 255, 0, 7, 0);
    /* 5: LDS.U32 R4, [UR0] -> base9=0x184, rd=4, ra=0(UR0), sz=4(.32) in hi[22:20], hi[36]=ra_is_ur */
    make_amp_inst(code, 5, 0x184, 4, 0, 0, 255, 0, 7, (4ULL << 20) | (1ULL << 36));
    /* 6: IADD3 R4, R4, 1, RZ -> base9=0x010, form=2(Imm), rd=4, ra=4, imm=1, rc=255 */
    make_amp_inst(code, 6, (2u << 9) | 0x010, 4, 4, 1u, 255, 0, 7, 0);
    /* 7: STG.E.32 [R2], R4 -> base9=0x186, rd=4(data), ra=2(64-bit addr), hi[16]=1(.E 64-bit), hi[22:20]=4(.U32) */
    make_amp_inst(code, 7, 0x186, 4, 2, 0, 255, 0, 7, (1ULL << 16) | (4ULL << 20));
    /* 8: EXIT -> base9=0x14d */
    make_amp_inst(code, 8, 0x14d, 255, 255, 0, 255, 0, 7, 0);

    g_shader_cb_read = test_cb_read;
    ShaderInfo si;
    memset(&si, 0, sizeof(si));
    si.cs_local[0] = 64;
    si.cs_local[1] = 1;
    si.cs_local[2] = 1;
    si.shared_size = 1024;
    size_t words = 0;
    u32* spv = shader_to_spirv(code, sizeof(code), ST_CS, &si, &words, 0, 0, 0, 0);
    int ok = verify_spirv("Ampere-CS", spv, words, &si);
    free(spv);
    return ok;
}

/* ============================================================================
 * Test 4: Ampere SM86 Dual Vertex (VertexA + VertexB) Merge -> SPIR-V
 * ============================================================================ */
static int test_ampere_dual_vertex_merge(void) {
    printf("\n================================================================================\n");
    printf("TEST 4: Ampere SM86 Dual Vertex (VertexA + VertexB) Merge -> SPIR-V\n");
    printf("================================================================================\n");

    u8 va[4096], vb[4096], merged[4096];
    memset(va, 0, sizeof(va));
    memset(vb, 0, sizeof(vb));
    memset(merged, 0, sizeof(merged));

    /* VertexA: stage=0 (VertexA) in SPH, loads generic attr 0 into R0..R3 and EXITs */
    u32 sph_a = 1u | (3u << 5) | (0u << 10) | (1u << 14) | (1u << 26);
    memcpy(va, &sph_a, 4);
    make_amp_inst(va, 0, (2u << 9) | 0x121, 0, 255, 0x80u, 255, 0, 7, (3ULL << 20)); /* ALD.128 R0, a[0x80] */
    make_amp_inst(va, 1, 0x14d, 255, 255, 0, 255, 0, 7, 0);                           /* EXIT */

    /* VertexB: stage=1 (VertexB) in SPH, stores R0..R3 to Position a[0x70] and EXITs */
    u32 sph_b = 1u | (3u << 5) | (1u << 10) | (1u << 14) | (1u << 26);
    memcpy(vb, &sph_b, 4);
    make_amp_inst(vb, 0, (2u << 9) | 0x122, 0, 255, 0x70u, 255, 0, 7, (3ULL << 20)); /* AST.128 a[0x70], R0 */
    make_amp_inst(vb, 1, 0x14d, 255, 255, 0, 255, 0, 7, 0);                           /* EXIT */

    int m_ok = shader_merge_dual(va, vb, merged, sizeof(merged));
    printf("[TEST-DUAL-VS] shader_merge_dual returned %d\n", m_ok);
    if (!m_ok) return 0;

    ShaderInfo si;
    memset(&si, 0, sizeof(si));
    si.clip_enable = 0xFFu;
    size_t words = 0;
    u32* spv = shader_to_spirv(merged, sizeof(merged), ST_VS, &si, &words, 0, 0, 0, 0);
    int ok = verify_spirv("Ampere-Dual-VS", spv, words, &si);
    free(spv);
    return ok;
}

int main(void) {
    ampere_set_log_level(2);
    printf("=== SwitchRecomp Ampere (SM86) -> Maxwell IR (MInst) -> SPIR-V Test Suite ===\n");
    printf("sizeof(ShaderInfo) = %zu bytes\n", sizeof(ShaderInfo));

    int ok_vs   = test_ampere_vertex_shader();
    int ok_fs   = test_ampere_fragment_shader();
    int ok_cs   = test_ampere_compute_shader();
    int ok_dual = test_ampere_dual_vertex_merge();

    printf("\n================================================================================\n");
    printf("SUMMARY: VS=%s | FS=%s | CS=%s | DUAL_VS=%s\n",
           ok_vs   ? "PASS" : "FAIL",
           ok_fs   ? "PASS" : "FAIL",
           ok_cs   ? "PASS" : "FAIL",
           ok_dual ? "PASS" : "FAIL");
    printf("================================================================================\n");

    return (ok_vs && ok_fs && ok_cs && ok_dual) ? 0 : 1;
}
