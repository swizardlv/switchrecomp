// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 SwitchRecomp Contributors
//
// Ampere (SM86 / GA10B 128-bit SASS) -> Maxwell IR (MInst) -> Canonical Maxwell 64-bit Stream
// Frontend Lowering & Diagnostic Tracing Pipeline

#pragma once

#include <stdbool.h>
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/shader/translator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int OpId;
typedef int ShaderStage;
typedef i32 s32;

/* Maximum number of Ampere 128-bit instructions and lowered Maxwell MInsts */
#define AMPERE_MAX_INSTS       4096
#define AMPERE_MAX_MINSTS      8192
#define AMPERE_MAX_OUT_WORDS   12288

/* Normalized Maxwell-level IR instruction (MInst) */
typedef struct MInst {
    OpId        op;               /* Target Maxwell OpId (from maxwell_opcodes.h) */
    u64         word;             /* Synthesized 64-bit Maxwell instruction word */
    u32         orig_idx;         /* Index of source Ampere 128-bit instruction */
    u32         orig_pc;          /* Byte offset of source Ampere instruction from code start */
    u16         amp_opc;          /* Original 12-bit Ampere opcode (lo[11:0]) */
    u8          sub_op;           /* 0 = primary op, 1 = expanded micro-op #2 (e.g. IMAD.WIDE hi, LDGSTS STS) */
    u8          is_pc_rel_branch; /* Non-zero if word[39:20] holds a signed PC target needing word-index fixup */
    s32         target_byte_pc;   /* Target byte PC relative to code start (after SPH) for branch fixup */
    u8          rd;               /* Destination GPR (after UR->R remapping) */
    u8          ra;               /* Source A GPR (after UR->R remapping) */
    u8          rb;               /* Source B GPR (after UR->R remapping) */
    u8          rc;               /* Source C GPR (after UR->R remapping) */
    u8          pred;             /* Guard predicate P0..P7 (after UP->P remapping) */
    u8          pred_inv;         /* Guard predicate negated */
    u8          ur_remapped_mask; /* Bitmask of operands that were remapped from UR -> R */
    u8          up_remapped_mask; /* Bitmask of predicates that were remapped from UP -> P */
    const char* mnemonic;         /* Human-readable Ampere mnemonic for trace logging */
    char        note[64];         /* Lowering note (e.g., "UR4->R192", "IMAD.WIDE.LO", "BSSY->SSY") */
} MInst;

/* Comprehensive statistics & diagnostic report for an Ampere -> Maxwell lowering pass */
typedef struct AmpereLowerStats {
    bool        is_ampere;            /* True if input was detected as Ampere SM86 128-bit SASS */
    bool        has_sph;              /* True if shader begins with an 80-byte SPH header */
    u32         sph_version;          /* SPH header version field */
    u32         code_offset;          /* Byte offset where 128-bit instruction stream starts (80 or 0) */
    u32         ampere_inst_count;    /* Number of 128-bit Ampere instructions decoded */
    u32         minst_count;          /* Number of MInst IR nodes after micro-op expansion */
    u32         maxwell_word_count;   /* Total 64-bit words in synthesized Maxwell stream (incl. SPH & sched) */
    u32         maxwell_byte_count;   /* Total bytes in synthesized Maxwell stream */
    u32         vec_reg_count;        /* Number of R0..R254 vector GPRs used natively */
    u32         uni_reg_count;        /* Number of UR0..UR62 uniform GPRs used and remapped */
    u32         uni_reg_spilled;      /* Number of UR registers spilled to local memory (when R pressure > 254) */
    u32         uni_pred_count;       /* Number of UP0..UP6 uniform predicates used and remapped */
    u32         expanded_micro_ops;   /* Count of 1-to-2 micro-op expansions (IMAD.WIDE, LDGSTS, BMSK) */
    u32         branch_fixups;        /* Count of PC-relative branches remapped to Maxwell word offsets */
    u32         unknown_opcodes;      /* Count of unrecognized 128-bit instructions lowered to NOP */
    u32         unsupported_emitted;  /* Count of emitted Maxwell words failing supported(w, op->id) verify */
    u8          ur_to_r[64];          /* Mapping table: UR[i] -> R[ur_to_r[i]] (255 = unused/RZ) */
    u8          up_to_p[8];           /* Mapping table: UP[i] -> P[up_to_p[i]] (7 = PT) */
} AmpereLowerStats;

/* Detect whether a raw shader binary is Ampere (SM86 128-bit SASS) rather than Maxwell (SM53 64-bit SASS).
 * Logs detection heuristics when trace logging is enabled. */
bool ampere_is_sm86_shader(const u8* code, u32 max_bytes, ShaderStage stage);

/* Lower an Ampere SM86 128-bit shader into:
 *   1) An explicit MInst[] IR array (optional out_minsts if non-NULL), and
 *   2) A newly heap-allocated canonical Maxwell 64-bit instruction stream (80-byte SPH + 1-in-4 sched words)
 *      that can be fed directly into shader_to_spirv(), shader_compute_to_spirv(), shader_hash(), or shader_to_glsl().
 *
 * Returns a malloc'd buffer containing the canonical Maxwell shader stream (caller must free()),
 * or NULL if the input is not an Ampere shader or allocation fails. */
u8* ampere_lower_to_maxwell(const u8* code,
                            u32 max_bytes,
                            ShaderStage stage,
                            u32* out_max_bytes,
                            AmpereLowerStats* out_stats,
                            MInst* out_minsts,
                            u32 max_out_minsts);

/* Set or query the Ampere lowering diagnostic log level:
 *   0 = silent (errors only)
 *   1 = summary per shader (detection, register map, lowering stats, verification)
 *   2 = full trace (logs every decoded 128-bit Ampere instruction, MInst expansion, and emitted 64-bit Maxwell word)
 */
void ampere_set_log_level(int level);
int  ampere_get_log_level(void);

#ifdef __cplusplus
}
#endif
