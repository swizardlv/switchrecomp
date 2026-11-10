# SwitchRecomp PS5 移植复盘：四大思维误区剖析与真实差距全景图

> **编写背景**：在将 Switch 2 游戏（如 *Donkey Kong Bananza*）通过 `SwitchRecomp` 移植到越狱 PS5 的过程中，我们经历了多次“修复一处崩溃 -> 往前走几步 -> 再次遇到新崩溃或黑屏死机”的调试循环。本文从底层架构与仓库实际代码出发，复盘直觉预期与工程现实之间的**四大认知偏差**，明确当前 `ps5` 分支与主线的真实差距，并给出摆脱“实机盲调死循环”的工程提效方案。

---

## 1. 核心结论：直觉预期 vs. 工程现实对比总表

为什么原本以为只是“把跑通的 x86 代码换个壳编译到 PS5”，实际却变成每一步都要精细 Debug？根本原因在于**我们在不知不觉中同时叠加了三个维度的跨代移植（Switch 1 → Switch 2、动态加载 PC → 静态签名 OrbisOS、极简 C 桩 → 商业引擎重度 NVN 2 调用）**：

| 认知维度 | 直觉预期（理想模型） | 工程现实（本仓库实际状况） | 导致的调试体感 |
| :--- | :--- | :--- | :--- |
| **1. 上游基础与宿主系统** | `SwitchRecomp` 已把 Switch 2 移植到 Windows/Linux（ARM→x86），而 PS5 差不多就是一台 x86 Linux | ① 上游 `main` 分支**仅支持初代 Switch (Switch 1 / Maxwell / NVN 1)**，完全没有 Switch 2 支持；<br>② PS5 原生环境是 **OrbisOS (魔改 FreeBSD 11 + Sony 专有内核)**，在内存分页、ELF 加载、权限隔离上与 x86 Linux 截然不同 | 表面上是在调“PS5 移植问题”，实际上 **60% 的崩溃是在从零补齐 Switch 2 (NVN 2) 的 HLE 内核与驱动接口** |
| **2. 现有 PS5 Switch 2 模拟器参考** | 已经有 `PS5_Lichii` / `EglantinePS5` 等 PS5 下的 Switch 2 模拟器，说明底层路线是现成的 | 那些模拟器基于 **Yuzu/Eden (Fritchii)**，拥有十几年积累的数十万行 C++ HLE 内核与完整 NVN 驱动；而 `SwitchRecomp` 的 `runtime/` 是**不到 2 万行的极简 C 运行时** | 模拟器里早已处理好的几百个服务细节（NVN 反射、内存池、音频同步），在极简 `runtime/kernel.c` 里全是空白或假桩 |
| **3. OoT / Bananza 现有重编译案例** | 社区已有 `ocarina-recomp` / `dkb-pc-recomp` 移植案例，重编译代码是现成的 | 现有案例能跑通，是因为作者针对该游戏**重度定制了专属的 `runtime/`**；而我们拿《Bananza》的 500MB 重编译 C 代码去配本仓库的基础 `runtime/`，存在严重的**接口与结构体偏移代差** | 游戏代码读取 NVN 对象内部偏移（如 `+0x40`、`+0xc0`）时，基础 `kernel.c` 根本没填这些字段，读出 `0` 或 `1TiB` 哨兵直接崩 |
| **4. `ps5` 分支与主线的核心差异** | 以为只差：<br>a. PS5 资源不如 PC 多<br>b. PS5 特定 GPU 问题 | 实际上当前 `ps5build.py` 链接的是 **`gpu_nogl.c`（空 GPU 桩）+ `sceVideoOut` CPU 软上屏**，甚至**还没走到启用 PS5 Vulkan 硬件渲染的那一步**！当前卡住的全是前置的虚拟内存、FSELF 限制与 NVN 2 初始化链 | 每次改一行代码都要走“GitHub → 253 编译打包 → 拷入 PS5 → 黑屏重启 → 捞日志”的 15 分钟慢速循环 |

---

## 2. 逐条拆解四大思维误区（基于仓库源码实证）

### 误区一：“SwitchRecomp 本身是由 Switch 2 移植到 Windows/Linux（都是 ARM 到 x86），而 PS5 确切地说更像一种 x86 Linux”

这里隐藏着两个极其关键的事实偏差：

#### 1.1 上游 `SwitchRecomp` (`main` 分支) 是 Switch 1 架构，不是 Switch 2 架构
通过对比本仓库 `git diff main..ps5` 可以清楚看到，上游 `main` 分支完全是为 **Switch 1（Tegra X1 / ARMv8.0 / Maxwell GM20B 64-bit SASS / NVN 1 API）** 编写的：
- **CPU 指令集代差**：Switch 1 是 ARMv8.0（无 LSE 原子指令）；Switch 2（T239）是 ARMv8.2+，广泛使用 `CAS`、`LDADD`、`SWP` 等 LSE 原子指令（这也是我们在 `ps5` 分支修改 `a64.py` 补齐 LSE 原子指令的原因）。
- **NVN 1 vs. NVN 2 驱动对象布局代差**：Switch 1 的 NVN 对象由驱动在内部维护或使用固定小结构；而 Switch 2 的《Bananza》使用了全新的 **NVN 2 API**，游戏代码会直接内联读取 `NVNdevice`、`NVNqueue`、`NVNmemoryPool`、`NVNbuffer`、`NVNtexture` 结构体内部偏移（例如直接从对象内部读取 GPU 虚拟地址或反射块指针 `GetFirstBlock`）。上游 `main` 分支的 `kernel.c` 只是返回了一个哑句柄或未初始化内存，游戏一解引用就崩溃。
- **Maxwell SM53 (64-bit) vs. Ampere SM86 (128-bit) 着色器代差**：上游 `runtime/snxvk/src/nx/gpu/shader/` 只能解码 64-bit 的 Maxwell 指令流（每 4 个字含 1 个调度字）；面对 Switch 2 的 128-bit Ampere 指令流和 Uniform 寄存器 (`UR0..UR63`, `UP0..UP6`) 会直接拒绝翻译（这正是我们刚刚完成 `ampere_lower.c` 降级管线的原因）。

#### 1.2 越狱 PS5 原生应用（OrbisOS Native Title）绝不是普通的 x86 Linux
虽然 PS5 的 CPU 是 AMD Zen 2 (x86_64)，但除非你在 PS5 上引导启动了独立的 Linux 发行版内核，否则我们编译的 `PPSAxxxxx/eboot.bin` 运行在 Sony 的 **OrbisOS (Prospero)** 上。它与 x86 Linux 有四个致命的底层差异：

1. **虚拟内存与页表硬限制（最痛的坑）**：
   - **x86 Linux**：调用 `mmap(NULL, 128GB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0)` 只在内核创建一个 `vm_area_struct` 节点，**消耗 0 个物理页、0 个页表项**，之后可以在任意 4KB 边界随意 `mprotect`。即使游戏产生了 `0x10000000000` (1TiB) 的偏移，只要在预留窗口内就不会破坏宿主堆。
   - **PS5 OrbisOS**：
     - 硬件规定 `sceKernelMapDirectMemory` / 匿名映射按 **2 MiB Granule** 管理，且对单个进程硬性限制 **16,384 个 CPU Page Table 项**（即最多同时映射 32 GiB 地址空间）。
     - 更要命的是，在 OrbisOS 上对 2 MiB 块内部的 4 KiB 子页做 `mprotect(PROT_NONE)` **不会释放页表项**，只有整块 2 MiB `munmap` 才会归还页表。
     - 当 Switch 2 引擎对未绑定的 NVN 对象写入 `1TiB (0x10000000000)` 哨兵地址时，它远远超出了 PS5 64GB 的 `g_mem` 窗口，直接踩进未映射高地址触发内核级致命 Page Fault！
2. **禁止动态库 `dlopen` 与 FSELF 镜像体积炸弹**：
   - **x86 Linux**：`SwitchRecomp` 把主程序编成一个几百 KB 的可执行文件，把游戏重编译出的 500MB 代码编成 `<Game>_code.so`，启动时用 `dlopen` 加载，极其轻量。
   - **PS5 OrbisOS**：内核严禁加载任何未经过 Sony 零售签名的 `.so` 动态库。因此 `ps5build.py` 必须把 **508 MB 的游戏 `.o` 静态库 (`libgame_code.a`) + Runtime** 静态链接成一个巨大的 PIE ELF，再签名打包成单一的 `eboot.bin`。
   - **连锁反应**：之前为了拦截 `1TiB` 哨兵地址，我们在 `runtime/cpu.h` 的 `MP(a)` 宏里加了一行三元运算符检查。因为 `MP(a)` 在 500MB 的重编译代码里被展开了**数百万次**，`eboot.bin` 瞬间从 500MB 暴涨到 **1.27 GB**，直接撑爆 PS5 的 FSELF 加载器导致点击图标立刻黑屏死机！这种坑在 PC Linux 上根本不可能触发。
3. **代码段只执行保护（Execute-Only, XO）**：
   - PS5 原生应用的 `.text` 段默认不可读（硬件 XO 位），如果编译器把 switch 跳转表或常量放进 `.text` 段，CPU 一读指令段的数据就会触发 `SYSTEM_XO_VIOLATION` 秒崩（必须强制加 `-fno-jump-tables` 与专用链接脚本 `ps5-pie.ld`）。
4. **可观测性黑盒**：
   - Linux 下程序崩了有终端报错、Core Dump、GDB 回溯；PS5 原生应用一旦触发未处理异常、看门狗超时或显存越界，主机直接表现为一个现象——**黑屏或卡死在启动画面**。

---

### 误区二：“已经有一些 PS5 下 Switch 2 模拟器的实现，为什么它们能跑而我们这么费劲？”

社区中确实存在运行在 PS5 上的 Switch / Switch 2 模拟器分支（如基于 Fritchii/Eden 的 **[PS5_Lichii](https://github.com/mihawk-99/PS5_Lichii)** 与 **EglantinePS5**），但它们与我们当前的工程存在两个本质不同：

1. **HLE 内核与驱动层的厚度相差两个数量级**：
   - `PS5_Lichii` 底层是 Yuzu -> Sudachi -> Fritchii 一脉相承的完整模拟器内核，仅 `hle/service/` 和 `video_core/` 就有数十万行 C++ 代码，完整模拟了 Switch OS 的文件系统、音频渲染器 (`audren`)、NVDRV 通道以及 NVN 驱动的每一个结构体细节。
   - 而 `SwitchRecomp` 的设计初衷是“轻量级静态重编译运行时”，其 `runtime/kernel.c` 在 `main` 分支上只有约 2,500 行 C 代码，采用的是“游戏用到哪个函数再去 `kernel.c` 里拦截（Hook）哪个函数”的按需打桩模式。
2. **那些 PS5 模拟器也是踩完了一模一样的坑才跑起来的**：
   - 正如我们在 [ps5_lichii_gap_analysis.md](file:///Users/swizard/code/switchrecomp/docs/ps5_lichii_gap_analysis.md) 中盘点的那样，`PS5_Lichii` 的作者同样在 PS5 上被 **16,384 个页表耗尽**、**未映射 Sparse 页导致 GPU 致命 Fault**、**ASTC 纹理爆内存**、**打开文件句柄上限 128 报 ENOMEM** 折磨过，并专门写了 LZ4 冷内存压缩、共享零页和页表回收才解决。我们并不是在走弯路，而是在用 C 语言把他们在 C++ 模拟器里踩过的平台必经之路重新走通。

---

### 误区三：“已经有一些针对 OoT、Bananza 的移植案例，为什么不能直接平滑拿过来？”

这是最容易产生错觉的地方：既然社区已经有 `ocarina-recomp`（时之笛）和 `dkb-pc-recomp`（Bananza）的静态重编译案例，为什么我们编译出来的《Bananza》每走一步都会崩？

原因在于：**静态重编译产出的 `code_000.c ... code_NNN.c` 只是把 ARM64 机器码原封不动地翻译成了等价的 C 赋值语句，它并不包含任何“游戏移植逻辑”；真正的“移植兼容层”全部在 `runtime/`（尤其是 `runtime/kernel.c`）里！**

- 在《塞尔达：时之笛》(N64Recomp) 中，N64 游戏体积小（几十 MB）、直接操作固定物理内存，几乎没有复杂的现代操作系统和专有图形驱动栈。
- 而在 Switch 2 的《Donkey Kong Bananza》中，游戏二进制内部静态链接了完整的 **Nintendo SDK (`nnSdk`) + NVN 2 客户端库 + 自研商业引擎**：
  - 游戏启动前几秒钟，就会密集调用数百个 `nn::fs`、`nn::oe`、`nn::hid`、`aud:u`、`vi` IPC 服务以及 **数十个 NVN 2 函数指针**。
  - 当我们的 `runtime/kernel.c` 中某个 NVN 2 函数（例如 `nvnDeviceInitialize`、`nvnMemoryPoolInitialize`、`nvnBufferInitialize`、`nvnTextureInitialize`）漏填了结构体里的某一个 8 字节偏移，游戏自己的引擎代码（甚至是一些用于 Shader 反射和调试校验的辅助函数，如 `0x99a6350`、`GetFirstBlock`）在几千条指令之后读到这个 `0`，就会算出 `0x10000000000` (1TiB) 甚至非法地址并崩溃！
  - 因为崩溃点（消费端）往往距离未初始化的源头（生产端，即某次 NVN 调用）隔了好几个函数调用层级，所以在默认模式下看起来就像是“修了一个空指针，下一个函数又冒出另一个空指针”。

---

### 误区四：“以为 `ps5` 分支跟主线最主要的差别仅在于 a. PS5 资源更少；b. PS5 特定 GPU 问题”

如果把把《Bananza》在 PS5 上跑出画面比作一场 100 公里的马拉松，**“a. 资源预算治理”和“b. PS5 GPU 渲染”其实是第 70~100 公里的赛段**，而我们之前所有的 Debug 精力，其实都花在了**第 0~70 公里的“前置生存关”**上：

```text
[第 0~25 公里：PS5 原生应用生存关]  ---> 已完成 95%！
  - 静态链接 508MB 游戏代码 + FSELF 签名 (ps5build.py)
  - 绕过 dlopen 限制与 XO 只执行段保护
  - 16,384 页表回收、4KB 子页位图与 1TiB 哨兵页重定向 (plat.c / cpu.h)
  - NUP 大体积 zstd 资源包流式读取 (nup_ps5.c)
  - sceVideoOut 彩条点亮 (first_light) 与 DualSense 手柄驱动 (backend_ps5.c)

[第 25~65 公里：Switch 2 / NVN 2 HLE 内核通关]  ---> 当前正在攻坚的核心战场！
  - Switch 2 特有 SDK 服务打桩 (nn::oe, aud:u, vi, RomFS 索引)
  - NVN 2 对象池、MemoryPool、Buffer、Texture、Sampler 句柄与结构体偏移对齐 (kernel.c)
  - Shader 反射块 (GetFirstBlock) 与 GPU 错误回调拦截

[第 65~85 公里：Ampere -> Maxwell IR -> SPIR-V 着色器打通]  ---> 刚刚完成代码落地！
  - Ampere SM86 128-bit 指令检测与解码 (ampere_lower.c)
  - UR0..UR63 / UP0..UP6 统一寄存器重映射与 IMAD.WIDE / LDGSTS 微指令展开
  - BSSY / BSYNC 汇聚屏障转 Maxwell SSY / SYNC 并输出合法 SPIR-V

[第 85~100 公里：PS5 RADV Vulkan 硬件渲染合拢与资源调优]  ---> 下一阶段即将进入！
  - 将 ps5build.py 从 gpu_nogl.c 切换为静态链接 libsnxvk + PS5_Vulkan (Mesa RADV)
  - ASTC 纹理分批解码与冷内存 LZ4 压缩
```

> **关键洞察**：看看 `tools/ps5/ps5build.py` 第 23 行：
> `EXTRA = ['gpu_nogl', 'shader_proc', 'host1x', 'snxvk_glue', 'backend_ps5', 'entry', 'ps5_crt']`
> 目前 PS5 构建脚本链接的其实是 **`gpu_nogl.c`（无硬件 3D 渲染桩）**！也就是说，之前的黑屏死机**甚至还不是 PS5 GPU 驱动（RADV / AGC）的问题**，而是游戏主线程在 CPU 侧执行 Switch 2 / NVN 2 初始化和着色器反射时，因为 HLE 结构体字段不匹配或内存越界而死在了进入主渲染循环之前！

---

## 3. 当前 `ps5` 分支与主线 (`main`) 的真实差异全景盘点

截至当前提交，我们在 `ps5` 分支上相比 `main` 分支已经累计修改/新增了 **53 个文件、+12,662 行代码**。这些工作绝没有白费，它们已经把最艰难的底层基建搭好了：

| 模块领域 | 涉及文件 | 相比 `main` 分支已完成的改造 | 当前状态 |
| :--- | :--- | :--- | :--- |
| **1. AArch64 重编译器扩展** | `a64.py`, `switchrecomp.py`, `nxloader.py` | 支持 Switch 2 使用的 ARMv8.1/v8.2 LSE 原子指令（`CAS`/`LDADD` 等）；支持 `--target ps5` 静态模块导出 `swr_module_static` | **已完成** |
| **2. PS5 原生构建与部署链** | `tools/ps5/ps5build.py`, `build_deploy.sh`, `fetch_log.sh` | 自动完成 500MB+ 静态库防膨胀校验、PIE 链接、OrbisOS ELF 转换、FSELF 签名、远程 `253` 编译部署与崩溃日志自动回收 | **已完成** |
| **3. OrbisOS 虚拟内存与页表治理** | `runtime/plat.c`, `runtime/mem.c`, `runtime/cpu.h` | 实现 2 MiB Granule + 4 KiB 子页位图、空 Granule 自动 `munmap` 回收页表、16,384 页表水位监控，以及 `1TiB` NVN 哨兵地址在 `MP()` 宏中的零开销位掩码重定向 | **已完成** |
| **4. PS5 专有文件容器与系统层** | `runtime/nup_ps5.c`, `runtime/svc_fs.c`, `runtime/ps5_crt.c` | 实现 `.nup` 资源包 Zstd 流式解压读取，绕过 PS5 打开文件数限制；实现 OrbisOS CRT 启动、崩溃栈回溯与卡死线程 PC 实时抓取 | **已完成** |
| **5. Ampere -> Maxwell IR -> SPIR-V** | `runtime/snxvk/src/nx/gpu/shader/ampere_lower.c` 等 | 实现 Switch 2 Ampere (SM86 128-bit) 着色器自动识别、Uniform 寄存器重映射、微指令展开、分支重算与 SPIR-V 生成，并通过 VS/FS/CS/Dual-VS 端到端测试 | **已完成** |
| **6. Switch 2 / NVN 2 HLE 内核适配** | `runtime/kernel.c`, `runtime/svc_core.c`, `runtime/ipc.c` | 已补齐大量 S2 NVN 2 函数路由、MemoryPool/Buffer/Texture 描述符池隔离、音频句柄节流，并新增了 `SWR_STRICT=1` 根因第一现场定位开关 | **收尾攻坚中（约 80%）** |
| **7. PS5 硬件 Vulkan (`snxvk` + `RADV`) 对接** | `runtime/snxvk_glue.c`, `runtime/backend_ps5.c`, `ps5build.py` | 目前 `backend_ps5.c` 仅使用 `sceVideoOut` CPU 软翻转 + `gpu_nogl.c`；需将 `snxvk` 跨平台编译为 `libsnxvk_ps5.a` 并链接 `PS5_Vulkan` (`libradv.a`) | **待接入（0% -> 下一阶段主线）** |

---

## 4. 破局行动指南：如何让接下来的调试不再“这么费劲”？

既然看清了“为什么每一步都要 debug”的本质原因，我们接下来的推进策略就非常明朗了——**绝对不要再把“Switch 2 游戏逻辑/NVN 2 HLE 兼容性问题”放在 PS5 实机上靠黑屏重启去试错**，而是分三步彻底扭转调试效率：

### 第一步：用 `SWR_STRICT=1` 终结“打地鼠式”空指针修补
之前我们遇到空指针或 `1TiB` 哨兵地址时，往往是在崩溃点加一个 `if (!ptr) return 0;` 兜底，结果游戏继续往下跑几百条指令又在下一个地方崩了。
- 现在 `runtime/kernel.c` 已经加入了 **`SWR_STRICT=1`（严格断言模式）**：一旦任何 NVN 2 对象创建、MemoryPool 映射、Texture/Buffer 初始化或反射查询收到了未处理的枚举或返回了未绑定的假指针，**当场在第一次产生脏数据的 NVN API 调用处打印完整调用栈并停下**，一次性把《Bananza》用到的所有 NVN 2 结构体字段补齐。

### 第二步：在本地 / 253 Linux 机器上用 Headless 模式跑通《Bananza》主循环
《Bananza》的 C 代码在 x86_64 Linux（如 `253` 编译机）上同样可以直接编译成原生 Linux 可执行文件（使用 `backend_headless.c`）：
- 在 `253` 上编译并运行 Linux 版的耗时只有 **几秒钟**，而且可以直接挂 `gdb` / `asan`，瞬间看到完整的源码行号、调用栈和变量值！
- 只要《Bananza》在 `253` 的 Headless 模式下能平稳跑过标题画面、进入主循环并持续发出 `nvdrv` GPFIFO 提交（不崩、不死锁），就证明 **Switch 2 HLE 内核层（第 25~65 公里）已经 100% 通关**！

### 第三步：将 `libsnxvk` + `PS5_Vulkan (Mesa RADV)` 接入 `ps5build.py`
当 CPU 侧的游戏主循环跑通并开始向 `nvdrv` 提交 GPFIFO 渲染命令后：
1. 将 `runtime/snxvk_glue.c` 的 `#if defined(SWR_HAVE_SNXVK) && defined(_WIN32)` 放开支持 `__PROSPERO__` / POSIX（启用 `snxvk` 内置的 FreeBSD 脏页哈希检测回退路径）。
2. 在 `ps5build.py` 中用 `libsnxvk_ps5.a` + `libradv.a` 替换掉 `gpu_nogl.c`，让游戏提交的 Ampere 着色器经过我们刚写好的 `ampere_lower.c` -> `spirv_translate.c` -> PS5 RADV 驱动直接送入 PS5 GPU 硬件渲染上屏！
