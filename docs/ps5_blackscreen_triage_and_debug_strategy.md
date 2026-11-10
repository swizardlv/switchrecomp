# PS5 端「点游戏黑屏死机」定位手册与调试策略重构

> 分支：`ps5`，HEAD `fe39b8c`（2026-11-10 17:35）
> 证据来源：`runs/ps5log_20261110_110956/`（磁盘上最新的一份实机日志，`swr.log` 写于 09:49:51）、`git log`、`runtime/` 源码、`.workbuddy/memory/` 笔记
> 目标读者：接下来要动手修的人。先看第 1 节和第 7 节，其余按需查。

---

## 1. 结论（先看这里）

1. **当前部署的 429 MB eboot 从未被证明能启动。** 磁盘上最新日志来自 09:49 的 214 MB eboot。之后 `dc59939`（15:04）换成 576 个对象的完整代码集，eboot 变成 429 MB，没有任何一份日志对应这个包。已知事实是：214 MB 能跑，546 MiB 在 `_start` 之前系统级黑屏（`5ca4493`），1.27 GB 冻结（`dc59939` 的 `cpu.h` 注释）。429 MB 落在未测区间。**这是当前「黑屏死机」的头号嫌疑，但还只是嫌疑**，第 2 节给出 5 分钟判别法。
2. **即使 eboot 能启动，画面也一定是黑的。** Switch 2 的 NVN 函数现在全部由 `hle_s2_nvn_true`（恒返回 1）/ `hle_s2_nvn_zero`（恒返回 0）应答，没有任何一条绘制命令被执行，`video_present_rgba` 没有调用方。`first_light` 彩条之后不会再有新帧。黑屏是当前架构的**正常状态**，不是 bug。
3. **「一点一点调太难」的原因在方法，不在某一个 bug。** 现在的循环是：实机跑到崩 → 看崩溃地址 → 在该客户机地址打一个钩子绕过 → 再跑。`kernel.c` 里已有 17 处硬编码客户机地址。每个钩子都让客户机状态更偏离真实，下一个崩溃离根因更远。第 4 节用仓库里的三个实例说明。
4. **出路是三件事**，按顺序做：
   - 把 90% 的调试搬到 253 Linux 上（`kernel.c` 的 S2 钩子没有平台宏，Linux 可以原样复现）。
   - 让实机「说话」：屏幕上常驻阶段号 + 心跳，UDP 实时日志。
   - 对图形路径做一次明确决策（第 6 节），停止给 NVN 打「返回 1」的桩。

---

## 2. 现在这次黑屏死机：5 分钟判别

只需要回答一个问题：**点游戏之后，`/data/switchrecomp/swr.log` 的 mtime 变了没有？**

```bash
./tools/ps5/fetch_log.sh
python3 - <<'EOF'
import json,glob
t=json.load(open(sorted(glob.glob('runs/ps5log_*/timeline.json'))[-1]))
print([f for f in t['dirs']['/data/switchrecomp'] if f['name']=='swr.log'])
EOF
```

| 观察到的现象 | 结论 | 去哪一节 |
| :--- | :--- | :--- |
| 主机整机无响应，`fetch_log.sh` 超时；重启后 `swr.log` mtime 仍是旧的（09:49） | 进程没进 `_start`。eboot 体积问题 | 3.1 |
| 屏幕**从未**出现顶部 1/3 的 8 色彩条；`swr.log` 是新的，但停在 `stage: video_init` 之前 | 启动阶段挂住，看最后一条 `stage:` | 3.2 |
| 出现过彩条，随后黑屏；`swr.log` 末尾有 `FATAL:` | 客户机崩溃（NVN 桩级联） | 3.3 |
| 出现过彩条，随后黑屏；`swr.log` 持续增长，有 `N s without a new image (frames=0, GPU submits=0)` | 主循环在等一个永远不会来的帧完成信号 | 3.4 |

彩条是一个免费的判别器：`backend_ps5.c:97` 的 `first_light()` 在 `video_init` 里画它。看到彩条 = 加载器、内存窗口、NSO 装载、RomFS、全部 HLE 钩子安装都已通过。

---

## 3. 四种情形的处理

### 3.1 进程没进 `_start`：eboot 体积

**已知数据点**（全部来自本仓库提交与笔记）：

| eboot 体积 | 结果 | 出处 |
| :--- | :--- | :--- |
| 247 KB（探针） | 正常启动 | `5ca4493` |
| 385 KB（interp-only） | 正常启动 | `5ca4493` |
| 214 MB（280 个代码对象） | 正常启动，跑到 6.85 s | `runs/ps5log_20261110_110956` |
| **429 MB（576 个代码对象，当前）** | **未知** | 无日志 |
| 546 MiB | `_start` 前系统级黑屏重启 | `5ca4493` |
| 1.27 GB | 加载器冻结 | `dc59939` 注释 |

失败机制我没有证据，不猜。阈值可以用实验确定，不需要理解机制：

**步骤（约 1 小时，6 次部署）：**

1. 用现成的探针包（`ps5build.py --probe`）加一个填充段：`static const char pad[N] __attribute__((used, section(".text.pad")))`，N 取 250 / 300 / 350 / 400 / 450 / 500 MiB。
2. 每个包部署、点击、看 `/data/ps5_probe.log` 有没有 `SWR_PS5_PROBE_OK`。
3. 记下最大可启动体积 `S_max`。把它写进 `ps5build.py` 当硬门槛：超过就**构建失败**，不部署。

注意把 `.text` 填充和 `.rodata` 填充各测一轮。546 MiB 那次记录的是「首个 LOAD 段 522.7 MiB」，限制可能在单段大小上，也可能在总大小上，两者的解法不同。

**把体积降到 `S_max` 以下的手段（按性价比排序，效果都需要实测）：**

| 手段 | 做法 | 风险 |
| :--- | :--- | :--- |
| 冷热分离 | 在 Linux 上跑一遍收集「实际执行过的函数」清单；只把这些编成原生代码，其余走已有的解释器（`g_disp` 本来就支持 interp 入口，`--interp-only` 证明解释器可独立工作）。`switchrecomp.py` 目前没有按函数筛选的开关，需要加 | 冷函数首次执行变慢；清单不全时行为不变只是慢 |
| `sdk` 模块整体解释 | `sdk` 占 15 MB ARM64 代码，其中 vi / nv / NVN / oe 已被 HLE 接管，几乎不执行 | 低 |
| 链接器去重 | `prospero-lld --icf=all` + `-ffunction-sections -Wl,--gc-sections`。重编译产物里有大量形状相同的 PLT 跳板（如 `0x99a6350` 那一片） | 低 |
| `-Oz` + machine outliner | `-Oz -mllvm -enable-machine-outliner` | 变慢，幅度未知 |
| 拆成多个模块 | 游戏代码拆成若干 PRX，`sceKernelLoadStartModule` 装载 | 高。笔记记录转换器要求 `.dynsym` 全部 undefined，且 SDK stub 没有 `dlsym` |

**同时修掉构建完整性问题**：笔记记录 214 MB 那个包只含 280 个对象，而当前 codegen 有 576 个文件。也就是说 11-09 全天的实机结论来自一个残缺的包。`ps5build.py` 的 `verify` 阶段应该断言「归档内 `code_*.o` 数量 == `code_*.c` 数量」，不等就失败。

### 3.2 启动阶段挂住

看 `swr.log` 最后一条 `stage:`。阶段顺序（`main.c`）：

`load_code → npdm_mmu_flags → mem_init → kernel_init → build_dispatch → mod_init → load_hints → loader_load → npdm_sysres → services_init → romfs_init → gpu_init → snxvk_gpu_init → hle_install → s2_hle_hooks → video_init → kernel_run_main`

09:49 那次全部通过，`video: ready` 在 1.12 s。如果新包停在这之前，对比两份日志的第一处差异即可。`s2_hle_hooks` 阶段耗时 0.8 s 且写了约 460 行日志，量大但正常。

### 3.3 `FATAL: invalid memory access`

09:49 那次的崩溃现场：

- `pc=8864814`，线程 80（main），调用链 `nnMain → … → 8479bc8 → 8484684 → 8485f98 → 8491a80 → 8492764 → 84932c8 → 8864848`
- `x21=0x0000010000000101`，`x8=0x18`，访问 `x21+0x18`
- 刚从 `0x99a6350`（PLT 跳板 → `nn::util::BinaryFileHeader::GetFirstBlock`）返回

`0x0000010000000101` 不是「NVN 未绑定对象哨兵」。它是小端的字节序列 `01 01 00 00 00 01 00 00`，即几个 `bool` 字段。`dc59939` 的注释已经得出同一结论：某处对 `NULL+偏移` 写了布尔值，后面对 `NULL+偏移` 读指针，读回这串字节当指针用。

真正的问题在更上游：**某个本该返回有效对象的调用返回了 0**，客户机把 0 当对象指针继续用。因为客户机第 0 页被映射成可读写（`mem.c:21`），写入和读取都「成功」了，崩溃被推迟到很远的地方。

处理方法见 4.2 和 5.2。不要再在 `0x8864xxx` 附近加钩子。

### 3.4 主循环等帧

11-09 的笔记已经定位：主线程在等一个「帧完成」标志，NVN 的 present / submit 被桩掉后这个标志永远不置位。这是第 6 节图形决策的范畴。

---

## 4. 为什么逐个崩溃修不动：三个仓库内的实例

### 4.1 「1 TiB 哨兵」：5 个提交追了一个假象

| 提交 | 做了什么 |
| :--- | :--- |
| `2b1f147` | 认定 `0x10000000101` 是 NVN 未绑定对象哨兵，尝试映射客户机 1 TiB 处的页 |
| `e6e2952` | 映射被 `mem_is_free` 挡住，改成直接 `plat_vm_commit` |
| `e64e53d` / `2d8a673` | 主机地址 1088 GiB 映射不了，加「高地址扩展窗口」，再加 `mmap` 回退 |
| `5e0f3b8` | 还是不行，改成在 `MP()` 宏里加分支重定向。该宏被内联进数百万处 `RD/WR`，eboot 从 214 MB 涨到 1.27 GB，加载器冻结 |
| `dc59939` | 回退 `MP()`，发现那个值其实是写进第 0 页的布尔字节 |

一天时间，根因是一句话：第 0 页可写。`plat.c` 里的扩展窗口代码和 `g_sentinel_page` 现在是无用代码，建议删除。

### 4.2 第 0 页可读写：把「第一现场」抹掉了

`mem.c:21`：`plat_vm_commit(g_mem,0x10000)`，注释写的是「让 `NULL(+off)` 读返回 0 而不是崩溃」。这是上游对成熟游戏的容错设计。在移植调试阶段它的效果相反：空指针解引用不崩，错误数据继续传播，最终崩在无关的位置。

`dc59939` 的对策是 `s2_scrub_null_page()`：在**每一次 SVC 和每一次 NVN 调用**时 `memset` 64 KiB。问题：

- 它不解决谁在往空指针写，只是定期擦除。
- 多线程下，擦除和客户机读之间有竞态，行为不确定。
- `svcSleepThread` 这类高频 SVC 每次都付 64 KiB 的 `memset`。

### 4.3 日志格式串错位被当成竞态

09:49 的日志第 16 行：`pt_used 1->3674856099`，此时进程只有一个线程、启动 0.03 秒。`5e0f3b8` 的提交说明把它归因于「未持 `g_memlock` 的竞态导致下溢」，并加了 `ps5_pt_dec` 下溢保护。真实原因是 `LOG` 的格式串有 9 个占位符却只传了 8 个参数（少了 `failed`），`dc59939` 顺手修了。同一行里的 `failed=27`、`failed=1655` 其实是 `pt_before` 的值，并没有任何 commit 失败。

教训：**诊断日志本身要被测试**。`-Wformat` 被 `-w` 关掉了（`ps5build.py:198`），打开它这类问题在编译期就会报出来。

### 4.4 共同模式

| 做法 | 后果 |
| :--- | :--- |
| NVN 桩恒返回 1 或 0 | 客户机拿到不存在的对象，往后每一步都可能崩 |
| 按崩溃地址加钩子 | 只对这一个 dump、这一个版本有效；钩子本身改变了后续执行路径 |
| 容错机制（第 0 页、哨兵页、擦除）掩盖首个错误 | 崩溃点和根因之间的距离越来越大 |
| 每次迭代都上实机 | 一轮 5–10 分钟，没有调试器，只能靠预先埋的日志 |

---

## 5. 调试方法重构

### 5.1 Linux 优先：能在 253 上复现的 bug 不上实机

依据：`swr_install_s2_hooks()`（`kernel.c:1320`）和全部 `hle_s2_*` 没有 `SWR_PS5` 宏保护；11-09 笔记的对照表也确认 HLE 缺口「按游戏代际划分，不按宿主平台」。所以 3.3 的崩溃在 Linux 上应当原样出现。

**搭建（预计半天）：**

1. `tools/ps5/nupread.py` 把 base 包解到 `/data/switchrecomp/work/bananza/{exefs,romfs}`。
2. `python3 switchrecomp.py <exefs> --target linux --backend headless`。
3. 用 `-O1 -g -fsanitize=address,undefined` 编 runtime（游戏代码保持 `-O2`，不加 sanitizer，否则编译时间不可接受）。
4. 跑，确认崩在同一个客户机 `pc=8864814`。

**得到的能力：**

| 工具 | 用法 |
| :--- | :--- |
| `gdb` | 崩溃时直接看宿主栈和客户机 `CPU` 结构体 |
| 硬件写断点 | `watch *(long*)(g_mem+0xd0)`：直接抓到是谁往 `NULL+0xd0` 写，一步到位解决 3.3 |
| `rr` | 录制一次，反向执行到坏值产生的那一刻 |
| ASan | runtime 自身的越界和 UAF |
| 迭代速度 | 改 `kernel.c` 后重编 runtime 约几十秒，无需部署 |

**规则：** 实机只用来验证 PS5 平台层（加载器、VA 窗口、Direct Memory、VideoOut、Pad、AudioOut）。`kernel.c` / `svc_*.c` / `ipc.c` 里的问题一律在 Linux 上解决后再部署。

**可选增强：** 给 Linux 构建加 `-DSWR_PS5_SIM`，在 `plat.c` 的 Linux 分支里模拟 PS5 的约束（2 MiB granule 提交、16,384 页表上限、`malloc` 总量上限）。这样 `plat.c` 的 PS5 逻辑也能在 Linux 上跑单元测试。

### 5.2 尽早崩：调试构建关掉所有容错

加一个环境开关（或 `swr.ini` 项）`strict=1`：

```c
/* mem.c mem_init() */
if(!g_strict) plat_vm_commit(g_mem,0x10000);   /* strict: 第 0 页保持 PROT_NONE */
```

`strict=1` 时同时：

- 不调用 `s2_scrub_null_page()`。
- 不安装按地址打的钩子（`0x884f2c0`、`0x99a6350`、`0xa966600`、`0xa966700` 等）。
- 未实现的 NVN 函数第一次被调用时打印「函数名 + 调用方 LR」。

这样崩溃会落在**第一个**空指针解引用上，回溯直接指向返回了 0 的那个桩。

### 5.3 NVN 调用普查：一次运行列出全部缺口

`hle_s2_nvn_proc` 已经能拿到函数名（`x1`）。给每个名字建一个计数槽，退出或崩溃时输出一张表：

```text
nvn census (按首次调用顺序):
  #   calls  stub   name
  1       1  true   nvnDeviceInitialize
  2     412  true   nvnDeviceGetInteger
  3      37  obj    nvnMemoryPoolMap
  ...
```

这张表就是图形工作的待办清单，也符合项目约定的「一次运行收齐所有问题」。按「首次调用顺序」实现，比按崩溃顺序实现稳定得多。

### 5.4 让实机说话

**（a）屏幕常驻状态条（预计 2 小时）。** `backend_ps5.c` 已经有帧缓冲和 `swkbd_font.h` 字体。起一个宿主线程，每 250 ms 往帧缓冲顶部画两行并 flip：

```text
stage=kernel_run_main  t=12.5s  hb=50  frames=0  nvn=18234  svc=99120  pt=1677/16384
last: romfs Eft_BananamondGetSplash00_Fc.bntx.zs
```

- `hb` 是心跳计数。数字在涨 = 进程活着；停了 = 挂了。不用取日志就能区分 3.3 和 3.4。
- `fatal()` 里把整屏刷成红色并打印 FATAL 文本，停 10 秒再 `_exit`。现在 `fatal()` 调的是 `plat_message`，PS5 上没有可见输出。

**（b）UDP 日志（预计 1 小时）。** `logf_` 里加一路 `sendto()` 到 Mac，Mac 上 `nc -ul 9999 | tee run.log`。整机冻结时，文件日志依赖 `fsync` 落盘，UDP 包在冻结前已经发出。启动期 fsync 可以随之关掉，启动会更快。目标地址放进 `swr.ini`，不要硬编码。

**（c）阶段面包屑。** 每进入一个 `stage:`，往 `/data/switchrecomp/stage.txt` 覆盖写一行并 `fsync`（几十字节）。重启后读它就知道上次死在哪一步，不依赖大日志。

**（d）打开编译警告。** runtime 的编译去掉 `-w`，至少加 `-Wformat -Werror=format`。游戏代码保持 `-w`。

### 5.5 构建与部署的硬门槛

在 `ps5build.py` 的 `verify` 阶段加三条断言，任何一条不过就不产出 eboot：

1. eboot 体积 ≤ `S_max`（3.1 实验得到）。
2. 归档内 `code_*.o` 数量 == `code_*.c` 数量。
3. eboot 内嵌入 git 短哈希，`swr.log` 第一行打印它。现在只有 `built Nov 10 2026`，同一天的 6 个构建无法区分。

---

## 6. 图形路径：必须做的一次决策

上游 SwitchRecomp 的图形方案是**模拟 Maxwell GPU**（Switch 1 的 Tegra X1）：游戏自带的 NVN 驱动照常运行，runtime 模拟 `nvdrv` ioctl 和 GPFIFO 命令流，再用 OpenGL / Vulkan 重放（`gpu_engines.c`、`gpu_gl.c`、`snxvk/`）。

Bananza 是 Switch 2 游戏。`f656059` 的做法是用符号级钩子跳过 Switch 2 的 GPU 启动链，之后 NVN 入口全部打桩。**这意味着上游的 GPU 模拟在这条路径上完全没有参与**，而 PS5 构建链接的又是 `gpu_nogl`。目前不存在任何能产生像素的代码路径。

| 选项 | 内容 | 能得到什么 | 量级 |
| :--- | :--- | :--- | :--- |
| G0 假帧完成 | present / submit 桩里置位「帧完成」，配合 5.4 的状态条 | 验证游戏逻辑、资源加载、音频、输入能走多远。屏幕只有状态条 | 1–2 天 |
| G1 NVN API 级 HLE | 实现 5.3 普查表里的 NVN 函数，翻译到某个宿主图形 API | 真实画面 | 以月计。523 个入口，着色器是 NVN2 二进制格式，需要转译 |
| G2 驱动级 LLE | 不跳过 GPU 启动链，模拟 Switch 2 的 `nvdrv` / GSV 服务与 GPU 命令流 | 真实画面，对所有 S2 游戏通用 | 以月计。需要 Switch 2 GPU 的命令流知识 |

**我不知道的事（需要先查清再选 G1 还是 G2）：**

- `docs/ps5_lichii_gap_analysis.md` 说 PS5_Lichii 有「完整 GPU 硬件路径：RADV/Vulkan」。我没有核实过这个仓库。如果属实，它已经同时解决了「PS5 上的 Vulkan」和「Switch 2 GPU 模拟」两个问题，而且是 GPL-3.0，与 `snxvk` 的许可兼容。**读它的 GPU 层怎么做，是选型前最有价值的一步**（预计 1–2 天阅读）。
- PS5 自制环境下可用的图形 API。`backend_ps5.c` 现在是 CPU 拷贝到 VideoOut 帧缓冲，这条路可以显示软件渲染结果，但跑不动 3D 游戏。

**建议：** 先做 G0（便宜，且能暴露图形之外的所有问题），同时花 1–2 天读 Lichii 的 GPU 层，再决定 G1 / G2。在此之前不要再给单个 NVN 函数打补丁。

**对照组（可选）：** 如果手头有任何 Switch 1 游戏的 dump，把它编成 PS5 包跑一次。上游对 Switch 1 是成熟的，eboot 也小得多。它能把「PS5 平台层的问题」和「Switch 2 代际的问题」分开。现在这两类未知数叠在一起。

---

## 7. 执行顺序

| 步 | 做什么 | 通过标准 | 预计 |
| :-- | :--- | :--- | :--- |
| 1 | 第 2 节判别：当前 429 MB 包点击后 `swr.log` mtime 是否更新 | 得到 3.1–3.4 之一 | 5 分钟 |
| 2 | eboot 体积二分，得到 `S_max`，写入构建门槛 | 超限构建失败 | 1 小时 |
| 3 | 5.4 的状态条 + 红屏 FATAL + UDP 日志 + git 哈希 | 不取日志就能说出进程死活和所在阶段 | 半天 |
| 4 | Linux 复现环境，复现 `pc=8864814` | 同一崩溃点 | 半天 |
| 5 | `strict=1` + 写断点，找到 3.3 的真正来源 | 知道是哪个桩返回了 0 | 1–2 小时 |
| 6 | NVN 普查表 | 得到完整函数清单 | 2 小时 |
| 7 | 把体积降到 `S_max` 以下（冷热分离优先） | 完整代码集在实机启动 | 1–3 天 |
| 8 | G0 假帧完成 | 状态条上 `frames` 在涨，主循环推进 | 1–2 天 |
| 9 | 读 Lichii GPU 层，做 G1 / G2 决策 | 一页决策记录 | 1–2 天 |

步骤 3、4 与步骤 2 互不依赖，可以并行。步骤 4 之后，步骤 5、6、8 全部在 Linux 上完成，不占实机。

时间均为估计，没有实测依据。

---

## 8. 可以清理的代码

以下内容在根因查清后已无作用，留着会干扰后续判断：

| 位置 | 内容 | 原因 |
| :--- | :--- | :--- |
| `plat.c` 约 585–720 行 | 高地址扩展窗口（`ext window`） | 为假「1 TiB 哨兵」而加，实机映射失败 |
| `cpu.h` / `mem.c` / `plat.c` | `g_sentinel_page`、`g_sentinel_hits`、`SWR_SENTINEL_GUEST_PAGE` | 同上，`MP()` 重定向已回退 |
| `kernel.c` | `s2_scrub_null_page()` 及其在 `swr_svc` / NVN 桩里的调用 | 被 `strict` 模式 + 修正返回 0 的桩取代 |
| `plat.c:563` | `ps5_pt_dec` 的下溢保护日志 | 针对的「下溢」是格式串错位 |
| `ps5build.py:560–630` | bloat 自动修复逻辑（约 70 行） | 换成一条体积断言，失败即停 |
| `docs/ps5_splash_hang_analysis.md` | 第一至五节 | 所述 6 个根因已全部修复，可加一行状态说明 |

清理前先完成步骤 4，在 Linux 上确认删除后行为不变。
