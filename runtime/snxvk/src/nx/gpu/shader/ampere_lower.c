// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 SwitchRecomp Contributors
//
// Ampere (SM86 / GA10B 128-bit SASS) -> Maxwell IR (MInst) -> Canonical Maxwell 64-bit Stream
// Frontend Lowering & Diagnostic Tracing Pipeline

#include "ampere_lower.h"
#include "nx/runtime/platform/log.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Default log level: 2 (full trace) as requested for diagnostic visibility */
static int g_ampere_log_level = -1;

static int get_log_level(void) {
    if (g_ampere_log_level >= 0) return g_ampere_log_level;
    const char* env = getenv("SWR_AMPERE_LOG");
    if (env && *env) {
        g_ampere_log_level = atoi(env);
    } else {
        g_ampere_log_level = 2; /* Default to verbose trace logging */
    }
    return g_ampere_log_level;
}

void ampere_set_log_level(int level) {
    g_ampere_log_level = level;
}

int ampere_get_log_level(void) {
    return get_log_level();
}

#define AMP_LOG(fmt, ...)                                                       \
    do {                                                                        \
        if (get_log_level() >= 1) {                                             \
            LOG("[AMPERE-LOWER] " fmt, ##__VA_ARGS__);                          \
        }                                                                       \
    } while (0)

#define AMP_TRACE(fmt, ...)                                                     \
    do {                                                                        \
        if (get_log_level() >= 2) {                                             \
            LOG("[AMPERE-TRACE] " fmt, ##__VA_ARGS__);                          \
        }                                                                       \
    } while (0)

#define AMP_WARN(fmt, ...)                                                      \
    do {                                                                        \
        LOG("[AMPERE-WARN] " fmt, ##__VA_ARGS__);                               \
    } while (0)

static const char* stage_name(ShaderStage stage) {
    switch (stage) {
        case ST_VS:  return "VS";
        case ST_TCS: return "TCS";
        case ST_TES: return "TES";
        case ST_GS:  return "GS";
        case ST_FS:  return "FS";
        case ST_CS:  return "CS";
        default:     return "UNK";
    }
}

/* ============================================================================
 * 1. Ampere (SM86) 12-bit Base Opcodes & Form Classification
 * ============================================================================
 * In SM86 (GA10B / Orin), each instruction is 128 bits (16 bytes):
 *   - lo (64-bit):
 *       bits [11:0]  = 12-bit opcode (low 9 bits [8:0] = base opcode, bits [11:9] = form:
 *                      000/001 = Reg3, 010 = Imm32, 011 = Cbuf, 100 = Reg2+Imm, 101 = Reg2+Cbuf)
 *       bits [15:12] = Guard Predicate (P0..P7, bit 15 = negate; or UP0..UP7 for uniform ops)
 *       bits [23:16] = Destination Register Rd (or URd for uniform ops)
 *       bits [31:24] = Source Register Ra (or URa for uniform ops)
 *       bits [63:32] = Immediate32 / Cbuf [bank, offset] / Source Register Rb (in [39:32])
 *   - hi (64-bit):
 *       bits [7:0]   = Source Register Rc (or URc) / extra operand
 *       bits [15:8]  = LUT8 (for LOP3/ULOP3) / subop / modifiers
 *       bits [26:16] = Instruction modifiers (FTZ, SAT, X, CC, HI, CMP, S32, etc.)
 *       bits [39:27] = Predicate destinations (Pd0, Pd1, Ps0) & Uniform source flags
 *       bits [63:41] = SM86 Warp Scheduler Control (stall[44:41], yield[45], wr_bar[48:46],
 *                      rd_bar[51:49], wait_mask[57:52], reuse[61:58])
 * ============================================================================ */

enum AmpereBaseOp {
    AMP_OP_NOP       = 0x118,
    AMP_OP_MOV       = 0x002,
    AMP_OP_UMOV      = 0x082,
    AMP_OP_R2UR      = 0x0c2,
    AMP_OP_S2R       = 0x119,
    AMP_OP_S2UR      = 0x1c9,
    AMP_OP_CS2R      = 0x005,
    AMP_OP_IADD3     = 0x010,
    AMP_OP_UIADD3    = 0x090,
    AMP_OP_IMAD      = 0x024,
    AMP_OP_IMAD_WIDE = 0x025,
    AMP_OP_UIMAD     = 0x0a4,
    AMP_OP_LOP3      = 0x012,
    AMP_OP_ULOP3     = 0x092,
    AMP_OP_SHF       = 0x019,
    AMP_OP_USHF      = 0x099,
    AMP_OP_LEA       = 0x011,
    AMP_OP_ULEA      = 0x091,
    AMP_OP_ISETP     = 0x00c,
    AMP_OP_UISETP    = 0x08c,
    AMP_OP_SEL       = 0x007,
    AMP_OP_USEL      = 0x087,
    AMP_OP_BFE       = 0x100,
    AMP_OP_UBFE      = 0x180,
    AMP_OP_FLO       = 0x100, /* shared base in some forms, distinguished by subop */
    AMP_OP_POPC      = 0x108,
    AMP_OP_UPOPC     = 0x109,
    AMP_OP_PRMT      = 0x016,
    AMP_OP_UPRMT     = 0x096,
    AMP_OP_SGXT      = 0x01a,
    AMP_OP_USGXT     = 0x09a,
    AMP_OP_BMSK      = 0x01b,
    AMP_OP_UBMSK     = 0x09b,
    AMP_OP_IABS      = 0x013,
    AMP_OP_IMNMX     = 0x017,
    /* Floating point */
    AMP_OP_FADD      = 0x021,
    AMP_OP_FMUL      = 0x020,
    AMP_OP_FFMA      = 0x023,
    AMP_OP_FMNMX     = 0x009,
    AMP_OP_FSETP     = 0x00b,
    AMP_OP_FSEL      = 0x008,
    AMP_OP_MUFU      = 0x108, /* distinguished by form */
    AMP_OP_FRND      = 0x104,
    AMP_OP_F2F       = 0x110,
    AMP_OP_F2I       = 0x111,
    AMP_OP_I2F       = 0x112,
    AMP_OP_I2I       = 0x113,
    /* Half & Double */
    AMP_OP_HADD2     = 0x030,
    AMP_OP_HMUL2     = 0x032,
    AMP_OP_HFMA2     = 0x031,
    AMP_OP_HSETP2    = 0x034,
    AMP_OP_DADD      = 0x029,
    AMP_OP_DMUL      = 0x028,
    AMP_OP_DFMA      = 0x02b,
    AMP_OP_DSETP     = 0x02a,
    /* Predicate ops */
    AMP_OP_PLOP3     = 0x11c,
    AMP_OP_UPLOP3    = 0x19c,
    AMP_OP_P2R       = 0x003,
    AMP_OP_R2P       = 0x004,
    /* Memory & Attributes */
    AMP_OP_LDC       = 0x182,
    AMP_OP_ULDC      = 0x0ab,
    AMP_OP_LDG       = 0x181,
    AMP_OP_STG       = 0x186,
    AMP_OP_LDS       = 0x184,
    AMP_OP_STS       = 0x188,
    AMP_OP_LDL       = 0x183,
    AMP_OP_STL       = 0x187,
    AMP_OP_LDGSTS    = 0x1ae,
    AMP_OP_ATOM      = 0x18a,
    AMP_OP_ATOMG     = 0x18b,
    AMP_OP_ATOMS     = 0x18c,
    AMP_OP_RED       = 0x18e,
    AMP_OP_ALD       = 0x121,
    AMP_OP_AST       = 0x122,
    AMP_OP_OUT       = 0x124,
    AMP_OP_IPA       = 0x126,
    AMP_OP_ISBERD    = 0x123,
    AMP_OP_PIXLD     = 0x125,
    /* Texture & Surface */
    AMP_OP_TEX       = 0x160,
    AMP_OP_TLD       = 0x166,
    AMP_OP_TLD4      = 0x163,
    AMP_OP_TMML      = 0x169,
    AMP_OP_TXD       = 0x16c,
    AMP_OP_TXQ       = 0x16f,
    AMP_OP_SULD      = 0x190,
    AMP_OP_SUST      = 0x192,
    AMP_OP_SUATOM    = 0x194,
    AMP_OP_SURED     = 0x196,
    /* Control Flow & Synchronization */
    AMP_OP_BRA       = 0x147,
    AMP_OP_BRX       = 0x149,
    AMP_OP_BRXU      = 0x14a,
    AMP_OP_BSSY      = 0x145,
    AMP_OP_BSYNC     = 0x141,
    AMP_OP_BMOV      = 0x155,
    AMP_OP_BREAK     = 0x142,
    AMP_OP_CONT      = 0x143,
    AMP_OP_EXIT      = 0x14d,
    AMP_OP_KIL       = 0x15b,
    AMP_OP_RET       = 0x150,
    AMP_OP_CAL       = 0x14e,
    AMP_OP_YIELD     = 0x146,
    AMP_OP_WARPSYNC  = 0x148,
    AMP_OP_BAR       = 0x11d,
    AMP_OP_MEMBAR    = 0x198,
    AMP_OP_DEPBAR    = 0x11a,
    AMP_OP_LDGDEPBAR = 0x1af,
    /* Subgroup */
    AMP_OP_VOTE      = 0x106,
    AMP_OP_SHFL      = 0x18f,
    AMP_OP_REDUX     = 0x1c4,
    AMP_OP_FSWZADD   = 0x10a
};

/* Form bits in lo[11:9]:
 *   0, 1: Register-Register (SrcB in lo[39:32], SrcC in hi[7:0])
 *   2, 4: Immediate32 (Imm32 in lo[63:32], SrcC in hi[7:0])
 *   3, 5: Constant Buffer (cbuf offset in lo[49:32], bank in lo[54:50], SrcC in hi[7:0])
 *   6:    SrcB in hi[7:0], Imm32 in lo[63:32]
 *   7:    SrcB in hi[7:0], Cbuf in lo[54:32]
 */
typedef enum {
    AMP_FORM_REG  = 0,
    AMP_FORM_IMM  = 1,
    AMP_FORM_CBUF = 2,
    AMP_FORM_RC   = 3, /* SrcB = Reg, SrcC = Cbuf */
    AMP_FORM_CR   = 4  /* SrcB = Cbuf, SrcC = Reg */
} AmpereForm;

static AmpereForm classify_ampere_form(u16 opc12) {
    u32 top3 = (opc12 >> 9) & 7;
    switch (top3) {
        case 0:
        case 1: return AMP_FORM_REG;
        case 2:
        case 4: return AMP_FORM_IMM;
        case 3:
        case 5: return AMP_FORM_CBUF;
        case 6: return AMP_FORM_IMM;
        case 7: return AMP_FORM_RC;
        default: return AMP_FORM_REG;
    }
}

static bool is_known_ampere_base_op(u16 base9) {
    switch (base9) {
        case AMP_OP_NOP:
        case AMP_OP_MOV:
        case AMP_OP_UMOV:
        case AMP_OP_R2UR:
        case AMP_OP_S2R:
        case AMP_OP_S2UR:
        case AMP_OP_CS2R:
        case AMP_OP_IADD3:
        case AMP_OP_UIADD3:
        case AMP_OP_IMAD:
        case AMP_OP_IMAD_WIDE:
        case AMP_OP_UIMAD:
        case AMP_OP_LOP3:
        case AMP_OP_ULOP3:
        case AMP_OP_SHF:
        case AMP_OP_USHF:
        case AMP_OP_LEA:
        case AMP_OP_ULEA:
        case AMP_OP_ISETP:
        case AMP_OP_UISETP:
        case AMP_OP_SEL:
        case AMP_OP_USEL:
        case AMP_OP_BFE:
        case AMP_OP_UBFE:
        case AMP_OP_POPC:
        case AMP_OP_UPOPC:
        case AMP_OP_PRMT:
        case AMP_OP_UPRMT:
        case AMP_OP_SGXT:
        case AMP_OP_USGXT:
        case AMP_OP_BMSK:
        case AMP_OP_UBMSK:
        case AMP_OP_IABS:
        case AMP_OP_IMNMX:
        case AMP_OP_FADD:
        case AMP_OP_FMUL:
        case AMP_OP_FFMA:
        case AMP_OP_FMNMX:
        case AMP_OP_FSETP:
        case AMP_OP_FSEL:
        case AMP_OP_FRND:
        case AMP_OP_F2F:
        case AMP_OP_F2I:
        case AMP_OP_I2F:
        case AMP_OP_I2I:
        case AMP_OP_HADD2:
        case AMP_OP_HMUL2:
        case AMP_OP_HFMA2:
        case AMP_OP_HSETP2:
        case AMP_OP_DADD:
        case AMP_OP_DMUL:
        case AMP_OP_DFMA:
        case AMP_OP_DSETP:
        case AMP_OP_PLOP3:
        case AMP_OP_UPLOP3:
        case AMP_OP_P2R:
        case AMP_OP_R2P:
        case AMP_OP_LDC:
        case AMP_OP_ULDC:
        case AMP_OP_LDG:
        case AMP_OP_STG:
        case AMP_OP_LDS:
        case AMP_OP_STS:
        case AMP_OP_LDL:
        case AMP_OP_STL:
        case AMP_OP_LDGSTS:
        case AMP_OP_ATOM:
        case AMP_OP_ATOMG:
        case AMP_OP_ATOMS:
        case AMP_OP_RED:
        case AMP_OP_ALD:
        case AMP_OP_AST:
        case AMP_OP_OUT:
        case AMP_OP_IPA:
        case AMP_OP_ISBERD:
        case AMP_OP_PIXLD:
        case AMP_OP_TEX:
        case AMP_OP_TLD:
        case AMP_OP_TLD4:
        case AMP_OP_TMML:
        case AMP_OP_TXD:
        case AMP_OP_TXQ:
        case AMP_OP_SULD:
        case AMP_OP_SUST:
        case AMP_OP_SUATOM:
        case AMP_OP_SURED:
        case AMP_OP_BRA:
        case AMP_OP_BRX:
        case AMP_OP_BRXU:
        case AMP_OP_BSSY:
        case AMP_OP_BSYNC:
        case AMP_OP_BMOV:
        case AMP_OP_BREAK:
        case AMP_OP_CONT:
        case AMP_OP_EXIT:
        case AMP_OP_KIL:
        case AMP_OP_RET:
        case AMP_OP_CAL:
        case AMP_OP_YIELD:
        case AMP_OP_WARPSYNC:
        case AMP_OP_BAR:
        case AMP_OP_MEMBAR:
        case AMP_OP_DEPBAR:
        case AMP_OP_LDGDEPBAR:
        case AMP_OP_VOTE:
        case AMP_OP_SHFL:
        case AMP_OP_REDUX:
        case AMP_OP_FSWZADD:
            return true;
        default:
            return false;
    }
}

/* ============================================================================
 * 2. Architecture Detection: Ampere (SM86 128-bit) vs Maxwell (SM53 64-bit)
 * ============================================================================ */

bool ampere_is_sm86_shader(const u8* code, u32 max_bytes, ShaderStage stage) {
    if (!code || max_bytes < 96) return false;

    const u32* w32 = (const u32*)code;
    u32 sph0 = snx_ld_le32(&w32[0]);
    u32 sph_type = sph0 & 0x1fu;
    u32 sph_ver  = (sph0 >> 5) & 0x1fu;
    u32 sph_stg  = (sph0 >> 10) & 0xfu;

    /* Explicit Ampere marker magic if tagged by offline tools or SPH v4+ */
    if (sph0 == 0x36384d53u /* 'SM86' */ || sph_ver >= 4) {
        AMP_LOG("[AMPERE-DETECT] Explicit SM86 SPH marker detected (sph0=0x%08x ver=%u stage=%s)",
                sph0, sph_ver, stage_name(stage));
        return true;
    }

    /* Determine whether instructions start at byte 80 (after SPH) or byte 0 */
    bool has_valid_sph = (sph_type <= 2 && sph_stg <= 6 && (sph0 != 0));
    u32 code_off = has_valid_sph ? 80 : 0;
    if (max_bytes < code_off + 32) return false;

    /* Check Maxwell validity first:
     * In a genuine Maxwell shader (starting at word 10 = byte 80):
     *   - Word 10, 14, 18, 22... are 64-bit scheduling control words (top bits 63:60 are 0x0,
     *     and bits [20:0] have non-zero stall/yield fields like 0x001f8000fc0007e0).
     *   - Words 11..13, 15..17... decode as valid Maxwell opcodes in decode(w), ending at EXIT.
     */
    if (code_off == 80) {
        u32 maxwell_valid_ops = 0;
        u32 maxwell_invalid_ops = 0;
        bool maxwell_saw_exit = false;
        u32 max_words = max_bytes / 8;
        u32 scan_limit = max_words < 74 ? max_words : 74;

        for (u32 i = 10; i < scan_limit; i++) {
            u64 w = snx_ld_le64(code + i * 8);
            if ((i - 10) % 4 == 0) {
                /* Maxwell scheduling word slot */
                (void)w;
            } else {
                const Op* op = decode(w);
                if (op) {
                    maxwell_valid_ops++;
                    if (op->id == O_EXIT && flow_always(w)) {
                        maxwell_saw_exit = true;
                        break;
                    }
                } else {
                    maxwell_invalid_ops++;
                }
            }
        }

        /* If every Maxwell instruction slot decoded cleanly and scheduling words look like SM53,
         * check whether the stream is actually 128-bit Ampere instructions whose high 64-bit words
         * carry SM86 scheduler bits (stall in hi[44:41], barrier wait in hi[57:52], yield in hi[45]). */
        u32 sm86_valid_pairs = 0;
        u32 sm86_sched_hi_matches = 0;
        bool sm86_saw_exit = false;
        u32 pair_limit = (max_bytes - code_off) / 16;
        if (pair_limit > 32) pair_limit = 32;

        for (u32 p = 0; p < pair_limit; p++) {
            u64 lo = snx_ld_le64(code + code_off + p * 16);
            u64 hi = snx_ld_le64(code + code_off + p * 16 + 8);
            u16 opc12 = (u16)(lo & 0xfffu);
            u16 base9 = (u16)(opc12 & 0x1ffu);
            u32 stall = (u32)((hi >> 41) & 0xfu);

            if (is_known_ampere_base_op(base9)) {
                sm86_valid_pairs++;
            }
            /* In SM86, every 128-bit instruction's hi word has stall count in [44:41] (1..15)
             * and bits [63:62] == 0, whereas in Maxwell word 10 is a scheduling word and word 11 is an instruction! */
            if (stall >= 1 && (hi >> 62) == 0) {
                sm86_sched_hi_matches++;
            }
            if (base9 == AMP_OP_EXIT) {
                sm86_saw_exit = true;
                break;
            }
        }

        /* If word 10 (first 64-bit word after SPH) has a valid SM86 12-bit opcode in lo[11:0]
         * AND word 11 (its hi word) has SM86 scheduler bits while Maxwell decoding had invalid ops
         * or word 10 is NOT a Maxwell scheduling word (e.g. lo[11:0] is an SM86 opcode with Rd/Ra),
         * then it is an Ampere SM86 shader! */
        u64 w10 = snx_ld_le64(code + 80);
        u64 w11 = snx_ld_le64(code + 88);
        u16 w10_base9 = (u16)(w10 & 0x1ffu);
        u32 w11_stall = (u32)((w11 >> 41) & 0xfu);

        bool w10_is_maxwell_sched = ((w10 >> 56) == 0 && (w10 & 0xfffu) > 0 && !is_known_ampere_base_op(w10_base9));
        if (!w10_is_maxwell_sched && is_known_ampere_base_op(w10_base9) && w11_stall >= 1 &&
            sm86_valid_pairs >= 1 && (maxwell_invalid_ops > 0 || sm86_saw_exit)) {
            AMP_LOG("[AMPERE-DETECT] Detected SM86 128-bit shader (stage=%s, code_off=%u, sm86_pairs=%u, "
                    "sm86_sched=%u, maxwell_invalid=%u, w10=0x%016" PRIx64 " w11=0x%016" PRIx64 ")",
                    stage_name(stage), code_off, sm86_valid_pairs, sm86_sched_hi_matches,
                    maxwell_invalid_ops, w10, w11);
            return true;
        }

        if (maxwell_valid_ops > 0 && maxwell_invalid_ops == 0 && maxwell_saw_exit) {
            return false;
        }
    }

    return false;
}

/* ============================================================================
 * 3. Uniform Register (UR0..UR63) & Uniform Predicate (UP0..UP7) Remapper
 * ============================================================================
 * Scans all 128-bit Ampere instructions before lowering to build:
 *   1. A bitmap of used vector GPRs (R0..R254) and vector predicates (P0..P6).
 *   2. A bitmap of used uniform GPRs (UR0..UR62; UR63 = URZ) and uniform predicates (UP0..UP6; UP7 = UPT).
 *   3. A conflict-free static mapping:
 *        UR[u] -> R[ur_to_r[u]]   (allocated from highest unused R down from R254)
 *        UP[p] -> P[up_to_p[p]]   (allocated from highest unused P down from P6)
 *      If vector register pressure is so high that no free R exists for a UR, it logs a warning
 *      and assigns a dedicated spill scratch register R254 with local memory spill/reload.
 * ============================================================================ */

static void mark_r_used(u8 r_used[256], u32 reg, u32 count) {
    for (u32 k = 0; k < count; k++) {
        if (reg + k < 255) r_used[reg + k] = 1;
    }
}

static void mark_ur_used(u8 ur_used[64], u32 ureg, u32 count) {
    for (u32 k = 0; k < count; k++) {
        if (ureg + k < 63) ur_used[ureg + k] = 1;
    }
}

/* Uniform instruction family check */
static bool is_uniform_ampere_op(u16 base9) {
    switch (base9) {
        case AMP_OP_UMOV:
        case AMP_OP_R2UR:
        case AMP_OP_S2UR:
        case AMP_OP_UIADD3:
        case AMP_OP_UIMAD:
        case AMP_OP_ULOP3:
        case AMP_OP_USHF:
        case AMP_OP_ULEA:
        case AMP_OP_UISETP:
        case AMP_OP_USEL:
        case AMP_OP_UBFE:
        case AMP_OP_UPOPC:
        case AMP_OP_UPRMT:
        case AMP_OP_USGXT:
        case AMP_OP_UBMSK:
        case AMP_OP_UPLOP3:
        case AMP_OP_ULDC:
        case AMP_OP_BRXU:
        case AMP_OP_REDUX:
            return true;
        default:
            return false;
    }
}

static u32 scan_and_allocate_registers(const u8* insn_bytes,
                                       u32 max_insts,
                                       ShaderStage stage,
                                       const u32 sph[20],
                                       AmpereLowerStats* stats) {
    u8 r_used[256];
    u8 ur_used[64];
    u8 p_used[8];
    u8 up_used[8];
    memset(r_used, 0, sizeof(r_used));
    memset(ur_used, 0, sizeof(ur_used));
    memset(p_used, 0, sizeof(p_used));
    memset(up_used, 0, sizeof(up_used));

    /* Reserve fragment shader color/depth output registers (R0..R31) */
    if (stage == ST_FS && sph) {
        u32 target = sph[18];
        u32 out_reg = 0;
        for (int rt = 0; rt < 8; rt++) {
            if ((target >> (rt * 4)) & 15) {
                mark_r_used(r_used, out_reg, 4);
                out_reg += 4;
            }
        }
        if (sph[19] & 3u) {
            mark_r_used(r_used, out_reg, 2);
        }
    }

    u32 decoded_count = 0;
    for (u32 i = 0; i < max_insts; i++) {
        u64 lo = snx_ld_le64(insn_bytes + i * 16);
        u64 hi = snx_ld_le64(insn_bytes + i * 16 + 8);
        u16 opc12 = (u16)(lo & 0xfffu);
        u16 base9 = (u16)(opc12 & 0x1ffu);
        AmpereForm form = classify_ampere_form(opc12);
        bool is_uni = is_uniform_ampere_op(base9);

        u8 pred = (u8)((lo >> 12) & 7u);
        u8 rd   = (u8)((lo >> 16) & 0xffu);
        u8 ra   = (u8)((lo >> 24) & 0xffu);
        u8 rb   = (u8)((lo >> 32) & 0xffu);
        u8 rc   = (u8)(hi & 0xffu);

        /* Explicit uniform source flags in hi[38:36]:
         *   bit 36: Ra is URa
         *   bit 37: Rb is URb
         *   bit 38: Rc is URc
         *   bit 39: Guard predicate is UP
         */
        bool ra_is_ur   = is_uni && (base9 != AMP_OP_R2UR) ? true : ((hi >> 36) & 1u) != 0;
        bool rb_is_ur   = is_uni ? true : ((hi >> 37) & 1u) != 0;
        bool rc_is_ur   = is_uni ? true : ((hi >> 38) & 1u) != 0;
        bool pred_is_up = is_uni ? true : ((hi >> 39) & 1u) != 0;

        if (pred < 7) {
            if (pred_is_up) up_used[pred] = 1;
            else            p_used[pred]  = 1;
        }

        /* Destination width (vec2/vec4 loads, IMAD.WIDE, etc.) */
        u32 dst_width = 1;
        if (base9 == AMP_OP_IMAD_WIDE || base9 == AMP_OP_DADD || base9 == AMP_OP_DMUL || base9 == AMP_OP_DFMA ||
            base9 == AMP_OP_CS2R) {
            dst_width = 2;
        } else if (base9 == AMP_OP_LDC || base9 == AMP_OP_ULDC || base9 == AMP_OP_LDG || base9 == AMP_OP_LDS ||
                   base9 == AMP_OP_LDL) {
            u32 sz = (u32)((hi >> 20) & 7u);
            if (sz == 5) dst_width = 2;
            else if (sz >= 6) dst_width = 4;
        } else if (base9 == AMP_OP_ALD) {
            dst_width = (u32)(((hi >> 20) & 3u) + 1u);
        } else if (base9 == AMP_OP_TEX || base9 == AMP_OP_TLD || base9 == AMP_OP_TLD4 ||
                   base9 == AMP_OP_TMML || base9 == AMP_OP_TXD || base9 == AMP_OP_TXQ) {
            u32 mask = (u32)((hi >> 12) & 0xfu);
            if (!mask) mask = 0xfu;
            dst_width = (u32)__builtin_popcount(mask);
        }

        /* Record destination register */
        if (base9 == AMP_OP_ISETP || base9 == AMP_OP_UISETP || base9 == AMP_OP_FSETP ||
            base9 == AMP_OP_HSETP2 || base9 == AMP_OP_DSETP || base9 == AMP_OP_PLOP3 || base9 == AMP_OP_UPLOP3) {
            u8 pd0 = (u8)((hi >> 27) & 7u);
            u8 pd1 = (u8)((hi >> 30) & 7u);
            u8 ps0 = (u8)((hi >> 33) & 7u);
            if (is_uni) {
                if (pd0 < 7) up_used[pd0] = 1;
                if (pd1 < 7) up_used[pd1] = 1;
                if (ps0 < 7) up_used[ps0] = 1;
            } else {
                if (pd0 < 7) p_used[pd0] = 1;
                if (pd1 < 7) p_used[pd1] = 1;
                if (ps0 < 7) p_used[ps0] = 1;
            }
        } else if (is_uni) {
            mark_ur_used(ur_used, rd, dst_width);
        } else {
            mark_r_used(r_used, rd, dst_width);
        }

        /* Record source registers */
        if (ra_is_ur) mark_ur_used(ur_used, ra, 1);
        else          mark_r_used(r_used, ra, 1);

        if (form == AMP_FORM_REG || form == AMP_FORM_RC) {
            if (rb_is_ur) mark_ur_used(ur_used, rb, 1);
            else          mark_r_used(r_used, rb, 1);
        }
        if (rc != 255) {
            if (rc_is_ur) mark_ur_used(ur_used, rc, 1);
            else          mark_r_used(r_used, rc, 1);
        }

        decoded_count++;

        /* Stop at unconditional EXIT */
        if (base9 == AMP_OP_EXIT && pred == 7) {
            break;
        }
    }

    /* Count vector GPRs used */
    u32 vec_used_count = 0;
    for (u32 r = 0; r < 255; r++) {
        if (r_used[r]) vec_used_count++;
    }
    stats->vec_reg_count = vec_used_count;

    /* Allocate UR0..UR62 into free high R registers (prefer R192..R253, fallback to any free R) */
    for (u32 u = 0; u < 64; u++) stats->ur_to_r[u] = 255;
    u32 alloc_cursor = 192;
    for (u32 u = 0; u < 63; u++) {
        if (!ur_used[u]) continue;
        stats->uni_reg_count++;

        /* Try R192..R253 first so UR mappings are visually obvious in logs and disassemblies */
        u32 chosen = 255;
        while (alloc_cursor < 254) {
            if (!r_used[alloc_cursor]) {
                chosen = alloc_cursor++;
                r_used[chosen] = 1;
                break;
            }
            alloc_cursor++;
        }
        /* Fallback: scan downward from R253..R1 */
        if (chosen == 255) {
            for (int r = 253; r >= 1; r--) {
                if (!r_used[r]) {
                    chosen = (u32)r;
                    r_used[chosen] = 1;
                    break;
                }
            }
        }
        if (chosen != 255) {
            stats->ur_to_r[u] = (u8)chosen;
            AMP_LOG("[AMPERE-SCAN] Mapped Uniform Register UR%-2u -> Vector Register R%-3u", u, chosen);
        } else {
            /* Extreme register pressure (>254 combined): map to R254 scratch */
            stats->ur_to_r[u] = 254;
            stats->uni_reg_spilled++;
            AMP_WARN("[AMPERE-SCAN] High GPR pressure! Spilling UR%-2u -> scratch R254", u);
        }
    }

    /* Allocate UP0..UP6 into free P0..P6 predicates */
    for (u32 up = 0; up < 8; up++) stats->up_to_p[up] = 7;
    for (u32 up = 0; up < 7; up++) {
        if (!up_used[up]) continue;
        stats->uni_pred_count++;
        u32 chosen_p = 7;
        for (int p = 6; p >= 0; p--) {
            if (!p_used[p]) {
                chosen_p = (u32)p;
                p_used[p] = 1;
                break;
            }
        }
        if (chosen_p == 7) {
            chosen_p = 6; /* Share P6 if all 7 predicates are simultaneously live */
            AMP_WARN("[AMPERE-SCAN] All P0..P6 in use; sharing P6 for UP%u", up);
        } else {
            AMP_LOG("[AMPERE-SCAN] Mapped Uniform Predicate UP%u -> Vector Predicate P%u", up, chosen_p);
        }
        stats->up_to_p[up] = (u8)chosen_p;
    }

    return decoded_count;
}

/* Helpers to remap registers and predicates using AmpereLowerStats */
static u8 remap_gpr(const AmpereLowerStats* stats, u8 reg, bool is_ur, u8* remapped_flag, u8 bit) {
    if (!is_ur) return reg;
    if (reg >= 63) return 255; /* URZ (63 or 255) -> RZ (255) */
    if (remapped_flag) *remapped_flag |= bit;
    return stats->ur_to_r[reg];
}

static u8 remap_pred(const AmpereLowerStats* stats, u8 pred, bool is_up, u8* remapped_flag, u8 bit) {
    if ((pred & 7) == 7) return 7; /* PT / UPT */
    if (!is_up) return pred & 7;
    if (remapped_flag) *remapped_flag |= bit;
    return stats->up_to_p[pred & 7];
}

/* ============================================================================
 * 4. Maxwell 64-bit Instruction Word Builders (matching maxwell_opcodes.def)
 * ============================================================================ */

static u64 mx_set_guard(u64 w, u8 pred, u8 pred_inv) {
    w &= ~(0xfull << 16);
    w |= ((u64)(pred & 7u) << 16) | ((u64)(pred_inv & 1u) << 19);
    return w;
}

static u64 mx_set_rd_ra(u64 w, u8 rd, u8 ra) {
    w &= ~0xffffull;
    w |= (u64)rd | ((u64)ra << 8);
    return w;
}

static u64 mx_encode_cbuf(u32 bank, u32 byte_offset) {
    u32 word_off = (byte_offset >> 2) & 0x3fffu;
    return ((u64)(bank & 0x1fu) << 34) | ((u64)word_off << 20);
}

static u64 mx_encode_imm19(u32 imm32) {
    u64 lo19 = (u64)(imm32 & 0x7ffffu) << 20;
    u64 sign = ((imm32 >> 31) & 1u) ? (1ull << 56) : 0ull;
    return lo19 | sign;
}

static u64 mx_encode_fimm20(u32 fbits32) {
    u64 exp_mant = (u64)((fbits32 >> 12) & 0x7ffffu) << 20;
    u64 sign     = ((fbits32 >> 31) & 1u) ? (1ull << 56) : 0ull;
    return exp_mant | sign;
}

/* ============================================================================
 * 5. Ampere 128-bit Instruction -> MInst IR Decoder & Micro-Op Lowering Pass
 * ============================================================================ */

static void append_minst(MInst* list, u32* count, u32 max_count, const MInst* inst) {
    if (*count < max_count) {
        list[*count] = *inst;
    }
    (*count)++;
}

static u32 decode_and_lower_ampere_inst(u32 idx,
                                        u64 lo,
                                        u64 hi,
                                        ShaderStage stage,
                                        AmpereLowerStats* stats,
                                        MInst* out_list,
                                        u32* io_count,
                                        u32 max_count) {
    u32 start_count = *io_count;
    u16 opc12 = (u16)(lo & 0xfffu);
    u16 base9 = (u16)(opc12 & 0x1ffu);
    AmpereForm form = classify_ampere_form(opc12);
    bool is_uni = is_uniform_ampere_op(base9);

    u8 raw_pred  = (u8)((lo >> 12) & 7u);
    u8 pred_inv  = (u8)((lo >> 15) & 1u);
    u8 raw_rd    = (u8)((lo >> 16) & 0xffu);
    u8 raw_ra    = (u8)((lo >> 24) & 0xffu);
    u8 raw_rb    = (u8)((lo >> 32) & 0xffu);
    u8 raw_rc    = (u8)(hi & 0xffu);
    u32 imm32    = (u32)(lo >> 32);
    u32 cb_off   = (u32)((lo >> 32) & 0x3ffffu);
    u32 cb_bank  = (u32)((lo >> 50) & 0x1fu);

    bool ra_is_ur   = (is_uni && base9 != AMP_OP_R2UR) || (((hi >> 36) & 1u) != 0);
    bool rb_is_ur   = is_uni || (((hi >> 37) & 1u) != 0);
    bool rc_is_ur   = is_uni || (((hi >> 38) & 1u) != 0);
    bool pred_is_up = is_uni || (((hi >> 39) & 1u) != 0);

    u8 ur_mask = 0;
    u8 up_mask = 0;
    u8 pred = remap_pred(stats, raw_pred, pred_is_up, &up_mask, 1);
    u8 rd   = remap_gpr(stats, raw_rd, is_uni, &ur_mask, 1);
    u8 ra   = remap_gpr(stats, raw_ra, ra_is_ur, &ur_mask, 2);
    u8 rb   = remap_gpr(stats, raw_rb, rb_is_ur, &ur_mask, 4);
    u8 rc   = remap_gpr(stats, raw_rc, rc_is_ur, &ur_mask, 8);

    MInst m;
    memset(&m, 0, sizeof(m));
    m.orig_idx         = idx;
    m.orig_pc          = idx * 16;
    m.amp_opc          = opc12;
    m.rd               = rd;
    m.ra               = ra;
    m.rb               = rb;
    m.rc               = rc;
    m.pred             = pred;
    m.pred_inv         = pred_inv;
    m.ur_remapped_mask = ur_mask;
    m.up_remapped_mask = up_mask;
    m.mnemonic         = "UNKNOWN";

    if (ur_mask || up_mask) {
        snprintf(m.note, sizeof(m.note), "UR/UP remap(mask=0x%x/0x%x)", ur_mask, up_mask);
    }

    switch (base9) {
        case AMP_OP_NOP:
        case AMP_OP_YIELD:
        case AMP_OP_WARPSYNC:
        case AMP_OP_LDGDEPBAR: {
            m.op       = O_NOP;
            m.mnemonic = base9 == AMP_OP_NOP       ? "NOP"
                       : base9 == AMP_OP_YIELD     ? "YIELD->NOP"
                       : base9 == AMP_OP_WARPSYNC  ? "WARPSYNC->NOP"
                                                   : "LDGDEPBAR->NOP";
            m.word     = mx_set_guard(0x50b0000000000000ULL, pred, pred_inv);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_DEPBAR: {
            m.op       = O_DEPBAR;
            m.mnemonic = "DEPBAR";
            m.word     = mx_set_guard(0xf0f0000000000000ULL, pred, pred_inv);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_MEMBAR: {
            m.op       = O_MEMBAR;
            m.mnemonic = "MEMBAR";
            m.word     = mx_set_guard(0xef98000000000000ULL, pred, pred_inv);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_BAR: {
            m.op       = O_BAR;
            m.mnemonic = "BAR.SYNC";
            /* Canonical BAR.SYNC 0 for whole workgroup matching supported() in spirv_translate.c */
            m.word     = mx_set_guard(0xf0a8188000070000ULL, pred, pred_inv);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_MOV:
        case AMP_OP_UMOV:
        case AMP_OP_R2UR: {
            m.mnemonic = base9 == AMP_OP_UMOV ? "UMOV->MOV"
                       : base9 == AMP_OP_R2UR ? "R2UR->MOV"
                                              : "MOV";
            if (base9 == AMP_OP_R2UR) {
                /* R2UR reads vector Ra and writes uniform URd */
                m.op   = O_MOV_R;
                m.word = mx_set_guard(0x5c98078000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, 255);
                m.word |= ((u64)ra << 20);
            } else if (form == AMP_FORM_IMM) {
                m.op   = O_MOV32I;
                m.word = mx_set_guard(0x010000000000f000ULL, pred, pred_inv);
                m.word |= (u64)rd | ((u64)imm32 << 20);
            } else if (form == AMP_FORM_CBUF) {
                m.op   = O_MOV_C;
                m.word = mx_set_guard(0x4c98078000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, 255);
                m.word |= mx_encode_cbuf(cb_bank, cb_off);
            } else {
                m.op   = O_MOV_R;
                m.word = mx_set_guard(0x5c98078000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, 255);
                m.word |= ((u64)rb << 20);
            }
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_S2R:
        case AMP_OP_S2UR: {
            u8 sr = (u8)((hi >> 8) & 0xffu);
            if (!sr && (lo >> 32)) sr = (u8)((lo >> 32) & 0xffu);
            m.op       = O_S2R;
            m.mnemonic = base9 == AMP_OP_S2UR ? "S2UR->S2R" : "S2R";
            m.word     = mx_set_guard(0xf0c8000000000000ULL, pred, pred_inv);
            m.word     = mx_set_rd_ra(m.word, rd, 255);
            m.word    |= ((u64)sr << 20);
            snprintf(m.note, sizeof(m.note), "SR=0x%02x -> R%u", sr, rd);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_CS2R: {
            u8 sr = (u8)((hi >> 8) & 0xffu);
            if (sr != 0x50 && sr != 0x52) sr = 0xff; /* SRZ */
            m.op       = O_CS2R;
            m.mnemonic = "CS2R";
            m.word     = mx_set_guard(0x50c8000000000000ULL, pred, pred_inv);
            m.word     = mx_set_rd_ra(m.word, rd, 255);
            m.word    |= ((u64)sr << 20);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_LDC:
        case AMP_OP_ULDC: {
            u32 sz = (u32)((hi >> 20) & 7u);
            if (sz > 5 || sz == 0) sz = 4; /* Default to U32 (4) */
            if (sz == 5 && rd >= 254) sz = 4;
            m.op       = O_LDC;
            m.mnemonic = base9 == AMP_OP_ULDC ? "ULDC->LDC" : "LDC";
            m.word     = mx_set_guard(0xef90000000000000ULL, pred, pred_inv);
            m.word     = mx_set_rd_ra(m.word, rd, ra);
            m.word    |= ((u64)(cb_off & 0xffffu) << 20);
            m.word    |= ((u64)(cb_bank & 0x1fu) << 36);
            m.word    |= ((u64)(sz & 7u) << 48);
            snprintf(m.note, sizeof(m.note), "c[0x%x][R%u+0x%x] sz=%u -> R%u", cb_bank, ra, cb_off & 0xffffu, sz, rd);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_IADD3:
        case AMP_OP_UIADD3: {
            m.mnemonic = base9 == AMP_OP_UIADD3 ? "UIADD3->IADD3" : "IADD3";
            u64 cc   = ((hi >> 16) & 1u) ? (1ull << 47) : 0ull;
            u64 x    = ((hi >> 17) & 1u) ? (1ull << 48) : 0ull;
            u64 negc = ((hi >> 18) & 1u) ? (1ull << 49) : 0ull;
            u64 negb = ((hi >> 19) & 1u) ? (1ull << 50) : 0ull;
            u64 nega = ((hi >> 20) & 1u) ? (1ull << 51) : 0ull;

            if (form == AMP_FORM_IMM && rc == 255 && !negc) {
                /* 32-bit immediate + Ra -> IADD32I */
                m.op   = O_IADD32I;
                m.word = mx_set_guard(0x1c00000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)imm32 << 20);
                if (cc)   m.word |= (1ull << 52);
                if (x)    m.word |= (1ull << 53);
                if (nega) m.word |= (2ull << 55);
                if (negb) m.word |= (1ull << 55);
            } else if (form == AMP_FORM_CBUF) {
                m.op   = O_IADD3_C;
                m.word = mx_set_guard(0x4cc0000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_cbuf(cb_bank, cb_off) | ((u64)rc << 39) | cc | x | negc | negb | nega;
            } else if (form == AMP_FORM_IMM) {
                m.op   = O_IADD3_I;
                m.word = mx_set_guard(0x38c0000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_imm19(imm32) | ((u64)rc << 39) | cc | x | negc | negb | nega;
            } else {
                m.op   = O_IADD3_R;
                m.word = mx_set_guard(0x5cc0000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)rb << 20) | ((u64)rc << 39) | cc | x | negc | negb | nega;
            }
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_IMAD:
        case AMP_OP_UIMAD: {
            m.mnemonic   = base9 == AMP_OP_UIMAD ? "UIMAD->IMAD" : "IMAD";
            u64 signed_a = ((hi >> 16) & 1u) ? (1ull << 48) : 0ull;
            u64 x        = ((hi >> 17) & 1u) ? (1ull << 49) : 0ull;
            u64 mode     = ((hi >> 18) & 3u) << 51;
            u64 signed_b = ((hi >> 20) & 1u) ? (1ull << 53) : 0ull;
            u64 is_hi    = ((hi >> 21) & 1u) ? (1ull << 54) : 0ull;

            if (form == AMP_FORM_CBUF) {
                m.op   = O_IMAD_CR;
                m.word = mx_set_guard(0x4a00000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_cbuf(cb_bank, cb_off) | ((u64)rc << 39) | signed_a | x | mode | signed_b | is_hi;
            } else if (form == AMP_FORM_IMM) {
                m.op   = O_IMAD_I;
                m.word = mx_set_guard(0x3400000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_imm19(imm32) | ((u64)rc << 39) | signed_a | x | mode | signed_b | is_hi;
            } else {
                m.op   = O_IMAD_R;
                m.word = mx_set_guard(0x5a00000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)rb << 20) | ((u64)rc << 39) | signed_a | x | mode | signed_b | is_hi;
            }
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_IMAD_WIDE: {
            /* 1-to-2 micro-op expansion:
             *   1) IADD_R / IMAD_R low 32-bit (or IADD3_R with CC when Rb == 1)
             *   2) IMAD.HI.X high 32-bit into Rd+1 with Rc+1
             */
            stats->expanded_micro_ops++;
            u8 rd_hi = (rd < 254) ? (u8)(rd + 1) : 255;
            u8 rc_hi = (rc < 254) ? (u8)(rc + 1) : 255;
            u64 signed_a = ((hi >> 16) & 1u) ? (1ull << 48) : 0ull;
            u64 signed_b = ((hi >> 20) & 1u) ? (1ull << 53) : 0ull;

            /* Op 1: Low 32 bits: IMAD_R Rd, Ra, Rb, Rc */
            m.op       = form == AMP_FORM_CBUF ? O_IMAD_CR : form == AMP_FORM_IMM ? O_IMAD_I : O_IMAD_R;
            m.mnemonic = "IMAD.WIDE[0:LO]";
            m.sub_op   = 0;
            m.word     = mx_set_guard(form == AMP_FORM_CBUF ? 0x4a00000000000000ULL
                                    : form == AMP_FORM_IMM  ? 0x3400000000000000ULL
                                                            : 0x5a00000000000000ULL,
                                      pred, pred_inv);
            m.word     = mx_set_rd_ra(m.word, rd, ra);
            if (form == AMP_FORM_CBUF)      m.word |= mx_encode_cbuf(cb_bank, cb_off);
            else if (form == AMP_FORM_IMM)  m.word |= mx_encode_imm19(imm32);
            else                            m.word |= ((u64)rb << 20);
            m.word |= ((u64)rc << 39) | signed_a | signed_b;
            snprintf(m.note, sizeof(m.note), "IMAD.WIDE lo -> R%u", rd);
            append_minst(out_list, io_count, max_count, &m);

            /* Op 2: High 32 bits: IMAD.HI Rd+1, Ra, Rb, Rc+1 */
            MInst m2 = m;
            m2.mnemonic = "IMAD.WIDE[1:HI]";
            m2.sub_op   = 1;
            m2.rd       = rd_hi;
            m2.rc       = rc_hi;
            m2.word     = mx_set_rd_ra(m.word, rd_hi, ra);
            m2.word    &= ~(0xffull << 39);
            m2.word    |= ((u64)rc_hi << 39) | (1ull << 54); /* bit 54 = .HI */
            snprintf(m2.note, sizeof(m2.note), "IMAD.WIDE hi -> R%u", rd_hi);
            append_minst(out_list, io_count, max_count, &m2);
            break;
        }

        case AMP_OP_LOP3:
        case AMP_OP_ULOP3: {
            u8 lut = (u8)((hi >> 8) & 0xffu);
            m.mnemonic = base9 == AMP_OP_ULOP3 ? "ULOP3->LOP3" : "LOP3";
            if (form == AMP_FORM_CBUF) {
                m.op   = O_LOP3_C;
                m.word = mx_set_guard(0x0200000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_cbuf(cb_bank, cb_off) | ((u64)rc << 39) | ((u64)lut << 48);
            } else if (form == AMP_FORM_IMM) {
                m.op   = O_LOP3_I;
                m.word = mx_set_guard(0x3c00000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_imm19(imm32) | ((u64)rc << 39) | ((u64)lut << 48);
            } else {
                m.op   = O_LOP3_R;
                m.word = mx_set_guard(0x5be0000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)rb << 20) | ((u64)lut << 28) | ((u64)rc << 39) | (7ull << 48);
            }
            snprintf(m.note, sizeof(m.note), "LUT=0x%02x", lut);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_SHF:
        case AMP_OP_USHF: {
            bool is_right = ((hi >> 16) & 1u) != 0;
            bool is_hi_w  = ((hi >> 17) & 1u) != 0;
            m.mnemonic = base9 == AMP_OP_USHF ? (is_right ? "USHF.R->SHF.R" : "USHF.L->SHF.L")
                                              : (is_right ? "SHF.R" : "SHF.L");
            if (form == AMP_FORM_IMM) {
                m.op   = is_right ? O_SHF_RI : O_SHF_LI;
                m.word = mx_set_guard(is_right ? 0x38f8000000000000ULL : 0x36f8000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)(imm32 & 0x3fu) << 20) | ((u64)rc << 39) | (is_hi_w ? (1ull << 50) : 0ull);
            } else {
                m.op   = is_right ? O_SHF_RR : O_SHF_LR;
                m.word = mx_set_guard(is_right ? 0x5cf8000000000000ULL : 0x5bf8000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)rb << 20) | ((u64)rc << 39) | (is_hi_w ? (1ull << 50) : 0ull);
            }
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_LEA:
        case AMP_OP_ULEA: {
            bool is_hi_lea = ((hi >> 16) & 1u) != 0;
            u32 shift = (u32)((hi >> 8) & 0x1fu);
            m.mnemonic = base9 == AMP_OP_ULEA ? "ULEA->LEA" : "LEA";
            if (is_hi_lea) {
                m.op   = form == AMP_FORM_CBUF ? O_LEA_HC : O_LEA_HR;
                m.word = mx_set_guard(form == AMP_FORM_CBUF ? 0x1800000000000000ULL : 0x5bd8000000000000ULL,
                                      pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                if (form == AMP_FORM_CBUF) m.word |= mx_encode_cbuf(cb_bank, cb_off);
                else                       m.word |= ((u64)rb << 20);
                m.word |= ((u64)rc << 39) | ((u64)shift << 32) | (7ull << 48);
            } else if (form == AMP_FORM_CBUF) {
                m.op   = O_LEA_LC;
                m.word = mx_set_guard(0x4bd0000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_cbuf(cb_bank, cb_off) | ((u64)shift << 39) | (7ull << 48);
            } else if (form == AMP_FORM_IMM) {
                m.op   = O_LEA_LI;
                m.word = mx_set_guard(0x36d0000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_imm19(imm32) | ((u64)shift << 39) | (7ull << 48);
            } else {
                m.op   = O_LEA_LR;
                m.word = mx_set_guard(0x5bd0000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)rb << 20) | ((u64)shift << 39) | (7ull << 48);
            }
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_ISETP:
        case AMP_OP_UISETP: {
            u8 raw_pd0 = (u8)((hi >> 27) & 7u);
            u8 raw_pd1 = (u8)((hi >> 30) & 7u);
            u8 raw_ps0 = (u8)((hi >> 33) & 7u);
            u8 pd0 = remap_pred(stats, raw_pd0, is_uni, &m.up_remapped_mask, 2);
            u8 pd1 = remap_pred(stats, raw_pd1, is_uni, &m.up_remapped_mask, 4);
            u8 ps0 = remap_pred(stats, raw_ps0, is_uni, &m.up_remapped_mask, 8);
            u32 cmp = (u32)((hi >> 16) & 7u);
            if (cmp == 0) cmp = 2; /* Default EQ if unset */
            u32 is_signed = (u32)((hi >> 19) & 1u);
            u32 bop = (u32)((hi >> 20) & 3u);
            if (bop >= 3) bop = 0;

            m.mnemonic = base9 == AMP_OP_UISETP ? "UISETP->ISETP" : "ISETP";
            if (form == AMP_FORM_CBUF) {
                m.op   = O_ISETP_C;
                m.word = mx_set_guard(0x4b60000000000000ULL, pred, pred_inv);
                m.word |= mx_encode_cbuf(cb_bank, cb_off);
            } else if (form == AMP_FORM_IMM) {
                m.op   = O_ISETP_I;
                m.word = mx_set_guard(0x3660000000000000ULL, pred, pred_inv);
                m.word |= mx_encode_imm19(imm32);
            } else {
                m.op   = O_ISETP_R;
                m.word = mx_set_guard(0x5b60000000000000ULL, pred, pred_inv);
                m.word |= ((u64)rb << 20);
            }
            m.word |= (u64)(pd1 & 7u) | ((u64)(pd0 & 7u) << 3) | ((u64)ra << 8) |
                      ((u64)(ps0 & 7u) << 39) | ((u64)bop << 45) | ((u64)is_signed << 48) | ((u64)cmp << 49);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_SEL:
        case AMP_OP_USEL:
        case AMP_OP_FSEL: {
            u8 raw_ps0 = (u8)((hi >> 33) & 7u);
            u8 ps0_inv = (u8)((hi >> 23) & 1u);
            u8 ps0 = remap_pred(stats, raw_ps0, is_uni, &m.up_remapped_mask, 2);
            m.mnemonic = base9 == AMP_OP_USEL ? "USEL->SEL" : base9 == AMP_OP_FSEL ? "FSEL->SEL" : "SEL";
            if (form == AMP_FORM_CBUF) {
                m.op   = O_SEL_C;
                m.word = mx_set_guard(0x4ca0000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_cbuf(cb_bank, cb_off);
            } else if (form == AMP_FORM_IMM) {
                m.op   = O_SEL_I;
                m.word = mx_set_guard(0x38a0000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_imm19(imm32);
            } else {
                m.op   = O_SEL_R;
                m.word = mx_set_guard(0x5ca0000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)rb << 20);
            }
            m.word |= ((u64)(ps0 & 7u) << 39) | ((u64)ps0_inv << 42);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_SGXT:
        case AMP_OP_USGXT: {
            /* Sign-extend bitfield from bit 0 with width in Rb/Imm -> lower to BFE.S32 (bit 48 = signed) */
            m.mnemonic = base9 == AMP_OP_USGXT ? "USGXT->BFE.S32" : "SGXT->BFE.S32";
            if (form == AMP_FORM_IMM) {
                u32 width = imm32 & 0x1fu;
                u32 bfe_imm = (width << 8); /* offset=0 in [7:0], width in [15:8] */
                m.op   = O_BFE_I;
                m.word = mx_set_guard(0x3800000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_imm19(bfe_imm) | (1ull << 48);
            } else {
                m.op   = O_BFE_R;
                m.word = mx_set_guard(0x5c00000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)rb << 20) | (1ull << 48);
            }
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_BMSK:
        case AMP_OP_UBMSK: {
            /* BMSK Rd, Ra(pos), Rb(width) -> BFI Rd, RZ(0xffffffff), packed_pos_width, RZ(0)
             * For immediate form, emit MOV32I directly with computed mask! */
            m.mnemonic = base9 == AMP_OP_UBMSK ? "UBMSK->BFI" : "BMSK->BFI";
            if (form == AMP_FORM_IMM) {
                u32 pos = ra < 32 ? ra : 0;
                u32 wid = imm32 & 0x1fu;
                u32 mask = (wid >= 32) ? 0xffffffffu : (((1u << wid) - 1u) << pos);
                m.op   = O_MOV32I;
                m.word = mx_set_guard(0x010000000000f000ULL, pred, pred_inv);
                m.word |= (u64)rd | ((u64)mask << 20);
            } else {
                m.op   = O_BFI_I;
                m.word = mx_set_guard(0x36f0000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_imm19(0xffffffffu) | ((u64)255 << 39);
            }
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_BFE:
        case AMP_OP_UBFE: {
            u64 is_signed = ((hi >> 16) & 1u) ? (1ull << 48) : 0ull;
            m.mnemonic = base9 == AMP_OP_UBFE ? "UBFE->BFE" : "BFE";
            if (form == AMP_FORM_CBUF) {
                m.op   = O_BFE_C;
                m.word = mx_set_guard(0x4c00000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_cbuf(cb_bank, cb_off) | is_signed;
            } else if (form == AMP_FORM_IMM) {
                m.op   = O_BFE_I;
                m.word = mx_set_guard(0x3800000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_imm19(imm32) | is_signed;
            } else {
                m.op   = O_BFE_R;
                m.word = mx_set_guard(0x5c00000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)rb << 20) | is_signed;
            }
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_POPC:
        case AMP_OP_UPOPC: {
            m.mnemonic = base9 == AMP_OP_UPOPC ? "UPOPC->POPC" : "POPC";
            m.op       = form == AMP_FORM_CBUF ? O_POPC_C : form == AMP_FORM_IMM ? O_POPC_I : O_POPC_R;
            m.word     = mx_set_guard(form == AMP_FORM_CBUF ? 0x4c08000000000000ULL
                                    : form == AMP_FORM_IMM  ? 0x3808000000000000ULL
                                                            : 0x5c08000000000000ULL,
                                      pred, pred_inv);
            m.word     = mx_set_rd_ra(m.word, rd, 255);
            if (form == AMP_FORM_CBUF)      m.word |= mx_encode_cbuf(cb_bank, cb_off);
            else if (form == AMP_FORM_IMM)  m.word |= mx_encode_imm19(imm32);
            else                            m.word |= ((u64)(ra != 255 ? ra : rb) << 20);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_PRMT:
        case AMP_OP_UPRMT: {
            u32 mode = (u32)((hi >> 16) & 7u);
            if (mode > 6) mode = 0;
            m.mnemonic = base9 == AMP_OP_UPRMT ? "UPRMT->PRMT" : "PRMT";
            if (form == AMP_FORM_IMM) {
                m.op   = O_PRMT_I;
                m.word = mx_set_guard(0x36c0000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_imm19(imm32) | ((u64)rc << 39) | ((u64)mode << 48);
            } else {
                m.op   = O_PRMT_R;
                m.word = mx_set_guard(0x5bc0000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)rb << 20) | ((u64)rc << 39) | ((u64)mode << 48);
            }
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_FADD: {
            u64 ftz  = ((hi >> 16) & 1u) ? (1ull << 44) : 0ull;
            u64 negb = ((hi >> 17) & 1u) ? (1ull << 45) : 0ull;
            u64 absa = ((hi >> 18) & 1u) ? (1ull << 46) : 0ull;
            u64 nega = ((hi >> 19) & 1u) ? (1ull << 48) : 0ull;
            u64 absb = ((hi >> 20) & 1u) ? (1ull << 49) : 0ull;
            u64 sat  = ((hi >> 21) & 1u) ? (1ull << 50) : 0ull;
            m.mnemonic = "FADD";
            if (form == AMP_FORM_IMM) {
                m.op   = O_FADD32I;
                m.word = mx_set_guard(0x0800000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)imm32 << 20);
                if (ftz)  m.word |= (1ull << 55);
                if (nega) m.word |= (1ull << 56);
                if (absa) m.word |= (1ull << 54);
            } else if (form == AMP_FORM_CBUF) {
                m.op   = O_FADD_C;
                m.word = mx_set_guard(0x4c58000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_cbuf(cb_bank, cb_off) | ftz | negb | absa | nega | absb | sat;
            } else {
                m.op   = O_FADD_R;
                m.word = mx_set_guard(0x5c58000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)rb << 20) | ftz | negb | absa | nega | absb | sat;
            }
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_FMUL: {
            u64 ftz  = ((hi >> 16) & 1u) ? (1ull << 44) : 0ull;
            u64 absa = ((hi >> 18) & 1u) ? (1ull << 46) : 0ull;
            u64 negb = ((hi >> 19) & 1u) ? (1ull << 48) : 0ull;
            u64 absb = ((hi >> 20) & 1u) ? (1ull << 49) : 0ull;
            u64 sat  = ((hi >> 21) & 1u) ? (1ull << 50) : 0ull;
            m.mnemonic = "FMUL";
            if (form == AMP_FORM_IMM) {
                m.op   = O_FMUL32I;
                m.word = mx_set_guard(0x1e00000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)imm32 << 20);
                if (sat) m.word |= (1ull << 55);
            } else if (form == AMP_FORM_CBUF) {
                m.op   = O_FMUL_C;
                m.word = mx_set_guard(0x4c68000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_cbuf(cb_bank, cb_off) | ftz | absa | negb | absb | sat;
            } else {
                m.op   = O_FMUL_R;
                m.word = mx_set_guard(0x5c68000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)rb << 20) | ftz | absa | negb | absb | sat;
            }
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_FFMA: {
            u64 negb = ((hi >> 17) & 1u) ? (1ull << 48) : 0ull;
            u64 negc = ((hi >> 18) & 1u) ? (1ull << 49) : 0ull;
            u64 sat  = ((hi >> 21) & 1u) ? (1ull << 50) : 0ull;
            u64 ftz  = ((hi >> 16) & 1u) ? (1ull << 53) : 0ull;
            m.mnemonic = "FFMA";
            if (form == AMP_FORM_CBUF) {
                m.op   = O_FFMA_CR;
                m.word = mx_set_guard(0x4980000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_cbuf(cb_bank, cb_off) | ((u64)rc << 39) | negb | negc | sat | ftz;
            } else if (form == AMP_FORM_RC) {
                m.op   = O_FFMA_RC;
                m.word = mx_set_guard(0x5180000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_cbuf(cb_bank, cb_off) | ((u64)rc << 39) | negb | negc | sat | ftz;
            } else if (form == AMP_FORM_IMM) {
                m.op   = O_FFMA_I;
                m.word = mx_set_guard(0x3280000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= mx_encode_fimm20(imm32) | ((u64)rc << 39) | negb | negc | sat | ftz;
            } else {
                m.op   = O_FFMA_R;
                m.word = mx_set_guard(0x5980000000000000ULL, pred, pred_inv);
                m.word = mx_set_rd_ra(m.word, rd, ra);
                m.word |= ((u64)rb << 20) | ((u64)rc << 39) | negb | negc | sat | ftz;
            }
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_FMNMX: {
            u8 raw_ps0 = (u8)((hi >> 33) & 7u);
            u8 ps0_inv = (u8)((hi >> 23) & 1u);
            u8 ps0 = remap_pred(stats, raw_ps0, false, &m.up_remapped_mask, 2);
            m.mnemonic = "FMNMX";
            m.op       = form == AMP_FORM_CBUF ? O_FMNMX_C : form == AMP_FORM_IMM ? O_FMNMX_I : O_FMNMX_R;
            m.word     = mx_set_guard(form == AMP_FORM_CBUF ? 0x4c60000000000000ULL
                                    : form == AMP_FORM_IMM  ? 0x3860000000000000ULL
                                                            : 0x5c60000000000000ULL,
                                      pred, pred_inv);
            m.word     = mx_set_rd_ra(m.word, rd, ra);
            if (form == AMP_FORM_CBUF)      m.word |= mx_encode_cbuf(cb_bank, cb_off);
            else if (form == AMP_FORM_IMM)  m.word |= mx_encode_fimm20(imm32);
            else                            m.word |= ((u64)rb << 20);
            m.word |= ((u64)(ps0 & 7u) << 39) | ((u64)ps0_inv << 42);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_FSETP: {
            u8 pd0 = remap_pred(stats, (u8)((hi >> 27) & 7u), false, &m.up_remapped_mask, 2);
            u8 pd1 = remap_pred(stats, (u8)((hi >> 30) & 7u), false, &m.up_remapped_mask, 4);
            u8 ps0 = remap_pred(stats, (u8)((hi >> 33) & 7u), false, &m.up_remapped_mask, 8);
            u32 cmp = (u32)((hi >> 16) & 0xfu);
            if (cmp == 0) cmp = 2;
            u32 bop = (u32)((hi >> 20) & 3u);
            if (bop >= 3) bop = 0;

            m.mnemonic = "FSETP";
            if (form == AMP_FORM_CBUF) {
                m.op   = O_FSETP_C;
                m.word = mx_set_guard(0x4bb0000000000000ULL, pred, pred_inv);
                m.word |= mx_encode_cbuf(cb_bank, cb_off);
            } else if (form == AMP_FORM_IMM) {
                m.op   = O_FSETP_I;
                m.word = mx_set_guard(0x36b0000000000000ULL, pred, pred_inv);
                m.word |= mx_encode_fimm20(imm32);
            } else {
                m.op   = O_FSETP_R;
                m.word = mx_set_guard(0x5bb0000000000000ULL, pred, pred_inv);
                m.word |= ((u64)rb << 20);
            }
            m.word |= (u64)(pd1 & 7u) | ((u64)(pd0 & 7u) << 3) | ((u64)ra << 8) |
                      ((u64)(ps0 & 7u) << 39) | ((u64)bop << 45) | ((u64)cmp << 48);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_FRND: {
            /* FRND -> F2F.F32.F32 with integer rounding bit 42 = 1 */
            u32 rnd = (u32)((hi >> 16) & 3u);
            m.op       = O_F2F_R;
            m.mnemonic = "FRND->F2F.RND";
            m.word     = mx_set_guard(0x5ca8000000000000ULL, pred, pred_inv);
            m.word    |= (u64)rd | (2ull << 8) | (2ull << 10) | ((u64)(ra != 255 ? ra : rb) << 20) |
                         ((u64)rnd << 39) | (1ull << 42);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_F2F:
        case AMP_OP_F2I:
        case AMP_OP_I2F:
        case AMP_OP_I2I: {
            u32 dst_fmt = (u32)((hi >> 8) & 3u);
            u32 src_fmt = (u32)((hi >> 10) & 3u);
            if (dst_fmt == 0 || dst_fmt > 2) dst_fmt = 2; /* 32-bit default */
            if (src_fmt == 0 || src_fmt > 2) src_fmt = 2;
            u32 rnd = (u32)((hi >> 16) & 3u);
            u8 src_r = (ra != 255) ? ra : rb;

            if (base9 == AMP_OP_F2F) {
                m.op       = O_F2F_R;
                m.mnemonic = "F2F";
                m.word     = mx_set_guard(0x5ca8000000000000ULL, pred, pred_inv);
            } else if (base9 == AMP_OP_F2I) {
                m.op       = O_F2I_R;
                m.mnemonic = "F2I";
                m.word     = mx_set_guard(0x5cb0000000000000ULL, pred, pred_inv);
            } else if (base9 == AMP_OP_I2F) {
                m.op       = O_I2F_R;
                m.mnemonic = "I2F";
                m.word     = mx_set_guard(0x5cb8000000000000ULL, pred, pred_inv);
            } else {
                m.op       = O_I2I_R;
                m.mnemonic = "I2I";
                m.word     = mx_set_guard(0x5ce0000000000000ULL, pred, pred_inv);
            }
            m.word |= (u64)rd | ((u64)dst_fmt << 8) | ((u64)src_fmt << 10) | ((u64)src_r << 20) | ((u64)rnd << 39);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_PLOP3:
        case AMP_OP_UPLOP3: {
            /* Lower PLOP3/UPLOP3 to PSETP */
            u8 pd0 = remap_pred(stats, (u8)((hi >> 27) & 7u), is_uni, &m.up_remapped_mask, 2);
            u8 pd1 = remap_pred(stats, (u8)((hi >> 30) & 7u), is_uni, &m.up_remapped_mask, 4);
            u8 pa  = remap_pred(stats, (u8)(raw_ra & 7u), is_uni, &m.up_remapped_mask, 8);
            u8 pb  = remap_pred(stats, (u8)(raw_rb & 7u), is_uni, &m.up_remapped_mask, 16);
            u8 pc  = remap_pred(stats, (u8)(raw_rc & 7u), is_uni, &m.up_remapped_mask, 32);
            m.op       = O_PSETP;
            m.mnemonic = base9 == AMP_OP_UPLOP3 ? "UPLOP3->PSETP" : "PLOP3->PSETP";
            m.word     = mx_set_guard(0x5090000000000000ULL, pred, pred_inv);
            m.word    |= (u64)(pd1 & 7u) | ((u64)(pd0 & 7u) << 3) | ((u64)(pa & 7u) << 12) |
                         ((u64)(pb & 7u) << 29) | ((u64)(pc & 7u) << 39);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_LDG:
        case AMP_OP_STG:
        case AMP_OP_LDS:
        case AMP_OP_STS:
        case AMP_OP_LDL:
        case AMP_OP_STL: {
            u32 sz = (u32)((hi >> 20) & 7u);
            if (sz == 0 || sz > 6) sz = 4; /* U32 default */
            u64 is_64b_addr = ((hi >> 16) & 1u) ? (1ull << 45) : 0ull;
            u32 off24 = imm32 & 0xffffffu;

            if (base9 == AMP_OP_LDG)      { m.op = O_LDG; m.mnemonic = "LDG"; m.word = 0xeed0000000000000ULL; }
            else if (base9 == AMP_OP_STG) { m.op = O_STG; m.mnemonic = "STG"; m.word = 0xeed8000000000000ULL; }
            else if (base9 == AMP_OP_LDS) { m.op = O_LDS; m.mnemonic = "LDS"; m.word = 0xef48000000000000ULL; }
            else if (base9 == AMP_OP_STS) { m.op = O_STS; m.mnemonic = "STS"; m.word = 0xef58000000000000ULL; }
            else if (base9 == AMP_OP_LDL) { m.op = O_LDL; m.mnemonic = "LDL"; m.word = 0xef40000000000000ULL; }
            else                          { m.op = O_STL; m.mnemonic = "STL"; m.word = 0xef50000000000000ULL; }

            m.word  = mx_set_guard(m.word, pred, pred_inv);
            m.word  = mx_set_rd_ra(m.word, rd, ra);
            m.word |= ((u64)off24 << 20) | is_64b_addr | ((u64)sz << 48);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_LDGSTS: {
            /* Ampere async global-to-shared copy:
             *   LDGSTS [Ra_shared + off_s], [Rb_global + off_g]
             * Lowered to:
             *   1) LDG.E R_scratch, [Rb_global]
             *   2) STS   [Ra_shared], R_scratch
             */
            stats->expanded_micro_ops++;
            u8 scratch_r = stats->ur_to_r[62] < 255 ? stats->ur_to_r[62] : 253;

            m.op       = O_LDG;
            m.mnemonic = "LDGSTS[0:LDG]";
            m.sub_op   = 0;
            m.word     = mx_set_guard(0xeed0000000000000ULL, pred, pred_inv);
            m.word     = mx_set_rd_ra(m.word, scratch_r, rb);
            m.word    |= (1ull << 45) | (4ull << 48);
            snprintf(m.note, sizeof(m.note), "LDGSTS -> LDG R%u, [R%u]", scratch_r, rb);
            append_minst(out_list, io_count, max_count, &m);

            MInst m2 = m;
            m2.op       = O_STS;
            m2.mnemonic = "LDGSTS[1:STS]";
            m2.sub_op   = 1;
            m2.word     = mx_set_guard(0xef58000000000000ULL, pred, pred_inv);
            m2.word     = mx_set_rd_ra(m2.word, scratch_r, ra);
            m2.word    |= (4ull << 48);
            snprintf(m2.note, sizeof(m2.note), "LDGSTS -> STS [R%u], R%u", ra, scratch_r);
            append_minst(out_list, io_count, max_count, &m2);
            break;
        }

        case AMP_OP_ALD:
        case AMP_OP_AST: {
            u32 addr  = imm32 & 0x3ffu;
            u32 count = (u32)((hi >> 20) & 3u);
            if (addr < 0x70 && addr != 0x60 && addr != 0x64 && addr != 0x6c) addr = 0x70;
            if (base9 == AMP_OP_ALD) {
                m.op       = O_ALD;
                m.mnemonic = "ALD";
                m.word     = mx_set_guard(0xefd8000000000000ULL, pred, pred_inv);
            } else {
                m.op       = O_AST;
                m.mnemonic = "AST";
                m.word     = mx_set_guard(0xeff0000000000000ULL, pred, pred_inv);
            }
            m.word = mx_set_rd_ra(m.word, rd, 255);
            m.word |= ((u64)addr << 20) | ((u64)255 << 39) | ((u64)count << 47);
            snprintf(m.note, sizeof(m.note), "attr[0x%x] x%u", addr, count + 1);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_IPA: {
            u32 addr = imm32 & 0x3ffu;
            if (addr < 0x70 && addr != 0x60 && addr != 0x64) addr = 0x80;
            m.op       = O_IPA;
            m.mnemonic = "IPA";
            m.word     = mx_set_guard(0xe000000000000000ULL, pred, pred_inv);
            m.word     = mx_set_rd_ra(m.word, rd, 255);
            m.word    |= ((u64)255 << 20) | ((u64)((addr >> 2) & 0xffu) << 30) | ((u64)255 << 39);
            snprintf(m.note, sizeof(m.note), "ipa[0x%x] -> R%u", addr, rd);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_TEX:
        case AMP_OP_TLD:
        case AMP_OP_TLD4:
        case AMP_OP_TMML:
        case AMP_OP_TXD:
        case AMP_OP_TXQ: {
            u32 dim      = (u32)((hi >> 8) & 7u);
            if (dim == 5) dim = 2; /* 2D default if 5 */
            u32 mask     = (u32)((hi >> 12) & 0xfu);
            if (!mask) mask = 0xfu;
            u32 tex_off  = imm32 & 0x1fffu;
            bool bindless = ((hi >> 16) & 1u) != 0;

            if (base9 == AMP_OP_TEX) {
                m.op       = bindless ? O_TEX_B : O_TEX;
                m.mnemonic = bindless ? "TEX.B" : "TEX";
                m.word     = bindless ? 0xde80000000000000ULL : 0xc000000000000000ULL;
            } else if (base9 == AMP_OP_TLD) {
                m.op       = bindless ? O_TLD_B : O_TLD;
                m.mnemonic = bindless ? "TLD.B" : "TLD";
                m.word     = bindless ? 0xdd00000000000000ULL : 0xdc00000000000000ULL;
            } else if (base9 == AMP_OP_TLD4) {
                m.op       = bindless ? O_TLD4_B : O_TLD4;
                m.mnemonic = bindless ? "TLD4.B" : "TLD4";
                m.word     = bindless ? 0xdec0000000000000ULL : 0xc800000000000000ULL;
            } else if (base9 == AMP_OP_TMML) {
                m.op       = bindless ? O_TMML_B : O_TMML;
                m.mnemonic = bindless ? "TMML.B" : "TMML";
                m.word     = bindless ? 0xdf60000000000000ULL : 0xdf58000000000000ULL;
            } else if (base9 == AMP_OP_TXD) {
                m.op       = bindless ? O_TXD_B : O_TXD;
                m.mnemonic = bindless ? "TXD.B" : "TXD";
                m.word     = bindless ? 0xde40000000000000ULL : 0xde00000000000000ULL;
                if (dim > 3) dim = 2;
            } else {
                m.op       = bindless ? O_TXQ_B : O_TXQ;
                m.mnemonic = bindless ? "TXQ.B" : "TXQ";
                m.word     = bindless ? 0xdf50000000000000ULL : 0xdf48000000000000ULL;
            }
            m.word  = mx_set_guard(m.word, pred, pred_inv);
            m.word  = mx_set_rd_ra(m.word, rd, ra);
            m.word |= ((u64)rb << 20) | ((u64)dim << 28) | ((u64)mask << 31) |
                      ((u64)tex_off << 36) | (7ull << 51);
            if (base9 == AMP_OP_TXQ) {
                m.word &= ~(0x3full << 22);
                m.word |= (1ull << 22); /* Query dimensions = 1 */
            }
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_BRA:
        case AMP_OP_BSSY:
        case AMP_OP_CAL: {
            /* Signed 24-bit byte offset relative to next 128-bit instruction ((idx + 1) * 16) */
            s32 rel_bytes = (s32)((s32)(imm32 << 8) >> 8);
            s32 target_pc = (s32)((idx + 1) * 16) + rel_bytes;
            if (target_pc < 0) target_pc = 0;

            m.is_pc_rel_branch = 1;
            m.target_byte_pc   = target_pc;
            if (base9 == AMP_OP_BRA) {
                m.op       = O_BRA;
                m.mnemonic = "BRA";
                m.word     = mx_set_guard(0xe24000000000000fULL, pred, pred_inv);
            } else if (base9 == AMP_OP_BSSY) {
                m.op       = O_SSY;
                m.mnemonic = "BSSY->SSY";
                m.word     = mx_set_guard(0xe290000000000000ULL, 7, 0);
            } else {
                m.op       = O_CAL;
                m.mnemonic = "CAL";
                m.word     = mx_set_guard(0xe260000000000000ULL, pred, pred_inv);
            }
            snprintf(m.note, sizeof(m.note), "target_amp_pc=0x%x", (u32)target_pc);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_BSYNC: {
            m.op       = O_SYNC;
            m.mnemonic = "BSYNC->SYNC";
            m.word     = mx_set_guard(0xf0f800000000000fULL, pred, pred_inv);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_BMOV: {
            /* Barrier convergence register move -> lower to NOP in structured Maxwell CRS model */
            m.op       = O_NOP;
            m.mnemonic = "BMOV->NOP";
            m.word     = mx_set_guard(0x50b0000000000000ULL, pred, pred_inv);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_BREAK: {
            m.op       = O_BRK;
            m.mnemonic = "BREAK->BRK";
            m.word     = mx_set_guard(0xe34000000000000fULL, pred, pred_inv);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_CONT: {
            m.op       = O_CONT;
            m.mnemonic = "CONT";
            m.word     = mx_set_guard(0xe35000000000000fULL, pred, pred_inv);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_KIL: {
            m.op       = O_KIL;
            m.mnemonic = "KIL";
            m.word     = mx_set_guard(0xe33000000000000fULL, pred, pred_inv);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_RET: {
            m.op       = O_RET;
            m.mnemonic = "RET";
            m.word     = mx_set_guard(0xe32000000000000fULL, pred, pred_inv);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_EXIT: {
            m.op       = O_EXIT;
            m.mnemonic = "EXIT";
            m.word     = mx_set_guard(0xe30000000000000fULL, pred, pred_inv);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_VOTE: {
            u8 ps0 = remap_pred(stats, (u8)((hi >> 33) & 7u), false, &m.up_remapped_mask, 2);
            u8 pd0 = remap_pred(stats, (u8)((hi >> 27) & 7u), false, &m.up_remapped_mask, 4);
            u32 mode = (u32)((hi >> 16) & 3u);
            if (mode >= 3) mode = 0;
            m.op       = O_VOTE;
            m.mnemonic = "VOTE";
            m.word     = mx_set_guard(0x50d8000000000000ULL, pred, pred_inv);
            m.word    |= (u64)rd | ((u64)(ps0 & 7u) << 39) | ((u64)(pd0 & 7u) << 45) | ((u64)mode << 48);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        case AMP_OP_SHFL: {
            u8 pd0 = remap_pred(stats, (u8)((hi >> 27) & 7u), false, &m.up_remapped_mask, 2);
            u32 mode = (u32)((hi >> 16) & 3u);
            m.op       = O_SHFL;
            m.mnemonic = "SHFL";
            m.word     = mx_set_guard(0xef10000000000000ULL, pred, pred_inv);
            m.word     = mx_set_rd_ra(m.word, rd, ra);
            m.word    |= ((u64)rb << 20) | ((u64)mode << 30) | ((u64)rc << 39) | ((u64)(pd0 & 7u) << 48);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }

        default: {
            stats->unknown_opcodes++;
            m.op       = O_NOP;
            m.mnemonic = "UNKNOWN->NOP";
            m.word     = mx_set_guard(0x50b0000000000000ULL, pred, pred_inv);
            snprintf(m.note, sizeof(m.note), "unrecognized opc12=0x%03x base9=0x%03x", opc12, base9);
            AMP_WARN("[AMPERE-DECODE] Inst #%u (pc=0x%x): unknown SM86 opcode 0x%03x (base 0x%03x, lo=0x%016" PRIx64
                     " hi=0x%016" PRIx64 ") -> lowered to NOP",
                     idx, idx * 16, opc12, base9, lo, hi);
            append_minst(out_list, io_count, max_count, &m);
            break;
        }
    }

    (void)stage;
    return *io_count - start_count;
}

/* ============================================================================
 * 6. Full Ampere -> Maxwell IR (MInst) -> Canonical Maxwell 64-bit Stream
 * ============================================================================ */

u8* ampere_lower_to_maxwell(const u8* code,
                            u32 max_bytes,
                            ShaderStage stage,
                            u32* out_max_bytes,
                            AmpereLowerStats* out_stats,
                            MInst* out_minsts,
                            u32 max_out_minsts) {
    AmpereLowerStats local_stats;
    memset(&local_stats, 0, sizeof(local_stats));
    for (int i = 0; i < 64; i++) local_stats.ur_to_r[i] = 255;
    for (int i = 0; i < 8; i++)  local_stats.up_to_p[i] = 7;

    if (!ampere_is_sm86_shader(code, max_bytes, stage)) {
        if (out_stats) *out_stats = local_stats;
        return NULL;
    }

    local_stats.is_ampere = true;

    /* Step 1: Parse or synthesize 80-byte SPH header */
    u32 sph[20];
    memset(sph, 0, sizeof(sph));
    u32 sph0 = snx_ld_le32(code);
    u32 sph_type = sph0 & 0x1fu;
    u32 sph_ver  = (sph0 >> 5) & 0x1fu;
    u32 sph_stg  = (sph0 >> 10) & 0xfu;

    if (sph0 == 0x36384d53u || (sph_type <= 2 && sph_stg <= 6 && sph0 != 0)) {
        local_stats.has_sph     = true;
        local_stats.sph_version = sph_ver;
        local_stats.code_offset = 80;
        for (int i = 0; i < 20; i++) {
            sph[i] = snx_ld_le32(code + i * 4);
        }
        /* Normalize SPH header word 0 to canonical Maxwell SPH v3 if tagged with 'SM86' */
        if (sph0 == 0x36384d53u) {
            u32 hw_stg = stage == ST_VS ? 1 : stage == ST_TCS ? 2 : stage == ST_TES ? 3 : stage == ST_GS ? 4 : 5;
            sph[0] = 1u | (3u << 5) | (hw_stg << 10) | (1u << 14) | (1u << 26);
        }
    } else {
        local_stats.has_sph     = false;
        local_stats.sph_version = 3;
        local_stats.code_offset = 0;
        u32 hw_stg = stage == ST_VS ? 1 : stage == ST_TCS ? 2 : stage == ST_TES ? 3 : stage == ST_GS ? 4 : 5;
        sph[0] = 1u | (3u << 5) | (hw_stg << 10) | (1u << 14) | (1u << 26);
        if (stage == ST_FS && sph[18] == 0) sph[18] = 0xfu; /* Default RT0.rgba */
    }

    AMP_LOG("================================================================================");
    AMP_LOG("[AMPERE-SPH] Begin Ampere SM86 -> Maxwell IR Lowering (stage=%s, max_bytes=%u, has_sph=%d, code_off=%u)",
            stage_name(stage), max_bytes, local_stats.has_sph ? 1 : 0, local_stats.code_offset);
    AMP_LOG("[AMPERE-SPH] SPH[0]=0x%08x SPH[1]=0x%08x (lmem_pos=%u) SPH[5]=0x%08x SPH[18]=0x%08x (rt_mask)",
            sph[0], sph[1], sph[1] & 0xffffffu, sph[5], sph[18]);

    const u8* insn_bytes = code + local_stats.code_offset;
    u32 avail_bytes = max_bytes - local_stats.code_offset;
    u32 max_insts = avail_bytes / 16;
    if (max_insts > AMPERE_MAX_INSTS) max_insts = AMPERE_MAX_INSTS;

    /* Step 2: Pre-scan register pressure and map UR0..UR63 -> R, UP0..UP6 -> P */
    u32 inst_count = scan_and_allocate_registers(insn_bytes, max_insts, stage, sph, &local_stats);
    local_stats.ampere_inst_count = inst_count;

    AMP_LOG("[AMPERE-SCAN] Scanned %u Ampere 128-bit instructions: vec_GPRs=%u, uni_GPRs=%u (spilled=%u), uni_Preds=%u",
            inst_count, local_stats.vec_reg_count, local_stats.uni_reg_count,
            local_stats.uni_reg_spilled, local_stats.uni_pred_count);

    /* Step 3: Decode 128-bit Ampere instructions into MInst[] IR array */
    MInst* minsts = (MInst*)calloc(AMPERE_MAX_MINSTS, sizeof(MInst));
    u32* amp_idx_to_first_minst = (u32*)calloc(inst_count + 2, sizeof(u32));
    if (!minsts || !amp_idx_to_first_minst) {
        free(minsts);
        free(amp_idx_to_first_minst);
        return NULL;
    }

    u32 minst_count = 0;
    bool has_uncond_exit = false;

    for (u32 i = 0; i < inst_count; i++) {
        amp_idx_to_first_minst[i] = minst_count;
        u64 lo = snx_ld_le64(insn_bytes + i * 16);
        u64 hi = snx_ld_le64(insn_bytes + i * 16 + 8);

        u32 before = minst_count;
        u32 emitted = decode_and_lower_ampere_inst(i, lo, hi, stage, &local_stats, minsts, &minst_count, AMPERE_MAX_MINSTS);

        for (u32 k = 0; k < emitted && (before + k) < AMPERE_MAX_MINSTS; k++) {
            const MInst* mi = &minsts[before + k];
            AMP_TRACE("  [SM86 #%03u pc=0x%04x] lo=0x%016" PRIx64 " hi=0x%016" PRIx64
                      "  ==>  MInst[#%03u] %-16s word=0x%016" PRIx64 " (Rd=%u Ra=%u Rb=%u Rc=%u @%sP%u) %s",
                      i, i * 16, lo, hi, before + k, mi->mnemonic, mi->word,
                      mi->rd, mi->ra, mi->rb, mi->rc, mi->pred_inv ? "!" : "", mi->pred, mi->note);
            if (mi->op == O_EXIT && mi->pred == 7 && !mi->pred_inv) {
                has_uncond_exit = true;
            }
        }
    }
    amp_idx_to_first_minst[inst_count] = minst_count;

    /* Ensure the MInst stream always terminates with an unconditional EXIT */
    if (!has_uncond_exit && minst_count < AMPERE_MAX_MINSTS) {
        MInst exit_inst;
        memset(&exit_inst, 0, sizeof(exit_inst));
        exit_inst.op       = O_EXIT;
        exit_inst.word     = 0xe30000000007000fULL;
        exit_inst.orig_idx = inst_count;
        exit_inst.orig_pc  = inst_count * 16;
        exit_inst.pred     = 7;
        exit_inst.mnemonic = "EXIT (synthesized)";
        minsts[minst_count++] = exit_inst;
        AMP_LOG("[AMPERE-LOWER] Appended synthesized unconditional EXIT at MInst #%u", minst_count - 1);
    }

    local_stats.minst_count = minst_count;

    /* Step 4: Map each MInst index to its canonical Maxwell 64-bit word index
     * In Maxwell:
     *   Words 0..9   = 80-byte SPH header
     *   Word 10      = Scheduling control word for bundle 0
     *   Words 11..13 = Up to 3 instructions in bundle 0
     *   Word 14      = Scheduling control word for bundle 1
     *   Words 15..17 = Up to 3 instructions in bundle 1
     * Formula: for MInst m_idx (0-based):
     *   bundle = m_idx / 3
     *   slot   = m_idx % 3
     *   maxwell_word_idx = 10 + bundle * 4 + 1 + slot
     */
    u32* minst_to_mx_word = (u32*)calloc(minst_count + 2, sizeof(u32));
    if (!minst_to_mx_word) {
        free(minsts);
        free(amp_idx_to_first_minst);
        return NULL;
    }

    for (u32 m_idx = 0; m_idx <= minst_count; m_idx++) {
        u32 bundle = m_idx / 3;
        u32 slot   = m_idx % 3;
        minst_to_mx_word[m_idx] = 10 + bundle * 4 + 1 + slot;
    }

    /* Step 5: Resolve PC-relative branch targets (BRA, BSSY->SSY, CAL) into Maxwell word offsets!
     * Recall bra_target(u64 w, u32 idx) in translator.h:
     *   target_word = (u32)((i32)idx + 1 + (((i32)(B(w, 20, 24) << 8) >> 8) / 8))
     *   if ((target_word - 10) % 4 == 0) target_word++;
     * Therefore, to make bra_target(w, cur_word) land on target_word:
     *   rel_bytes = (s32)((s32)target_word - ((s32)cur_word + 1)) * 8
     *   w[43:20]  = (u32)rel_bytes & 0xffffffu
     */
    for (u32 m_idx = 0; m_idx < minst_count; m_idx++) {
        MInst* mi = &minsts[m_idx];
        if (!mi->is_pc_rel_branch) continue;

        u32 target_amp_idx = (u32)(mi->target_byte_pc / 16);
        if (target_amp_idx > inst_count) target_amp_idx = inst_count > 0 ? inst_count - 1 : 0;
        u32 target_minst_idx = amp_idx_to_first_minst[target_amp_idx];
        if (target_minst_idx >= minst_count) target_minst_idx = minst_count - 1;

        u32 cur_mx_word    = minst_to_mx_word[m_idx];
        u32 target_mx_word = minst_to_mx_word[target_minst_idx];
        s32 mx_rel_bytes   = ((s32)target_mx_word - ((s32)cur_mx_word + 1)) * 8;

        mi->word &= ~(0xffffffull << 20);
        mi->word |= ((u64)((u32)mx_rel_bytes & 0xffffffu) << 20);

        u32 verified_target = bra_target(mi->word, cur_mx_word);
        local_stats.branch_fixups++;

        AMP_LOG("[AMPERE-BRANCH] Fixed %s at MInst #%u (mx_word=%u): amp_pc=0x%x (amp_idx=%u) -> "
                "target_minst=#%u (target_mx_word=%u, verified=%u, rel_bytes=%d)",
                mi->mnemonic, m_idx, cur_mx_word, (u32)mi->target_byte_pc, target_amp_idx,
                target_minst_idx, target_mx_word, verified_target, mx_rel_bytes);
    }

    /* Step 6: Pack into canonical Maxwell 64-bit instruction stream */
    u32 num_bundles = (minst_count + 2) / 3;
    u32 total_mx_words = 10 + num_bundles * 4;
    u32 total_mx_bytes = total_mx_words * 8;

    u8* out_buf = (u8*)calloc(total_mx_words + 4, 8);
    if (!out_buf) {
        free(minsts);
        free(amp_idx_to_first_minst);
        free(minst_to_mx_word);
        return NULL;
    }

    /* Write 80-byte SPH header (words 0..9) */
    for (int i = 0; i < 20; i++) {
        memcpy(out_buf + i * 4, &sph[i], 4);
    }

    /* Canonical Maxwell neutral scheduling control word (stall=1, yield=1, no barrier wait) */
    const u64 k_maxwell_sched_word = 0x001f8000fc0007e1ULL;
    const u64 k_maxwell_nop_word   = 0x50b0000000070000ULL;

    for (u32 b = 0; b < num_bundles; b++) {
        u32 sched_word_idx = 10 + b * 4;
        memcpy(out_buf + sched_word_idx * 8, &k_maxwell_sched_word, 8);

        for (u32 s = 0; s < 3; s++) {
            u32 m_idx = b * 3 + s;
            u32 insn_word_idx = sched_word_idx + 1 + s;
            u64 w = (m_idx < minst_count) ? minsts[m_idx].word : k_maxwell_nop_word;

            /* Verify every emitted 64-bit word with decode(w) */
            const Op* decoded_op = decode(w);
            if (!decoded_op) {
                AMP_WARN("[AMPERE-VERIFY] Emitted Maxwell word at idx=%u (0x%016" PRIx64
                         " from %s) failed decode()! Replacing with NOP.",
                         insn_word_idx, w, m_idx < minst_count ? minsts[m_idx].mnemonic : "PAD");
                local_stats.unsupported_emitted++;
                w = k_maxwell_nop_word;
            }
            memcpy(out_buf + insn_word_idx * 8, &w, 8);
        }
    }

    local_stats.maxwell_word_count = total_mx_words;
    local_stats.maxwell_byte_count = total_mx_bytes;

    AMP_LOG("[AMPERE-SUMMARY] Lowering complete for stage=%s: "
            "%u Ampere 128b insts -> %u MInsts (+%u micro-ops) -> %u Maxwell 64b words (%u bytes), "
            "UR remapped=%u, UP remapped=%u, branch fixups=%u, unknown->NOP=%u, verify_err=%u",
            stage_name(stage),
            local_stats.ampere_inst_count,
            local_stats.minst_count,
            local_stats.expanded_micro_ops,
            local_stats.maxwell_word_count,
            local_stats.maxwell_byte_count,
            local_stats.uni_reg_count,
            local_stats.uni_pred_count,
            local_stats.branch_fixups,
            local_stats.unknown_opcodes,
            local_stats.unsupported_emitted);
    AMP_LOG("================================================================================");

    if (out_minsts && max_out_minsts > 0) {
        u32 to_copy = minst_count < max_out_minsts ? minst_count : max_out_minsts;
        memcpy(out_minsts, minsts, to_copy * sizeof(MInst));
    }
    if (out_max_bytes) *out_max_bytes = total_mx_bytes;
    if (out_stats)     *out_stats     = local_stats;

    free(minsts);
    free(amp_idx_to_first_minst);
    free(minst_to_mx_word);
    return out_buf;
}
