# gsim-mt 项目总体进展说明

本文面向不了解 gsim、FIRRTL、RTL 仿真、Wolvrix 和本项目上下文的读者。它的目标不是替代设计文档，而是用一份可读的项目说明解释：这个项目为什么存在、现在已经完成了哪些阶段、当前实现的技术边界是什么、还没有完成什么，以及下一步应该往哪里推进。

截至本文状态，`gsim-mt` 已经完成了从基线复现、调度导出、GRHIR sidecar、保守任务分类、顺序 helper 重构、ActiveBuffer、最小多线程调度器，到 06B.3 最小语义 RepCut-lite 的第一轮闭环。当前多线程模式已经能在小型 FIR smoke tests 上用 `GSIM_THREADS=1/2/4` 跑通并通过等价性检查；RepCut-lite 也已经能在同 active word 的窄范围 `pure_compute -> pure_compute` cut 上生成 cloned value，让一个 focused cut batch 不再强制 `workerCount = 1`。07 曾经做过 Wolvrix fixed small-SV frontend bridge 探索，但该方向现在已从 active code 撤回并归档。当前主线再次收敛：先用最短路径让 XiangShan 通过 `gsim-mt` 生成带多线程 runtime 的 gsim emu 并跑通 tiny smoke，再测 `GSIM_THREADS=1/2/4` 最小矩阵；之后才根据 XiangShan profile 做 runtime 或 RepCut-lite 优化。

## 1. 一句话结论

`gsim-mt` 目前处在“最小多线程 runtime 和窄范围 RepCut-lite 语义 cut 已经正确跑通小测试”的阶段。

已经做到：

- 保留原始 gsim 默认行为。
- 能导出当前 gsim 的调度信息。
- 能给每个调度任务打上 conservative 的 pure/serial 分类。
- 能把原始 step 逻辑拆成 `mtTask*` helper，并验证顺序 helper 与默认路径一致。
- 能用 `ActiveBuffer` 让 helper 不直接写全局 `activeFlags`。
- 能在 `--mt-helper-mode=mt` 下，把一段安全的 pure compute batch 分发给多个 `std::thread` worker。
- 能在小型 FIR 测试上验证 `seq`、`buffered-seq`、`mt` 三种新路径的基本正确性。
- 能在一个 focused `pure_compute -> pure_compute` cut 场景中，为 selected successor 生成 cloned producer value，记录 `cut_edges[]`、`cut_batches[]`、`duplicated_nodes[]` 和 forced sink proof，并用 reference trace 对比检查并行路径。

还没有做到：

- 还没有实现通用 RepCut clone 框架；当前只覆盖窄范围同 active word pure cut。
- 还没有给出有效性能结论。
- Wolvrix frontend bridge 探索已归档，近期不继续作为主线。
- 还没有声明 XiangShan 级别多线程正确性或性能收益。
- 当前代码工作树仍有未提交改动。

## 2. 背景：gsim 是什么

gsim 是一个 RTL 仿真器。它把硬件设计的中间表示输入编译成 C++ 模型，然后通过编译出的 C++ 程序模拟硬件电路每个周期的行为。

在当前仓库里，gsim 主要接受 FIR/FIRRTL 形式的输入。FIRRTL 是 Chisel 生态常用的一种硬件中间表示。gsim 会把输入设计转换成内部图结构，然后生成 C++。生成出的 C++ 模型会维护电路中的寄存器、存储器、线网、组合逻辑和调度状态。

这个项目的长期目标不是只改一个 FIRRTL 仿真路径，而是在 gsim 的基础上做一个可验证的多线程 RTL 仿真原型，并最终服务于 XiangShan 这类大型 SystemVerilog 设计。

## 3. gsim-mt 是什么

`gsim-mt` 是基于 gsim 的多线程 RTL 仿真改造项目。

这里的 `mt` 指 multi-thread。项目核心问题是：在不改变 RTL 仿真语义的前提下，把一部分可以并行的计算放到多个 CPU 线程上执行。

RTL 仿真不能简单地“看到很多节点就并行跑”。原因是硬件仿真每一步都有严格语义：

- 当前周期的状态和输入决定组合逻辑结果。
- 组合逻辑结果可能激活后续节点。
- 寄存器、memory、reset、外部模块、assert/printf/stop 等有副作用或状态边界。
- gsim 原本的调度顺序、active flag 可见性、reset 观察点都可能影响最终行为。

所以 `gsim-mt` 的策略不是一开始就改成全图并行，而是先把原始 gsim 调度行为“结构化地拆出来”，逐步证明每一步仍然等价。

## 4. 为什么多线程 RTL 仿真难

软件程序中的普通并行计算，通常只要保证数据竞争被处理好即可。但 RTL 仿真还有额外约束。

### 4.1 active 调度语义

gsim 使用 `activeFlags` 表示哪些 SuperNode 需要执行。每个 SuperNode 有一个 `cppId`，仿真时按 `cppId` 扫描 active bit。

原始行为大致是：

1. 看当前 active word 是否非零。
2. 把这个 word 取快照到 `oldFlag`。
3. 清掉当前 `activeFlags[word]`。
4. 按 `cppId` 顺序扫描这一组 bit。
5. 执行某个 SuperNode 后，可能设置后续 SuperNode 的 active bit。

这里最关键的是“可见性时机”。一个节点执行后激活的后继任务，不能比原始 gsim 更早或更晚被观察到。否则就算没有 C++ 数据竞争，RTL 行为也可能变化。

### 4.2 pure compute 与 serial task

只有满足条件的纯组合计算任务可以并行。当前项目把任务保守分成两类：

- `pure_compute`：没有状态提交、memory write、reset、外部副作用等，可以进入受限并行 batch。
- serial task：寄存器、memory、reset、external module、special statement、unknown task 等，必须串行执行或作为 barrier。

分类原则是保守的。只要不能证明安全，就不并行。

### 4.3 reset 尤其敏感

异步 reset 不能被普通 pure compute 处理。原始 gsim 对 async reset 有 pre-reset / reset-source compute / post-reset 的观察点语义。当前 `gsim-mt` 保留了这种串行语义，不把 `SUPER_ASYNC_RESET` 当作 pure parallel task。

### 4.4 worker 不能直接写全局 activeFlags

如果多个 worker 直接写全局 `activeFlags`，不仅会有数据竞争，还会破坏 active 可见性。当前实现引入 `ActiveBuffer`：worker 只写自己的本地 buffer，主线程在确定的 barrier 点按顺序合并。

## 5. 当前路线

当前 FIR/FIRRTL 路径是 correctness bootstrap，也是近期性能主线。原因很直接：这条路已经可构建、可回归、可 profile，最适合先把并行 runtime 的性能问题解决。

07 已经证明过 Wolvrix `read_sv -> GRH -> frontend facts` 方向可以跑通一个 fixed small-SV smoke，但这个实现不是通用前端，也不能仿真 XiangShan。现在该方向已归档。后续只有在性能主线证明值得继续，且明确需要 SystemVerilog/GRH 前端时，再恢复前端工作。

近期主线是：

```text
existing gsim/FIR bootstrap path
  -> direct XiangShan difftest gsim-mt bring-up with GSIM_FLAGS
  -> temporary tiny no-diff correctness smoke with NO_DIFF=1 WITH_CHISELDB=0 WITH_CONSTANTIN=0
  -> XiangShan GSIM_THREADS=1/2/4 bootstrap matrix
  -> restore difftest/ChiselDB/ConstantIn generated-file strategy for fresh BUILD_DIR
  -> optimize only the bottleneck shown by XiangShan data
```

边界也很明确：

- 新代码继续写在 `/home/zhangyangjie/test/.worktrees/gsim-mt/gsim`。
- 不继续在 `wolvrix-playground` 中接入 Wolvrix frontend。
- 不把 bootstrap-path 性能说成最终 SystemVerilog/Wolvrix frontend 性能。
- 不继续做小 FIR-only 性能优化，直到 XiangShan gsim-mt bring-up 和最小矩阵至少被尝试。
- 不为了性能改变 correctness oracle。
- 08X 的 fresh build dir 暂不复用 `/home/zhangyangjie/test/XiangShan/build/chisel_db.cpp` 或 ConstantIn 生成物；no-diff bring-up 先显式关闭 `WITH_CHISELDB`、`WITH_CONSTANTIN` 和 difftest。
- 这不是最终形态。后续 10X 必须恢复 difftest/ChiselDB/ConstantIn 路径，并明确 fresh gsim-mt `BUILD_DIR` 的 generated support files 来自哪里。

## 6. 当前阶段完成情况

### Target 00：baseline 复现

状态：已完成。

目的：先确认原始 gsim 能构建、能跑基本测试，后续改动才有对照基线。

意义：如果没有 baseline，后续任何失败都无法判断是原始问题还是 `gsim-mt` 引入的问题。

### Target 01：调度 JSON 导出

状态：已完成。

新增能力：

- 支持导出 gsim 当前调度信息。
- 记录 `cpp_id`、scan order、active word/mask、依赖、active fanout、状态/存储器/reset/external 边界等信息。

意义：多线程调度不能只靠代码生成时的隐式逻辑。必须先把当前调度显式导出，才能分析哪些任务可以并行，哪些必须串行。

### Target 01A：GRHIR sidecar

状态：已完成。

新增能力：

- 在不接管 runtime 的前提下，用 GRHIR sidecar 表达调度图和状态边界。
- 把 gsim 的 `cpp_id` 与 sidecar 分析结果关联起来。

意义：GRHIR sidecar 目前用于分析调度图和状态边界，也保留了以后重新打开 GRH/frontend 路线的接口余地。它不是当前性能主线的 runtime。

### Target 02：保守分类器与 helper-seq

状态：已完成。

新增能力：

- 支持 `--mt-helper-mode=seq`。
- 把每个 SuperNode 的执行逻辑拆成 `mtTask*` helper。
- 默认模式仍走原始路径。
- `seq` 模式按原始顺序调用 helper，用来证明“拆 helper”本身不改变行为。

意义：这是多线程前的第一步重构。如果顺序 helper 都不能和默认路径一致，就不能继续做并行。

### Target 03：ActiveBuffer 与 buffered-seq

状态：已完成。

新增能力：

- 支持 `--mt-helper-mode=buffered-seq`。
- 新增 `ActiveBuffer`。
- helper 不再直接把后续激活写到全局 `activeFlags`，而是写到本地 buffer。
- 主线程在原始观察点合并 buffer。

意义：这是多线程安全性的关键铺垫。worker 未来可以并行执行 helper，但 active 更新仍由主线程确定性合并。

### Target 04：最小多线程调度器

状态：已完成第一版。

新增能力：

- 支持 `--mt-helper-mode=mt`。
- 生成 `mtRunPureBatch()`。
- 用 `std::thread` 执行 pure compute batch。
- 通过 `GSIM_THREADS` 控制 worker 数。
- worker 使用自己的 `ActiveBuffer` 和本地 flag。
- 主线程在 batch 结束后确定性合并结果。

当前实现是一个有意收窄的最小并行调度器：

- 使用静态分块。
- 不做 work stealing。
- 不做 lock-free runtime。
- 只并行经过保守分类的 `pure_compute`。
- serial/reset/memory/external/special/unknown 任务仍串行。
- 保持原始 `cppId` 扫描和 active word snapshot 语义。

### Target 06B：RepCut-lite 最小语义 cut

状态：已完成 06B.1、06B.2、06B.3 的窄范围闭环。

新增能力：

- `--mt-repcut-lite=on|off`、copy budget、fanout budget 和 deterministic report。
- selected task / cut edge / cut batch 的报告字段。
- 在 focused real-cut fixture 中记录真实 `pure_compute -> pure_compute` `cut_edges[]`。
- 为 selected successor 生成 cloned producer value，并在 `mtRepCutLiteTask<N>` 中替换原 producer node。
- 对 parallel-safe cut batch 记录 `clone_count`、`forced_sink_cpp_ids`、`forced_sink_mask`、`forced_sink_activation` 和 `parallel_safe_reason`。
- 对支持的 parallel-safe batch 不再生成 `workerCount = 1` guard，同时保留 unsupported cut 的 forced-serial fallback。
- focused checker 会编译生成模型，并比较 `GSIM_THREADS=1` reference trace 与 `GSIM_THREADS=4` trace。

当前实现仍然是很窄的 RepCut-lite：

- 只支持同 active word。
- 只支持 `pure_compute -> pure_compute`。
- 只支持单 selected successor consumer 的 cloned producer value。
- clone 表达式只覆盖当前明确支持的纯组合表达式子集，包括 `add/and/or/xor/tail/bits/pad/asUInt/mux` 等。
- 不支持 state、memory、reset、special、external、unknown、cross-clock、cross-active-word、多消费者或递归 clone chain。
- 没有做性能收益声明。

## 7. Target 04 的实现细节

### 7.1 新增命令行模式

当前 `--mt-helper-mode` 支持：

```text
off
seq
buffered-seq
mt
```

含义：

- `off`：默认模式，保留原始 gsim 生成路径。
- `seq`：生成 `mtTask*` helper，但仍顺序执行。
- `buffered-seq`：顺序执行 helper，但 active 更新先进入 `ActiveBuffer`。
- `mt`：对安全的 pure compute batch 使用多线程执行。

### 7.2 pure batch 的进入条件

任务要进入 MT pure batch，必须满足：

- 分类结果是 `pure_compute`。
- 不是 always-active task。
- 与 batch 内已有任务没有会破坏当前调度语义的边。
- 位于同一个 active word 扫描范围内。

如果不满足条件，就回到串行 helper 调用。

### 7.3 ActiveBuffer 的作用

`ActiveBuffer` 是一个本地 active 更新缓冲区。worker 执行任务时，把“要激活哪些后续节点”的信息写进自己的 buffer。batch 结束后，主线程按确定顺序把所有 worker buffer 合并到全局 `activeFlags`。

这解决两个问题：

- 避免多个 worker 同时写全局 `activeFlags`。
- 保持 active 更新只在原始调度允许的观察点变得可见。

### 7.4 确定性合并

MT batch 执行完成后，主线程做两类合并：

- 把 worker 本地 flag OR 回当前 `oldFlag`。
- 把 worker 的 `ActiveBuffer` 合并到全局 `activeFlags`。

合并顺序是确定的：按 worker 下标从小到大处理。当前设计优先保证可验证性和确定性，不追求第一版 runtime 的极限性能。

### 7.5 serial barrier

所有不适合并行的任务继续串行执行。这些任务包括：

- reset 相关任务。
- memory write。
- 外部模块或 DPI 类行为。
- assert/printf/stop 等特殊语句。
- 分类器无法证明安全的 unknown task。

这些任务也是并行 batch 的边界。

### 7.6 async reset 处理

async reset 没有进入 pure parallel batch。当前实现仍保留原始 gsim 的 reset helper 调用语义，包括 reset-source supernode 前后的 reset 观察点。

这是正确性上非常重要的保守选择。reset 的可见性如果变化，可能导致电路在某些周期被错误重置或没有被重置。

## 8. 当前验证证据

最近一轮 Target 04 + 06B.3 验证已经通过以下命令：

```bash
make -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim build-gsim
make -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim run-fir-test FIR_TEST=mt-repcut-lite-real-cut FIR_TEST_OUTPUT_DIR=build/fir-tests/mt-repcut-parallel-clone-on FIR_TEST_TIMEOUT=30s GSIM_FLAGS_EXTRA="--supernode-max-size=1 --mt-helper-mode=mt --mt-repcut-lite=on --mt-repcut-copy-budget=64 --mt-repcut-fanout-budget=4 --dump-mt-schedule-json --dump-mt-repcut-lite-report"
python3 /home/zhangyangjie/test/.worktrees/gsim-mt/gsim/scripts/check-mt-repcut-lite-parallel-clone.py /home/zhangyangjie/test/.worktrees/gsim-mt/gsim/build/fir-tests/mt-repcut-parallel-clone-on/mt-repcut-lite-real-cut --threads 4
python3 /home/zhangyangjie/test/.worktrees/gsim-mt/gsim/scripts/check-mt-repcut-lite-cut.py /home/zhangyangjie/test/.worktrees/gsim-mt/gsim/build/fir-tests/mt-repcut-parallel-clone-on/mt-repcut-lite-real-cut
GSIM_THREADS=4 make -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim run-mt-fir-smoke MT_FIR_SMOKE_CASES="mt-repcut-lite-real-cut mt-repcut-lite-mux-cut" FIR_TEST_TIMEOUT=30s GSIM_FLAGS_EXTRA="--mt-helper-mode=mt --mt-repcut-lite=on --mt-repcut-copy-budget=64 --mt-repcut-fanout-budget=8 --dump-mt-repcut-lite-report"
make -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim check-mt-active-buffer FIR_TEST=repro-usefulreset FIR_TEST_TIMEOUT=30s
make -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim run-fir-test FIR_TEST=repro-usefulreset FIR_TEST_TIMEOUT=30s
make -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim run-fir-test FIR_TEST=repro-usefulreset FIR_TEST_TIMEOUT=30s GSIM_FLAGS_EXTRA="--mt-helper-mode=seq"
make -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim run-fir-test FIR_TEST=repro-usefulreset FIR_TEST_TIMEOUT=30s GSIM_FLAGS_EXTRA="--mt-helper-mode=buffered-seq"
make -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim run-mt-fir-smoke GSIM_FLAGS_EXTRA="--mt-helper-mode=seq" FIR_TEST_TIMEOUT=30s
make -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim run-mt-fir-smoke GSIM_FLAGS_EXTRA="--mt-helper-mode=buffered-seq" FIR_TEST_TIMEOUT=30s
GSIM_THREADS=1 make -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim run-mt-fir-smoke GSIM_FLAGS_EXTRA="--mt-helper-mode=mt" FIR_TEST_TIMEOUT=30s
GSIM_THREADS=2 make -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim run-mt-fir-smoke GSIM_FLAGS_EXTRA="--mt-helper-mode=mt" FIR_TEST_TIMEOUT=30s
GSIM_THREADS=4 make -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim run-mt-fir-smoke GSIM_FLAGS_EXTRA="--mt-helper-mode=mt" FIR_TEST_TIMEOUT=30s
GSIM_THREADS=4 make -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim run-mt-fir-smoke GSIM_FLAGS_EXTRA="--mt-helper-mode=mt --mt-repcut-lite=on --mt-repcut-copy-budget=64 --mt-repcut-fanout-budget=4 --dump-mt-repcut-lite-report" FIR_TEST_TIMEOUT=30s
git -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim diff --check
```

小型 MT smoke tests 包括：

- `mt-comb.fir`：纯组合逻辑链，用来证明至少能形成一个 pure batch。
- `mt-reg-reset.fir`：寄存器和同步 reset 相关场景。
- `mt-async-reset.fir`：异步 reset 场景。
- `mt-memory.fir`：memory 相关场景。
- `mt-multiclock.fir`：多时钟相关场景。
- `mt-repcut-lite-real-cut.fir`：最小真实 cut 场景，证明 selected sink 的两个输入确实被 cut 并 clone。
- `mt-repcut-lite-mux-cut.fir`：包含 `mux` cloned producer 的 cut 场景，防止 clone emitter 生成非法 C++。

这些测试的作用是做早期正确性闸门，不代表已经覆盖大型设计的全部行为。

## 9. 当前代码改动范围

主要改动文件：

- `src/main.cpp`：新增 `--mt-helper-mode=buffered-seq|mt`、`--mt-repcut-lite=on|off`、RepCut budget/report 参数支持和 help 文案。
- `src/cppEmitter.cpp`：新增 ActiveBuffer 生成、buffered helper、MT pure batch runner、reset buffered overload、MT 调度生成逻辑，以及 06B.3 窄范围 RepCut-lite clone / report / proof 逻辑。
- `include/graph.h`：更新 codegen 函数声明，传递 active buffer 参数。
- `Makefile`：新增 MT active buffer 检查和 MT FIR smoke test 目标。

新增脚本：

- `scripts/check-mt-active-buffer.py`
- `scripts/check-mt-helper-equivalence.py`
- `scripts/check-mt-repcut-lite-cut.py`
- `scripts/check-mt-repcut-lite-parallel-clone.py`
- `scripts/check-mt-repcut-lite-runtime.py`
- `scripts/run-mt-fir-smoke.py`

新增 FIR 测试：

- `test/mt-comb.fir`
- `test/mt-reg-reset.fir`
- `test/mt-async-reset.fir`
- `test/mt-memory.fir`
- `test/mt-multiclock.fir`
- `test/mt-repcut-lite-candidate.fir`
- `test/mt-repcut-lite-real-cut.fir`
- `test/mt-repcut-lite-mux-cut.fir`
- `test/repro-asyncreset.fir`

当前工作树仍有未提交改动。查看状态可用：

```bash
git -C /home/zhangyangjie/test/.worktrees/gsim-mt/gsim status --short
```

## 10. 还没有完成的内容

### 10.1 RepCut-lite 仍不是通用优化框架

RepCut-lite 是必做模块。它的目标是通过有界复制减少同步和跨分区依赖，让更多 pure compute 能留在并行区域内。

当前已经完成到最小 parallel semantic proof：

- 06B.1 证明 selected task 能生成并调用 copied helper。
- 06B.2 证明真实 `pure_compute -> pure_compute` schedule edge 可以出现在 `cut_edges[]`，并且 cut-aware planner 能把被依赖边隔开的 pure tasks 合进同一个 MT pure batch。
- 06B.3 在 focused real-cut fixture 中生成 cloned value，selected successor 通过 `mtRepCutLiteTask<N>` 消费 clone，parallel-safe batch 不再强制 `workerCount = 1`。
- 06B.3 report 记录 `boundary_candidate_count`、`selected_sink_count`、`cut_edges[]`、`cut_batches[]`、`duplicated_nodes[]`、`forced_sink_cpp_ids`、`forced_sink_mask` 和 `parallel_safe_reason`，用于解释 forced sink activation 为什么安全。
- Focused parallel clone checker 现在会比较 `GSIM_THREADS=1` reference trace 和 `GSIM_THREADS=4` parallel trace。

仍未完成的是通用化：递归 clone chain、多消费者 clone、更多表达式/节点形态、跨 active word、cross-clock、state/memory/reset/special/external 边界，以及性能评估。

### 10.2 尚无性能结论

当前重点是正确性，不是性能。虽然已经有多线程执行路径，但还不能据此宣称速度提升。

后续性能报告需要至少比较：

- 原始 gsim。
- `gsim-mt` 单线程/helper 路径。
- `gsim-mt` 多线程路径。
- RepCut-lite on/off。
- 不同线程数。

FIR bootstrap 性能和已归档的 Wolvrix/SystemVerilog 前端探索必须分开标注。当前阶段只能声明 bootstrap gsim path 的结果，不能把它说成最终 SystemVerilog 输入路径性能。

### 10.3 Wolvrix 前端桥接已归档

07 阶段曾经验证过 Wolvrix `read_sv -> GRH -> frontend facts` 在 fixed small-SV smoke 上可行，但它不是通用 SystemVerilog 前端，也不能支持 XiangShan。相关代码已经从 active worktree 撤回，计划和日志已归档。

当前主线不继续接入 Wolvrix frontend。后续代码开发只在 `gsim` 工作树推进，除非重新打开前端路线，否则不要在 `wolvrix-playground` 添加新的 `gsim-mt` 实现代码。

### 10.4 XiangShan 正确性未声明

当前没有把 XiangShan 作为正确性完成项。后续如果做 XiangShan，需要小周期数启动、明确日志、通过已有 NEMU difftest 或其他 oracle 判断正确性。

不能只因为小 FIR smoke 通过，就声明 XiangShan 正确。

## 11. 建议下一步

推荐的下一阶段顺序：

1. 在 `gsim` 工作树内整理当前 Target 03/04/06B 改动，并保持回归可跑。
2. 保持 `run-mt-fir-smoke` 作为每轮回归闸门。
3. 执行 08X：直接通过 XiangShan `difftest/gsim.mk` 的 `GSIM_FLAGS` 传入 `--mt-helper-mode=mt`，构建 gsim-mt emu，并检查生成模型包含 `GSIM_THREADS`/`mtTask` 等 MT 证据。
4. 执行 08X tiny smoke：先 `GSIM_THREADS=1`，再 `GSIM_THREADS=4`，跑 100 或 1000 cycle no-diff smoke。
5. 执行 09X：在同一 XiangShan spec 上跑 `GSIM_THREADS=1/2/4` 最小性能矩阵。
6. 如果 08X/09X 阻塞，执行 10X：只修第一个 XiangShan build/run/correctness blocker。
7. 如果 09X 有数据，执行 11X：只优化 XiangShan profile 显示的主瓶颈。

短期不建议恢复 Wolvrix 前端工作，也不建议继续做与 XiangShan 无关的小 FIR-only 性能优化。现在最有价值的信息是：XiangShan gsim-mt 能不能 build、能不能 tiny run、`GSIM_THREADS=1/2/4` 相比 gsim ST 是快还是慢。

## 12. 给新人看的术语表

### RTL

Register Transfer Level，寄存器传输级硬件描述。RTL 描述硬件寄存器、组合逻辑、时钟、reset 和状态更新行为。

### FIR / FIRRTL

FIRRTL 是 Chisel 生态中的硬件中间表示。当前 gsim 主要从 FIR/FIRRTL 输入生成 C++ 仿真模型。本项目目前用 FIR 测试作为 bootstrap。

### SystemVerilog

一种常见硬件描述语言。XiangShan 等大型设计通常以 SystemVerilog 或 Chisel 生成的 Verilog/SystemVerilog 形式进入后端工具链。

### gsim

一个把 RTL 中间表示编译成 C++ 的快速 RTL 仿真器。当前仓库中的原始单线程行为是 `gsim-mt` 必须保持等价的基线。

### gsim-mt

本项目中的多线程 gsim 原型。目标是在保留 gsim 语义的前提下，让安全的组合逻辑任务并行执行。

### Wolvrix

已归档的 frontend 探索来源。07 阶段只证明 fixed small-SV facts 路径可行；当前性能主线不依赖 Wolvrix frontend，也不使用 GrhSIM runtime。

### GRH / GRHIR

GRH 是 Wolvrix 侧的图式硬件表示。GRHIR sidecar 在当前项目中主要用于分析调度图和状态边界，并为后续前端桥接做准备。

### SuperNode

gsim 内部调度单位。多个原始节点会被组织成 SuperNode。每个 SuperNode 有 `cppId`，仿真时按 `cppId` 扫描调度。

### cppId

gsim 生成 C++ 时给 SuperNode 分配的顺序编号。`cppId` 扫描顺序是保持原始调度语义的重要依据。

### activeFlags

gsim runtime 中记录哪些 SuperNode 当前需要执行的 bitset。多线程模式必须严格控制 worker 对 active 状态的写入和可见性。

### ActiveBuffer

`gsim-mt` 新增的本地 active 更新缓冲区。worker 把激活后继节点的信息写到本地 `ActiveBuffer`，主线程在确定的时机合并到全局 `activeFlags`。

### pure_compute

保守分类器认为可以安全并行的纯组合计算任务。它不能包含状态提交、memory write、reset、外部副作用或未知行为。

### serial task

必须串行执行的任务。包括 reset、memory write、外部模块、特殊语句、unknown task 等。

### async reset

异步复位。它不依赖普通时钟边沿，语义敏感。当前 `gsim-mt` 不把它放入 pure parallel batch，而是保留原始 gsim 的 reset 观察点行为。

### RepCut-lite

有界复制优化。目标是在安全预算内复制部分 pure compute，减少并行分区之间的同步和跨区依赖。当前已经有 06B.3 的窄范围语义实现：同 active word 的 `pure_compute -> pure_compute` cut 可以为 selected successor 生成 cloned value，并让经过证明的 cut batch 并行运行。它还不是通用 clone 框架。

### XiangShan

开源高性能 RISC-V 处理器项目。本项目长期希望服务于 XiangShan 级别的大型设计仿真，但当前还没有完成 XiangShan 级别正确性和性能验证。

## 13. 常用路径

当前实现工作树：

```text
/home/zhangyangjie/test/.worktrees/gsim-mt/gsim
```

基线 gsim checkout：

```text
/home/zhangyangjie/test/gsim
```

相关计划文档：

```text
/home/zhangyangjie/test/wolvrix-playground/pdocs/draft/gsim-mt-minimal-plan.md
/home/zhangyangjie/test/wolvrix-playground/pdocs/draft/gsim-mt-03-active-buffer-and-runtime.md
/home/zhangyangjie/test/wolvrix-playground/pdocs/draft/gsim-mt-04-parallel-scheduler-and-correctness.md
/home/zhangyangjie/test/wolvrix-playground/pdocs/draft/gsim-mt-06b3-parallel-semantic-repcut-lite.md
```

## 14. 推荐阅读顺序

如果完全不了解项目，建议按这个顺序看：

1. 先看本文，理解项目目标和当前阶段。
2. 再看 `README.md`，了解原始 gsim 的基本用途。
3. 再看 `docs/firrtl-6-upgrade-notes.md`，了解当前 FIRRTL 兼容性背景。
4. 如果要继续开发当前阶段，先看 `gsim-mt-minimal-plan.md`、`gsim-mt-08x-xiangshan-gsim-mt-bringup.md` 和 `gsim-mt-09x-xiangshan-mt-matrix.md`。
5. 如果以后重新打开前端路线，再看归档的 Wolvrix frontend 文档、GRH 文档和 activity-schedule / repcut 相关资料。
