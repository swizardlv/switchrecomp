# Ampere (SM86) -> Maxwell IR -> SPIR-V 实现指南与现有代码盘点

## 1. 核心架构结论：为什么选择“Ampere -> Maxwell 级归一化 IR -> SPIR-V”？

在 Switch 2（NVIDIA T239 / Ampere GA10B，SM86）向 Vulkan / SPIR-V（或 PS5 / PC 后端）转译的过程中，**不要把 128-bit Ampere 机器码重新打包成 64-bit Maxwell 机器码二进制**（会因 `UR0..UR63` 寄存器位宽、移除的 1-in-4 调度字以及跳转偏移重算而产生大量无用功），而是采用**解码层降级（Normalized Maxwell-Level IR，`MInst`）**：

```text
           ┌───────────────────────────────────┐
           │ Switch 1: Maxwell GM20B (64-bit)  │
           │  - 80B SPH Header + 1-in-4 调度字  │
           └─────────────────┬─────────────────┘
                             │ frontend_maxwell.c (提取现有 B(w, lo, n) 逻辑)
                             ▼
                     ┌───────────────┐
                     │  MProgram /   │  <-- 统一的 Maxwell 级语义指令数组 (MInst[])
                     │   MInst[]     │      复用现有的 213 个 O_* 枚举 + 扩展寄存器编号
                     └───────┬───────┘
                             ▲
                             │ frontend_ampere.c (解析 128-bit SM86 并降级为 O_*)
           ┌─────────────────┴─────────────────┐
           │ Switch 2: Ampere GA10B (128-bit)  │
           │  - 128B SPH Header + 连续 128b 指令│
           └───────────────────────────────────┘
                             │
                             ▼ (100% 复用现有后端分析与生成管线)
           ┌───────────────────────────────────┐
           │ 现有 CFG / 静态分析 / SPIR-V 后端   │
           │  - shader.c (CRS/BRX/CFG)         │
           │  - spirv_translate.c (VS..CS)     │
           │  - spirv_compute.c / spirv_emit.c │
           └───────────────────────────────────┘
```

这一方案的核心优势在于：**Ampere SM86 新增的绝大多数常用标量/三操作数指令（如 `IADD3`、`LOP3`、`IMAD`、`SHF`、`LEA`），其实早在 Maxwell 后期的 `maxwell_opcodes.def` 和 `spirv_translate.c` 中就已经完整实现了！**

---

## 2. 仓库现有实现完整盘点（What Is Already Implemented）

当前仓库中共有两套 Maxwell 着色器转译管线，其中 **`runtime/snxvk/src/nx/gpu/shader/` 是功能最全、结构最现代的 Maxwell -> SPIR-V 核心资产**。

### 2.1 核心模块清单与职责

| 文件路径 | 代码规模 | 已实现功能 | 在 Ampere -> Maxwell IR 架构中的角色 |
| :--- | :--- | :--- | :--- |
| `runtime/snxvk/src/nx/gpu/shader/maxwell_opcodes.def` | 213 条编码模式 | 覆盖 ~95 个 Maxwell 指令族（算术、逻辑、纹理、图像、原子、控制流、子组）的掩码与枚举 `O_*` | **直接复用**：`O_*` 枚举直接作为 `MInst.op` 的指令集定义 |
| `runtime/snxvk/src/nx/gpu/shader/decode.c` | 表格驱动解码器 | 将 64-bit Maxwell 字匹配为 `Opcode` 枚举 | **保留为 Maxwell 前端**，并为 Ampere 新增同构的 `decode_ampere.c` |
| `runtime/snxvk/src/nx/gpu/shader/shader.h` | 核心数据结构 | 定义 `ShaderStage`、`ShaderInfo`（SPH 属性/插值模式）、`BasicBlock`、`Shader`、`ShaderKey` | **小幅扩展**：增加 `MInst` 结构体定义，扩展寄存器上限以容纳 `UR0..UR63` |
| `runtime/snxvk/src/nx/gpu/shader/shader.c` | 24 KB | SPH 头部解析 (`sph_parse`)、代码块扫描 (`shader_code_words`)、`BRX` 跳转表静态追踪 (`shader_brx_targets`)、CRS 汇聚栈静态配对 (`shader_resolve_crs`)、基本块 CFG 构建 (`shader_new`)、`VertexA+VertexB` 双顶点着色器合并 (`shader_merge_dual`) | **95% 复用**：将直接读取 `insn[i]` 的位域宏改为读取 `MInst[i]` 即可同时支持 Maxwell 与 Ampere |
| `runtime/snxvk/src/nx/gpu/shader/spirv_translate.c` | 402 KB | 完整的 6 阶段（`VS`/`TCS`/`TES`/`GS`/`FS`/`CS`）Maxwell -> SPIR-V 编译器，含静态/动态控制流、SSBO 指针溯源、Bindless 纹理句柄追踪、属性 I/O、Transform Feedback | **核心复用资产**：无需重写任何 SPIR-V 生成逻辑，只需将操作数提取从 `B(w, ...)` 切换到 `MInst` 字段 |
| `runtime/snxvk/src/nx/gpu/shader/spirv_compute.c` | 20 KB | 针对无分支线性 Compute Shader 的快速 SPIR-V 生成通道 | **可选复用或合并入主通道** |
| `runtime/snxvk/src/nx/gpu/shader/spirv_emit.c` / `.h` | 94 KB | 底层 SPIR-V 二进制构建器 (`SpvEmitter`)，封装类型去重、常量池、装饰器、Phi 节点、GLSL.std.450 扩展指令 | **100% 原样复用（零修改）** |
| `runtime/snxvk_glue.c` | 12 KB | 运行时着色器缓存与 `snx_translate_shader` 入口封装 | **原样复用**：根据 SPH 魔数自动分流 Maxwell / Ampere 前端 |
| `runtime/shader.c` / `runtime/shader_proc.c` | 122 KB | 早期 Switch 1 Maxwell -> GLSL 4.30 源码转译器（供 OpenGL 后端使用） | **参考/兼容保留**：SPIR-V 路线以 `snxvk` 为准 |

---

### 2.2 `maxwell_opcodes.def` 与 `spirv_translate.c` 已实现的指令族清单

以下是 `spirv_translate.c` 中**已经完整实现 SPIR-V 翻译**的指令族。这也是为什么“把 Ampere 降级到 Maxwell IR”极度划算——因为 Ampere 编译器大量生成的指令（下表加粗项）在现有代码中已有现成实现：

1. **整数算术、位移与三操作数指令（Ampere 极高频使用，已实现！）**
   - 基础加减与比较：`IADD` (`R/C/I/32I`)、`ISCADD` (`R/C/I/32I`)、`IMNMX` (`R/C/I`)、`ISETP` (`R/C/I`)、`ISET` (`R/C/I`)、`ICMP` (`R/RC/CR/I`)
   - **三输入加法 `IADD3` (`O_IADD3_R` / `O_IADD3_C` / `O_IADD3_I`)**：含进位输出 `CC` 与全修饰符支持（Ampere 彻底用 `IADD3` 取代了 `IADD`）
   - **三输入逻辑查表 `LOP3` (`O_LOP3_R` / `O_LOP3_C` / `O_LOP3_I`)**：已实现完整的 8-bit `imm8` 真值表分解器（支持常见掩码快速路径与通用布尔最小项展开，Ampere 用 `LOP3` 取代了 `LOP`）
   - **整数乘加 `IMAD` (`O_IMAD_R/RC/CR/I/32I`) 与 `IMUL`**：已支持 32 位低位乘加及 `.HI`（64 位高精度高 32 位提取，含 `OpSMulExtended` / `OpUMulExtended` + 64 位累加）
   - **漏斗移位 `SHF` (`O_SHF_LR/LI/RR/RI`)**：已支持 64 位拼接左移/右移及 `.HI` 高位模式（Ampere 常用 `SHF` 取代 `SHL`/`SHR`）
   - **有效地址计算 `LEA` (`O_LEA_HR/HC/LR/LC/LI`)**：已实现高低位带移位地址计算及进位链
   - 位操作与字节重排：`LOP` (`R/C/I/32I`)、`SHL` (`R/C/I`)、`SHR` (`R/C/I`)、`BFE` (`R/C/I`)、`BFI` (`R/RC/CR/I`)、`FLO` (`R/C/I` 找最高有效位)、`POPC` (`R/C/I` 计数置位)、`PRMT` (`R/RC/CR/I` 字节置换)、`SEL` (`R/C/I` 谓词选择)、`XMAD` (`R/RC/CR/I` 16 位交叉乘加)

2. **FP32 / FP16 / FP64 浮点指令（全部已实现）**
   - FP32：`FADD` (`R/C/I/32I`)、`FMUL` (`R/C/I/32I`)、`FFMA` (`R/RC/CR/I/32I`)、`FMNMX` (`R/C/I`)、`FSET` (`R/C/I`)、`FSETP` (`R/C/I`)、`FCMP` (`R/RC/CR/I`)、`FSWZADD`、`RRO`、`FCHK`
   - 超越函数 `MUFU`：`COS`、`SIN`、`EX2`、`LG2`、`RCP`、`RSQ`、`RCP64H`、`RSQ64H`、`SQRT`
   - FP16 打包双半精度：`HADD2` (`R/C/I/32I`)、`HMUL2` (`R/C/I/32I`)、`HFMA2` (`R/RC/CR/I/32I`)、`HSET2` (`R/C/I`)、`HSETP2` (`R/C/I`)
   - FP64 双精度：`DADD` (`R/C/I`)、`DMUL` (`R/C/I`)、`DFMA` (`R/RC/CR/I`)、`DMNMX` (`R/C/I`)、`DSET` (`R/C/I`)、`DSETP` (`R/C/I`)

3. **类型转换、数据移动与系统寄存器（全部已实现）**
   - 转换：`F2F` (`R/C/I`，支持 FP16/FP32/FP64 互转与舍入模式)、`F2I` (`R/C/I`)、`I2F` (`R/C/I`)、`I2I` (`R/C/I`)
   - 移动与系统态：`MOV` (`R/C/I/32I`)、`S2R`（读取 `SR_LANEID`、`SR_TID_X/Y/Z`、`SR_CTAID_X/Y/Z`、`SR_EQMASK` 等系统寄存器）、`CS2R`、`P2R`、`R2P`、`PSETP`、`PSET`、`CSETP`、`CSET`

4. **内存加载/存储与原子操作（全部已实现）**
   - 常量缓冲：`LDC`（支持 `U8`/`S8`/`U16`/`S16`/`B32`/`B64`、动态寄存器索引 `c[bank][Rn + imm]`）
   - 全局内存 SSBO：`LDG`、`STG`（支持 `U8`..`B128` 向量化读写，含静态描述符溯源 + 动态地址 `PhysicalStorageBuffer` / `global_dynamic` 兜底）
   - 共享与本地内存：`LDS`、`STS`（Compute Workgroup Shared Memory，支持 `U8`..`B128`）、`LDL`、`STL`（线程私有栈数组 `local_mem`）、`LD`、`ST`
   - 原子与归约：`ATOM`、`ATOM_CAS`、`ATOMS`、`ATOMS_CAS`、`RED`（支持 `ADD`/`MIN`/`MAX`/`INC`/`DEC`/`AND`/`OR`/`XOR`/`EXCH`）

5. **纹理采样、图像读写与几何/片元属性 I/O（全部已实现）**
   - 纹理采样：`TEX`、`TEX_B`、`TEXS`、`TLD`、`TLD_B`、`TLDS`、`TLD4`、`TLD4_B`、`TLD4S`、`TMML`、`TMML_B`、`TXD`、`TXD_B`、`TXQ`、`TXQ_B`、`TXA`（支持 1D/2D/3D/Cube/Array/Shadow、Gather 偏移、LOD clamp、Bindless 动态句柄数组 `SNX_VKB_TEXARRAY`）
   - Surface / Storage Image：`SULD`、`SUST`、`SURED`、`SUATOM`、`SUATOM_CAS`
   - 阶段属性与片元内插：`IPA`（支持透视、平直、线性及 `PASS`/`SC`/`OFFSET` 采样位置修饰）、`ALD`、`AST`、`AL2P`、`ISBERD`、`OUT`（GS `EmitVertex`/`EndPrimitive`）、`PIXLD`（`SampleId`、`CovMask`）

6. **控制流、屏障与 Warp 子组指令（全部已实现）**
   - 分支与汇聚：`BRA`、`BRX`（间接跳转表）、`SSY`/`SYNC`、`PBK`/`BRK`、`PCNT`/`CONT`、`PEXIT`/`EXIT`、`CAL`/`PRET`/`RET`、`JMP`、`KIL`（支持 `OpDemoteToHelperInvocationEXT` 或 `OpKill`）
   - 屏障：`BAR`（Workgroup `OpControlBarrier`）、`MEMBAR`（`OpMemoryBarrier`）、`DEPBAR`
   - 子组（Warp）：`VOTE` / `VOTE_VTG`（`ALL`/`ANY`/`EQ` + `OpGroupNonUniformBallot`）、`SHFL`（`IDX`/`UP`/`DOWN`/`BFLY` 跨线程洗牌）

---

### 2.3 现有静态分析Pass（可直接作用于 Ampere 降级后的 IR）

在 `shader.c` 与 `spirv_translate.c` 中，已经编写了四套极其关键的静态分析算法，只要把输入换成 `MInst[]`，Ampere 着色器就能免费获得这些能力：

1. **CRS 静态汇聚栈分析 + 动态汇聚栈兜底 (`shader_resolve_crs` + `dynamic_pop`)**
   - 静态模拟 `SSY`/`PBK`/`PCNT`/`CAL` 压栈与 `SYNC`/`BRK`/`CONT`/`RET` 弹栈。
   - 当遇到复杂 CFG 时，自动启用 SPIR-V 运行时栈 + `OpGroupNonUniformUMin(Subgroup, next_pc)` 实现精确的 SIMT 线程重汇聚。
   - **对 Ampere 的意义**：Ampere 废除了隐式 CRS 栈，改用显式屏障寄存器 `BSSY Bn, target` 和 `BSYNC Bn`。只要在 Ampere 前端把 `BSSY` 映射为 `O_SSY`、`BSYNC` 映射为 `O_SYNC`，现有的整套汇聚分析与 SPIR-V 重汇聚代码即可**零成本接管**！
2. **全局内存（SSBO）基址溯源分析 (`gtags[]` 数据流传播)**
   - 自动追踪从常量缓冲 `c[0][0x110..0x830]` 加载的 64 位指针经过 `MOV`、`LDC`、`IADD`、`IADD3`、`ISCADD`、`LEA` 后的 `GTag(slot, bias)`，将 64 位裸指针访存还原为标准的 Vulkan SSBO 索引访问；对无法静态分析的指针提供 `global_dynamic` 兜底。
3. **Bindless / 动态纹理句柄支配树追踪 (`direct_reaching_def` / `direct_indexed_handle`)**
   - 在 `TEX_B` / `TLD_B` 使用寄存器传入句柄时，沿 CFG 支配树回溯 `LDC`、`MOV`、`IADD`/`IADD3`、`SHL`/`SHR`/`BFE` 等句柄拼接链，自动生成 `SNX_VKB_TEXARRAY` 动态索引采样。
4. **`BRX` 间接跳转表静态解析 (`shader_brx_targets`)**
   - 追踪索引寄存器的掩码/加减链并读取常量缓冲跳转表，将间接跳转展开为 SPIR-V `OpSwitch`。

---

## 3. 当前代码与 Maxwell 64-bit 编码的耦合点（需要解耦的 3 处地方）

目前 `shader.c` 和 `spirv_translate.c` 直接读取 `const u64 *insn`，存在以下 3 处与 Maxwell 64-bit 强绑定的假设：

1. **1-in-4 调度字过滤宏 `IDX_OK(i)`**：
   - 现有代码：`#define IDX_OK(i) ((i) >= 11 && ((i) - 10) % 4 != 0)`（跳过前 10 个字的 80 字节 SPH 头部，以及每 4 个字中的第 1 个调度控制字）。
   - Ampere 差异：Ampere 的每条指令占 128-bit（2 个 `u64`），调度信息内嵌在每条指令的高 64 位高字节中，不存在独立的每 4 字调度字。
2. **内联位域提取 `B(w, lo, n)`**：
   - 现有代码在 `spirv_translate.c` 的每个 `case O_...:` 分支中直接调用 `B(w, 0, 8)` 取 `rd`、`B(w, 8, 8)` 取 `ra`、`B(w, 20, 19)` 取常量缓冲或寄存器操作数。
   - Ampere 差异：Ampere 的 `rd`/`ra`/`rb`/`rc` 位于 128-bit 字的不同偏移处，且立即数和常量缓冲均为完整 32-bit / 独立字段。
3. **寄存器与谓词编号上限**：
   - 现有代码：`FnCtx` 中 `u32 regs[256]`（`0..254` 为 `R0..R254`，`255` 为 `RZ`）和 `u32 preds[8]`（`0..6` 为 `P0..P6`，`7` 为 `PT`）。
   - Ampere 差异：Ampere 增加了 Uniform Datapath，拥有 64 个统一通用寄存器 `UR0..UR63`（外加 `URZ`）和 7 个统一谓词寄存器 `UP0..UP6`（外加 `UPT`）。

---

## 4. `MInst`（Maxwell 级归一化 IR）设计方案

为了在不重写 `spirv_translate.c` 的前提下同时支持 Maxwell 与 Ampere，我们引入统一的指令结构 `MInst`，让前端负责“解码 + 归一化”，后端只读 `MInst`。

### 4.1 扩展寄存器与谓词编码空间

将所有统一寄存器（Uniform Registers）直接映射到同一套寄存器文件的高位编号（因为 `URn` 在语义上就是所有线程值相同的标量 `Rn`，在 SPIR-V 中作为普通 `uint`/`int`/`float` 变量参与运算完全等价且合法）：

```c
// 通用寄存器编号空间 (u16, 共 322 个槽位)
enum {
    MREG_R0   = 0,     // 0..254: 线程寄存器 R0..R254
    MREG_RZ   = 255,   // 255:    零寄存器 RZ (读为 0，写丢弃)
    MREG_UR0  = 256,   // 256..319: Ampere 统一寄存器 UR0..UR63
    MREG_URZ  = 320,   // 320:    Ampere 统一零寄存器 URZ (映射到与 RZ 相同的 0 常量)
    MREG_COUNT = 322
};

// 谓词寄存器编号空间 (u8, 共 16 个槽位)
enum {
    MPRED_P0  = 0,     // 0..6:  线程谓词 P0..P6
    MPRED_PT  = 7,     // 7:     恒真谓词 PT
    MPRED_UP0 = 8,     // 8..14: Ampere 统一谓词 UP0..UP6
    MPRED_UPT = 15,    // 15:    Ampere 恒真统一谓词 UPT (映射到 PT)
    MPRED_COUNT = 16
};

static inline bool mreg_is_zero(u16 r) {
    return r == MREG_RZ || r == MREG_URZ;
}
static inline bool mpred_is_true(u8 p) {
    return p == MPRED_PT || p == MPRED_UPT;
}
```

> **收益**：在 `spirv_translate.c` 中，只需将 `FnCtx.regs[256]` 改为 `FnCtx.regs[MREG_COUNT]`、`FnCtx.preds[8]` 改为 `FnCtx.preds[MPRED_COUNT]`，并在初始化时将 `regs[MREG_URZ] = regs[MREG_RZ]`、`preds[MPRED_UPT] = preds[MPRED_PT]`，**所有 Ampere 的 `U*` 统一指令（`UIADD3`、`ULDC`、`UMOV`、`ULOP3`、`UISETP`、`USHF` 等）就能直接当作普通 `O_IADD3`、`O_LDC`、`O_MOV`、`O_LOP3`、`O_ISETP`、`O_SHF` 执行，无需在 SPIR-V 后端多写一行 `case`！**

### 4.2 `MInst` 结构体定义

```c
typedef enum {
    MSRC_NONE = 0,
    MSRC_REG,      // 寄存器 (R0..R255 或 UR0..UR63)
    MSRC_CB,       // 常量缓冲 c[bank][offset] (可带寄存器索引 ra)
    MSRC_IMM32,    // 32-bit 立即数 (已由前端完成 19/20-bit 符号/浮点扩展)
} MSrcKind;

typedef struct {
    MSrcKind kind;
    u16      reg;       // MSRC_REG 时的寄存器号
    u8       cb_bank;   // MSRC_CB 时的常量槽号 (0..17)
    s32      cb_off;    // MSRC_CB 时的字节偏移
    u32      imm32;     // MSRC_IMM32 时的 32 位立即数位模式
    bool     neg;       // 源操作数取反修饰符 (-src / ~src)
    bool     abs;       // 源操作数绝对值修饰符 (|src|)
    bool     h1;        // 16-bit 高半字选择修饰符
} MOperand;

typedef struct {
    u16      op;        // 复用 maxwell_opcodes.def 中的 Opcode (O_*)
    u32      orig_idx;  // 原始机器码索引（用于调试日志与 PC 映射）
    u32      pc_off;    // 相对于代码段起点的字节偏移

    // 指令级谓词门控 (@P0 / @!P0 / @UP0)
    u8       guard_pred; // MPRED_*
    bool     guard_inv;

    // 目标操作数
    u16      rd;         // 主目标寄存器 (MREG_*)
    u16      rd2;        // 副目标寄存器（用于 LDC.64 / LDG.64 / TEX 双目标等）
    u8       pd0;        // 主目标谓词 (ISETP/FSETP/TXQ 等)
    u8       pd1;        // 副目标谓词

    // 最多 3 个源操作数 (ra = src[0], rb = src[1], rc = src[2])
    MOperand src[3];
    u8       ps0;        // 源谓词 0 (ISETP/FSETP/SEL/PSETP 等)
    u8       ps1;        // 源谓词 1
    u8       ps2;        // 源谓词 2 (PSETP 等)
    bool     ps0_inv, ps1_inv, ps2_inv;

    // 通用修饰符与子操作码字段 (由前端从 Maxwell/Ampere 各自位域解码填充)
    struct {
        bool     sat;       // .SAT 饱和修饰
        bool     ftz;       // .FTZ 非规格化刷零
        bool     cc_write;  // .CC 写条件码 (bit 47 in Maxwell)
        bool     x_mode;    // .X 进位扩展模式 (IADD/IADD3/ISCADD/SUB)
        bool     is_signed; // .S32 / .U32 符号标志
        bool     is_hi;     // .HI 高位模式 (IMAD.HI / SHF.HI / LEA.HI)
        u8       rnd;       // 舍入模式 (RN/RM/RP/RZ)
        u8       cmp_op;    // 比较操作符 (LT/EQ/LE/GT/NE/GE/...)
        u8       bool_op;   // 谓词组合布尔操作 (AND/OR/XOR)
        u8       lop_op;    // LOP 操作或 LOP3 的 8-bit imm8 真值表
        u8       subop;     // 子操作码 (MUFU op / SHFL mode / VOTE mode / ATOM op / IPA mode)
        u8       mem_size;  // 访存位宽枚举 (U8/S8/U16/S16/B32/B64/B128)
        u8       shift;     // ISCADD / LEA / VSHL 移位量 (0..31)
        u8       tex_dim;   // 纹理维度 (1D/2D/3D/Cube/Array)
        u8       tex_mask;  // 纹理/TLD4 组件掩码 (4-bit RGBA)
        bool     tex_ndv;   // 纹理强制非衍生计算 / Shadow / DC / AOFFI 等打包标志
        u8       tex_flags; // DC / AOFFI / LC / NDV / NODEP 位掩码
        u16      tex_cb_idx;// 绑定纹理常量表索引 (tid)
        s32      target_idx;// 分支/SSY/PBK/PCNT/CAL 的目标 MInst 数组下标！
        u64      raw_lo;    // 保留原始低 64 位（供过渡期尚未完全迁移的极少数冷门指令回退）
        u64      raw_hi;    // 保留原始高 64 位（Ampere 128-bit 指令高字）
    } mod;
} MInst;
```

> **关键设计亮点：`mod.target_idx` 直接存放目标指令在 `MInst[]` 数组中的下标**。
> 在 Maxwell 中，分支偏移是按 64-bit 字（跳过调度字）计算的；在 Ampere 中，分支偏移是按 128-bit 指令（或 4 字节对齐偏移）计算的。前端解码时直接把 `BRA`、`SSY`、`PBK`、`PCNT`、`CAL` 的目标换算成 `MInst[]` 的数组下标 `target_idx`，后端 CFG 构建 (`shader.c`) 和 `spirv_translate.c` 就彻底摆脱了 `IDX_OK(i)` 和不同架构字节步长的困扰！

---

## 5. Ampere (SM86) -> Maxwell IR (`MInst`) 完整降级映射表

Ampere SM86 采用固定 128-bit 指令字（`u64 lo`, `u64 hi`），其中 `lo[11:0]` 为 12-bit 主操作码，`lo[15:12]` 为谓词门控（`guard_pred` + `guard_inv`），`lo[23:16]` 为 `rd`，`lo[31:24]` 为 `ra`，后续操作数根据编码格式（Reg-Reg、Reg-Imm32、Reg-Cbuf、Reg-UReg）分布在 `lo[63:32]` 与 `hi[63:0]`。

下面按类别列出 Ampere SM86 指令如何降级到现有的 `O_*`（`maxwell_opcodes.def`）：

### 5.1 1:1 直接映射指令（零信息损失，100% 复用现有 `spirv_translate.c`）

| Ampere SM86 指令 | 降级目标 (`maxwell_opcodes.def`) | 归一化要点 |
| :--- | :--- | :--- |
| `IADD3 Rd, Pout, Pin, Ra, Rb/Imm/CB, Rc` | `O_IADD3_R` / `O_IADD3_C` / `O_IADD3_I` | 提取 `ra, rb, rc` 及各自取反修饰符；若 Ampere 带进位谓词 `Pout`/`Pin`，映射为 `mod.cc_write` / `mod.x_mode` |
| `IMAD Rd, Ra, Rb/Imm/CB, Rc` | `O_IMAD_R` / `O_IMAD_RC` / `O_IMAD_I` / `O_IMAD_32I` | 当 `Ra == RZ` 或 `Rb == 1`（Ampere 用 `IMAD` 代替 `MOV`/`IADD` 的惯用手法）时，可直接原样发 `O_IMAD_*` 或化简为 `O_MOV_*` / `O_IADD_*` |
| `IMAD.HI Rd, Ra, Rb, Rc` | `O_IMAD_R` / `O_IMAD_RC` / `O_IMAD_I` (`mod.is_hi = true`) | `spirv_translate.c` 已完整支持 `IMAD.HI` 64 位高字提取 |
| `LOP3.LUT Rd, Ra, Rb/Imm/CB, Rc, imm8` | `O_LOP3_R` / `O_LOP3_C` / `O_LOP3_I` | 提取 `imm8` 真值表填入 `mod.lop_op`，现有 `spirv_translate.c` 自动分解布尔表达式 |
| `SHF.L / SHF.R Rd, Ra, Rb/Imm, Rc` | `O_SHF_LR` / `O_SHF_LI` / `O_SHF_RR` / `O_SHF_RI` | 提取 `.HI` / `.W` / 类型修饰填入 `mod.is_hi` |
| `LEA Rd, Pout, Ra, Rb/Imm/CB, shift` | `O_LEA_LR` / `O_LEA_LC` / `O_LEA_LI` / `O_LEA_HR` / `O_LEA_HC` | 提取 5-bit `shift` 与 `.HI` / `.X` 标志 |
| `ISETP.<cmp>.<bool> Pd0, Pd1, Ra, Rb/Imm/CB, Ps0` | `O_ISETP_R` / `O_ISETP_C` / `O_ISETP_I` | 映射 `cmp_op`、`bool_op`、`.EX` 扩展比较标志 |
| `SEL Rd, Ra, Rb/Imm/CB, Ps0` | `O_SEL_R` / `O_SEL_C` / `O_SEL_I` | 1:1 映射 |
| `BFE` / `FLO` / `POPC` / `PRMT` | `O_BFE_*` / `O_FLO_*` / `O_POPC_*` / `O_PRMT_*` | 1:1 映射 |
| `FADD` / `FMUL` / `FFMA` / `FMNMX` / `FSETP` / `MUFU` | `O_FADD_*` / `O_FMUL_*` / `O_FFMA_*` / `O_FMNMX_*` / `O_FSETP_*` / `O_MUFU` | Ampere 32-bit 浮点立即数统一转为 `O_FADD_32I` / `O_FMUL_32I` / `O_FFMA_32I` 或 `MSRC_IMM32` |
| `HADD2` / `HMUL2` / `HFMA2` / `HSETP2` | `O_HADD2_*` / `O_HMUL2_*` / `O_HFMA2_*` / `O_HSETP2_*` | 1:1 映射 |
| `DADD` / `DMUL` / `DFMA` / `DSETP` | `O_DADD_*` / `O_DMUL_*` / `O_DFMA_*` / `O_DSETP_*` | 1:1 映射 |
| `F2F` / `F2I` / `I2F` / `I2I` | `O_F2F_*` / `O_F2I_*` / `O_I2F_*` / `O_I2I_*` | 注意：Ampere 移除了单条 `F2F` 同时做“绝对值/取反 + 规模转换”的部分组合，本身是 Maxwell `F2F` 的子集，100% 兼容 |
| `MOV` / `S2R` / `CS2R` / `P2R` / `R2P` / `PLOP3` | `O_MOV_*` / `O_S2R` / `O_CS2R` / `O_P2R_*` / `O_R2P_*` / `O_PSETP` | `CS2R` 读取 `SRZ` 或 64 位特殊寄存器可直接在前端展开 |
| `LDC` / `LDG` / `STG` / `LDS` / `STS` / `LDL` / `STL` | `O_LDC` / `O_LDG` / `O_STG` / `O_LDS` / `O_STS` / `O_LDL` / `O_STL` | 1:1 映射访存位宽 `U8`..`B128` 与偏移量 |
| `ATOM` / `ATOMG` / `ATOMS` / `RED` | `O_ATOM` / `O_ATOM_CAS` / `O_ATOMS` / `O_ATOMS_CAS` / `O_RED` | 1:1 映射 |
| `IPA` / `ALD` / `AST` / `OUT` / `ISBERD` / `PIXLD` | `O_IPA` / `O_ALD` / `O_AST` / `O_OUT_*` / `O_ISBERD` / `O_PIXLD` | 1:1 映射 |
| `VOTE` / `SHFL` / `BAR` / `MEMBAR` | `O_VOTE` / `O_SHFL` / `O_BAR` / `O_MEMBAR` | Ampere `BAR.SYNC.DEFER_BLOCKING` 直接映射为 `O_BAR` |

---

### 5.2 Uniform Datapath 指令降级（`U*` 指令族 -> 普通 `O_*` + `URn`/`UPn` 寄存器号）

Ampere 引入了标量统一数据通路（Uniform Datapath）。在 `frontend_ampere.c` 中，只需将目标/源寄存器号加上 `MREG_UR0 (256)` 偏移、谓词加上 `MPRED_UP0 (8)` 偏移，并填入对应的普通 `O_*` 操作码即可：

| Ampere 统一指令 | 降级后的 `MInst.op` | 寄存器映射规则 |
| :--- | :--- | :--- |
| `UMOV URd, URa/Imm/CB` | `O_MOV_R` / `O_MOV_C` / `O_MOV_32I` | `rd = MREG_UR0 + urd` |
| `R2UR URd, Ra` | `O_MOV_R` | `rd = MREG_UR0 + urd`, `src[0].reg = MREG_R0 + ra` |
| `S2UR URd, SR_n` | `O_S2R` | `rd = MREG_UR0 + urd` |
| `ULDC[.64] URd, c[bank][URa + off]` | `O_LDC` | `rd = MREG_UR0 + urd`, `src[0].reg = MREG_UR0 + ura` |
| `UIADD3 URd, UPout, UPin, URa, URb, URc` | `O_IADD3_R` / `O_IADD3_C` / `O_IADD3_I` | 所有 `UR*` 映射至 `MREG_UR0 + n` |
| `UIMAD URd, URa, URb, URc` | `O_IMAD_R` / `O_IMAD_RC` / `O_IMAD_I` | 所有 `UR*` 映射至 `MREG_UR0 + n` |
| `ULOP3.LUT URd, URa, URb, URc, imm8` | `O_LOP3_R` / `O_LOP3_C` / `O_LOP3_I` | 所有 `UR*` 映射至 `MREG_UR0 + n` |
| `USHF.L / USHF.R URd, URa, URb, URc` | `O_SHF_LR` / `O_SHF_LI` / `O_SHF_RR` / `O_SHF_RI` | 所有 `UR*` 映射至 `MREG_UR0 + n` |
| `ULEA URd, UPout, URa, URb, shift` | `O_LEA_*` | 所有 `UR*` 映射至 `MREG_UR0 + n` |
| `UISETP UPd0, UPd1, URa, URb, UPs0` | `O_ISETP_R` / `O_ISETP_C` / `O_ISETP_I` | `pd0/pd1/ps0` 映射至 `MPRED_UP0 + p` |
| `USEL URd, URa, URb, UPs0` | `O_SEL_R` / `O_SEL_C` / `O_SEL_I` | `rd/src` 用 `MREG_UR0 + n`，`ps0` 用 `MPRED_UP0 + p` |
| `UBFE` / `UFLO` / `UPOPC` / `UPRMT` | `O_BFE_*` / `O_FLO_*` / `O_POPC_*` / `O_PRMT_*` | 所有 `UR*` 映射至 `MREG_UR0 + n` |
| `UPLOP3.LUT UPd0, UPd1, UPs0, UPs1, UPs2, imm8` | `O_PSETP`（或前端展开为谓词布尔组合） | `pd/ps` 映射至 `MPRED_UP0 + p` |

> **附带收益**：在 Switch 2 着色器中，SSBO 基址和 Bindless 纹理句柄几乎全部通过 `ULDC` 加载到 `URn` 中，并用 `UIADD3` / `ULEA` 计算偏移。将 `URn` 映射入统一寄存器空间后，现有 `spirv_translate.c` 中的 **SSBO 指针溯源 (`gtags[]`) 和 Bindless 纹理追踪 (`direct_indexed_handle`) 会自动穿透 `URn` 寄存器链**，无需重写数据流追踪器！

---

### 5.3 Ampere 特有算术/位操作/访存指令的“微指令展开”（1 条 Ampere -> 1~2 条 `MInst`）

对于少数 Maxwell 中没有同名指令的 Ampere 新指令，前端在解码生成 `MInst[]` 时直接将它们展开为等价的 1~2 条现有 Maxwell `MInst`：

| Ampere 新指令 | 语义说明 | 前端降级为现有 Maxwell `MInst` 的展开方式 |
| :--- | :--- | :--- |
| **`SGXT Rd, Ra, Rb/Imm`** | 有符号位域符号扩展（从 bit `Rb` 起符号扩展） | 直接生成 1 条 `O_BFE_R` / `O_BFE_I`（`mod.is_signed = true`，位宽 `width = Rb`，起始位 `pos = 0`） |
| **`BMSK Rd, Ra, Rb`** | 生成从 `pos=Ra` 开始、长度为 `width=Rb` 的位掩码 | 生成 1 条 `O_BFI_R`（将全 1 立即数 `0xffffffff` 按 `(Rb << 8) \| Ra` 插入 `RZ`） |
| **`FRND Rd, Ra, mode`** | 浮点取整为整数值浮点数（替代 Maxwell 同 bit-size 的 `F2F.RND`） | 生成 1 条 `O_F2F_R`（源/目标尺寸均设为 FP32，携带整数舍入标志 `RNI`/`RMI`/`RPI`/`RZI`） |
| **`IMAD.WIDE Rd, Ra, Rb, Rc`** | `32x32 + 64 -> 64` 宽乘加（写 `Rd, Rd+1`，读 `Rc, Rc+1`） | 展开为 **2 条连续 `MInst`**：<br>1. `O_IMAD_R` (`rd = Rd`, `mod.is_hi = false`, `mod.cc_write = true`) 计算低 32 位并写进位<br>2. `O_IMAD_R` (`rd = Rd+1`, `mod.is_hi = true`, `mod.x_mode = true`, `rc = Rc+1`) 计算高 32 位加进位 |
| **`LDGSTS [shmem], [gmem]`** | 异步全局内存到共享内存拷贝（Compute Shader 常见） | 展开为 **2 条连续 `MInst`**：<br>1. `O_LDG` 加载到内部保留临时寄存器 `MREG_TMP0`<br>2. `O_STS` 将 `MREG_TMP0` 写入共享内存 |
| **`DEPBAR.LE SBn, imm`** / **`LDGDEPBAR`** | 异步计分板依赖屏障 | 在同步执行的 SPIR-V 模型中直接降级为 `O_NOP`（若紧跟共享内存可见性需求则映射为 `O_MEMBAR`） |
| **`REDUX.<op> URd, Ra`** | 跨 Warp 归约到 `URd`（`SUM`/`MIN`/`MAX`/`AND`/`OR`/`XOR`） | 展开为子组归约（或在 `spirv_translate.c` 的 `O_VOTE` 旁增加 10 行 `OpGroupNonUniformIAdd`/`IMin`/`IMax`/`BitwiseOr` 调用写入 `regs[MREG_UR0 + urd]`） |

---

### 5.4 控制流与显式汇聚屏障（`BSSY` / `BSYNC` / `BMOV`）降级

Turing / Ampere (SM70+) 引入了独立线程调度（Independent Thread Scheduling），移除了 Maxwell 硬件隐式的 CRS 栈，改用显式屏障寄存器 `B0..B15`：

| Ampere 控制流指令 | 降级后的 `MInst.op` | 处理细节 |
| :--- | :--- | :--- |
| **`BSSY Bn, target`** | `O_SSY` (`mod.target_idx = target`) | 直接映射为 `O_SSY`！现有 `shader_resolve_crs` 会自动将其与后续的 `BSYNC` 配对 |
| **`BSYNC Bn`** | `O_SYNC` | 直接映射为 `O_SYNC`！ |
| **`BMOV Rd, Bn`** / **`BMOV Bn, Ra`** | `O_NOP` | 用于保存/恢复屏障掩码，SPIR-V 结构化/动态重汇聚无需硬件掩码寄存器 |
| **`BRA` / `ubra` Target** | `O_BRA` (`mod.target_idx = target`) | 若由 `@UPn` 门控，已通过 `guard_pred = MPRED_UP0 + n` 自动支持 |
| **`BRX Rd, offset`** / **`BRXU URd, offset`** | `O_BRX` | `src[0].reg` 指向 `Rd` 或 `URd`，`shader_brx_targets` 自动沿 `R`/`UR` 定义链解析跳转表 |
| **`BREAK Bn`** | `O_BRK` | 配合前序 `BSSY` 映射为 `O_PBK` / `O_BRK` |
| **`WARPSYNC` / `YIELD` / `NANOSLEEP`** | `O_NOP` | 线程束内调度提示，在 Vulkan/SPIR-V 中为无操作 |

---

### 5.5 128-bit 纹理与 Surface 指令归一化

在 Maxwell 中，`TEX` / `TLD` / `TLD4` / `TMML` / `TXD` 使用隐式连续寄存器元组（`ra..ra+3` 和 `rb..rb+3`）；而在 Ampere 128-bit 编码中，纹理指令的句柄（通常在 `URc` 或立即数 `tid`）与坐标寄存器分布得更灵活：

- **归一化策略**：
  1. 在 `MInst` 中增加 `u16 tex_coord_regs[4]` 和 `u16 tex_meta_regs[4]`（或复用现有 `ra`/`rb` 连续寄存器约定，若 Ampere 传入了非连续寄存器则在前端插入轻量 `O_MOV_R` 或在 `spirv_translate.c` 的纹理辅助函数中直接按 `inst->tex_coord_regs[k]` 读取）。
  2. 当 Ampere `TEX` 使用统一寄存器 `URc` 传递 Bindless 纹理句柄时，将其设为 `MInst` 的句柄寄存器（`src[2].reg = MREG_UR0 + urc`），并将 `op` 标记为 `O_TEX_B` / `O_TLD_B` / `O_TLD4_B`。现有的 `direct_indexed_handle()` 会自动追踪 `URc` 的 `ULDC` 来源并生成 `SNX_VKB_TEXARRAY` 访问！

---

## 6. 分阶段实施路线图（Step-by-Step Implementation Plan）

整个改造可以分为 **3 个解耦且可独立验证的阶段**，全程不会破坏现有的 Switch 1 (Maxwell) 兼容性：

### 阶段 1：在 `snxvk` 中引入 `MInst` 归一化层（保持 Maxwell 100% 行为一致）
1. **定义 `MInst` 与扩展寄存器空间**：
   - 在 `runtime/snxvk/src/nx/gpu/shader/shader.h` 中定义 `MInst`、`MOperand`、`MREG_*`（`322` 槽位）与 `MPRED_*`（`16` 槽位）。
   - 在 `Shader` 结构体中新增 `MInst *minst; u32 n_minst;`。
2. **编写 `frontend_maxwell.c`**：
   - 将原先散布在 `shader.c` 和 `spirv_translate.c` 中的 `IDX_OK(i)` 遍历与 `B(w, lo, n)` 位域提取集中搬入 `maxwell_lift_program(const u64 *words, u32 n_words, MInst **out_insts, u32 *out_count)`。
   - 在此阶段可保留 `MInst.mod.raw_lo = w`，支持按指令族逐步将 `spirv_translate.c` 从 `B(w, ...)` 切换到 `inst->...`。
3. **扩展 `FnCtx` 寄存器数组**：
   - 将 `spirv_translate.c` 中的 `regs[256]` 扩展为 `regs[MREG_COUNT]`，`preds[8]` 扩展为 `preds[MPRED_COUNT]`，让 `MREG_URZ` 指向 `zero_u32`、`MPRED_UPT` 指向 `true_bool`。
4. **回归验证**：
   - 使用现有的 `tools/shaderdec` 与已测试的 Switch 1 游戏着色器跑一遍，确认生成的 SPIR-V 与改造前完全一致。

### 阶段 2：实现 Ampere (SM86) SPH 头部与 128-bit 前端解码器 `frontend_ampere.c`
1. **SPH Header v4 / NVN2 解析 (`sph_parse_ampere`)**：
   - 识别 Ampere 128 字节 SPH 头部（或无 SPH 的 Compute Shader 描述符），填充现有的 `ShaderInfo`（`stage`、`imap_sys`、`imap_generic`、`omap_targets`、`omap_sys` 等）。
2. **128-bit 指令表与字段提取 (`decode_ampere.c` + `frontend_ampere.c`)**：
   - 按 16 字节（2 个 `u64`）步进扫描代码段，直到遇到 `EXIT` + 对齐填充或 `BRA self` 死循环尾标。
   - 根据 `lo[11:0]` 12-bit 操作码匹配 Ampere 指令族，按照第 5 节的映射表填充 `MInst`：
     - 将 `UR0..UR63` 映射为 `MREG_UR0 + n`，`UP0..UP6` 映射为 `MPRED_UP0 + p`。
     - 将 `BSSY`/`BSYNC` 映射为 `O_SSY`/`O_SYNC`。
     - 将 `IMAD.WIDE`、`LDGSTS`、`SGXT`、`BMSK`、`FRND` 展开为现有的 `O_*` 微指令。
   - 完成第二遍扫描，将所有分支/汇聚指令的字节偏移换算为 `MInst` 数组下标 `mod.target_idx`。

### 阶段 3：少量 Ampere 专属特性补齐与离线验证工具
1. **`REDUX` 与 128-bit 纹理坐标解包**：
   - 在 `spirv_translate.c` 中为 `REDUX` 添加 `OpGroupNonUniform*` 子组归约支持（约 15 行代码）。
   - 支持从 `MInst` 的显式坐标寄存器数组读取纹理坐标。
2. **扩展离线测试工具 `tools/shaderdec`**：
   - 在 `tools/shaderdec/shaderdec.c` 中加入 `--arch sm86` 选项，支持直接输入一段 Ampere 128-bit 机器码二进制，打印降级后的 `MInst` 序列、调用 `spirv_translate` 生成 `.spv`，并自动运行 `spirv-val` 与 `spirv-cross` 验证生成的 SPIR-V 合法性。

---

## 7. 已落地实现与全链路诊断日志体系（Implemented Modules & Diagnostic Logging）

上述“Ampere (SM86 128-bit) -> Maxwell IR (`MInst`) -> Canonical Maxwell 64-bit Stream -> SPIR-V”管线现已完整落地并合入 `runtime/snxvk`：

| 模块文件 | 状态 | 核心实现内容 |
| :--- | :--- | :--- |
| `runtime/snxvk/src/nx/gpu/shader/ampere_lower.h` | **已实现** | 定义 `MInst` 归一化 IR 节点、`AmpereLowerStats` 统计报告、`ampere_is_sm86_shader()`、`ampere_lower_to_maxwell()` 及分级日志接口 |
| `runtime/snxvk/src/nx/gpu/shader/ampere_lower.c` | **已实现** | 完整实现 SM86 128-bit 自动检测、`UR0..UR62 -> R192..R253` 与 `UP0..UP6 -> P0..P6` 零冲突重映射、128-bit 指令解码与微指令展开（`IMAD.WIDE`、`LDGSTS`、`SGXT`、`BMSK`、`FRND`、`BSSY`/`BSYNC`）、PC 相对分支重算及 `decode(w)` 自校验 |
| `runtime/snxvk/src/nx/gpu/shader/shader.c` | **已实现** | `shader_hash()` 支持 SM86 128-bit 长度扫描；`shader_merge_dual()` 支持 Ampere `VertexA + VertexB` 双顶点自动降级合并 |
| `runtime/snxvk/src/nx/gpu/shader/spirv_translate.c` | **已实现** | `shader_to_spirv()` 自动拦截 Ampere 着色器并降级为 Maxwell 流，且为 `IMAD.WIDE` 展开的 `O_IMAD_*` 补齐了 64-bit SSBO 全局内存基址溯源 (`gtags[]`) |
| `runtime/snxvk/src/nx/gpu/shader/spirv_compute.c` | **已实现** | `shader_compute_to_spirv()` 自动拦截 Ampere Compute Shader 并输出 `[AMPERE->CS-SPIRV]` 日志 |
| `runtime/snxvk/src/nx/gpu/vk/pipeline.c` | **已实现** | `sh_make()` 记录 `swr_vk_crumb` 面包屑与 `[SHADER-MAKE]` 架构/哈希/SPIR-V 字数诊断日志，且保持 `sizeof(ShaderInfo) == 2096` 不变 |
| `tools/test_ampere_spirv.c` | **已实现** | 覆盖 VS、FS（含 `BSSY`/`BSYNC`/`BRA` + `TEX`）、CS（含 `S2UR` + `ULDC.64` + `IMAD.WIDE` + `LDGSTS` + `BAR.SYNC`）及 `Dual-VS` 合并的离线端到端 SPIR-V 验证套件 |

### 日志标签速查（可通过环境变量 `SWR_AMPERE_LOG=0|1|2` 或 `.ini` 中 `ampere_log=0|1|2` 控制，默认级别 `2`）

- `[AMPERE-DETECT]`：记录 SM86 128-bit 与 Maxwell SM53 64-bit 启发式识别结果及首条指令特征字。
- `[AMPERE-SPH]`：记录 80 字节 SPH 头部的版本、阶段、Local Memory 大小与 Render Target 掩码。
- `[AMPERE-SCAN]`：记录向量寄存器压力及每个 `URn -> Rm`、`UPn -> Pm` 的静态分配表。
- `[AMPERE-TRACE]`：逐条打印 128-bit Ampere 原始指令 `(lo, hi)`、助记符、重映射掩码及降级后的 `MInst` 与 64-bit Maxwell 编码字。
- `[AMPERE-BRANCH]`：记录每条 `BRA`、`BSSY->SSY`、`CAL` 从 Ampere 字节偏移重算为 Maxwell 64-bit 字偏移前后的对比及 `bra_target()` 校验值。
- `[AMPERE-VERIFY]` / `[AMPERE-SUMMARY]`：对发射出的每条 64-bit 字执行 `decode(w)` 自检，并汇总指令膨胀率与重映射统计。
- `[AMPERE->SPIRV]` / `[AMPERE->CS-SPIRV]` / `[AMPERE->MERGE]` / `[SHADER-MAKE]`：记录后端 SPIR-V 编译结果、生成的 SPIR-V 字数、描述符绑定掩码及拒绝原因。
