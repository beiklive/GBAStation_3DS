# GBAStation_3DS 长卡顿 Debug 进度

测试原则：每轮只改变一个变量；固定同一 ROM、存档、分辨率、CPU 时钟、shader cache 和运行场景。Fastmem、ARM64 JIT、CPU limiter、HLE IPC 延迟、JIT cache、PICA/Vulkan 等变量不在第 1 轮同时修改。

| 顺序 | 单变量 | 基线 | 本轮设置 | 状态 | 结果 |
|---:|---|---|---|---|---|
| 1 | Dynarmic `hook_hint_instructions` | `false` | `true` | 已实测：无效 | 配置生效但仍有约 2.36 s 长卡顿；不作为修复，继续第 2 轮 |
| 2 | Core1 `CpuLimiter` | Switch 分支禁用 | 恢复原始时间片 | 已实测：未证实 | 仍有长卡顿，但与基线的 CPU 时钟、Fast Forward、严格 GPU 同步设置不一致；需受控复测 |
| 3 | HLE IPC 延迟 | Switch 下跳过 39 µs | 恢复 39 µs | 已实测：未消除 | `tw_src_ipc` 已生效，但仍有约 2.01 s CPU 长卡顿；不是根因 |
| 4 | Dynarmic code cache | 16 MiB | 32 MiB | 未开始 | — |
| 5 | Vulkan `WaitWorker` | 等待 worker 执行完成 | 有界等待 | 未开始 | 仅在 GPU/管线指标同步升高时做 |
| 6 | Fastmem | `jit_fastmem=0` | 独立开启 | 未开始 | 不作为唯一修复；同时检查正确性/崩溃 |

## 第 1 轮构建记录

- 改动文件：`src/core/arm/dynarmic/arm_dynarmic.cpp`
- 唯一运行时变量：`config.hook_hint_instructions = true`
- 预期机制：`PLD/WFI/WFE/YIELD` 返回 Dynarmic dispatcher，缩短单次 guest 忙循环占用批次
- 诊断构建：`build_switch/azahar-switch-diagnostic.debug.elf`
- NRO：`GBAStation3DSStub-diagnostic.nro`（39,957,176 bytes，SHA-256 `2c29607c1e7f18c04451ba5d56847bca5815398b0f69aa013a7b4f51f65a70e8`）
- 静态链接审计：PASS；11 个 NVK/VI 符号，未解析符号 0，Mesa 26.2.1
- Switch 实测：已收到并分析 `heartbeat.txt`（本轮样本较短，但足以判定未消除长卡顿）

## 第 1 轮实测结果（`jit_hook_hints=1`）

- 日志确认配置生效：`jit_hook_hints=1`、`jit_fastmem=0`、`jit_cache_mb=16.0`。
- 本轮样本：143 条 heartbeat、44 条 CPU phase maxima。
- CPU 长窗口：10 次 `>=100 ms`、1 次 `>=500 ms`、1 次 `>=1000 ms`。
- 最大 CPU 批次：`2358.62 ms`；`cpu_pc=0x0015eb74`；对应 `runloop_max_ms=2358.64 ms`。
- 之前基线：322 条 heartbeat、139 条 CPU phase maxima；最大 `2065.90 ms`，`>=100 ms` 28 次、`>=500 ms` 3 次。
- 结论：本轮没有消除长卡顿，最大值反而更高；虽然本轮样本较短、长窗口比例略低，不能据此宣称有改善。`PLD` 忙循环不再是最大样本，但另一个 guest/JIT 执行批次仍可连续占用约 2.36 秒，说明 hint hook 不是根因。
- 平均表现：本轮末段约 60 FPS，基线末段约 63 FPS；未观察到平均性能收益。
- 第 1 轮后续：已恢复干净的 hint 基线并制作 CPU limiter 测试包；实测结果见下方第 2 轮记录。

## 第 2 轮实测结果（CPU limiter 版本）

- 日志确认 `jit_hook_hints=0`、`jit_fastmem=0`、`jit_cache_mb=16.0`。
- 本轮样本：92 条 heartbeat、61 条 CPU phase maxima。
- CPU 长窗口：23 次 `>=100 ms`、3 次 `>=500 ms`、2 次 `>=1000 ms`。
- 最大 CPU 批次：`2279.50 ms`；`cpu_pc=0x00751af0`；对应 `runloop_max_ms=2279.53 ms`。另一处 `1978.44 ms` 位于 `0x00164e4c`。
- 两个严重样本的 `hle_gpu_ms` 约 `0.10/0.88 ms`，`vk_compile=0`、`vk_pipe_q=0`，仍属于 CPU/guest 执行长尾，不是 Vulkan 管线等待。
- 严重样本的 `cpu_core=0`；而 `CpuLimiterMulti::DoTimeLimit()` 只在 Core1 的线程重调度路径生效，因此即使本轮配置正确，也不会直接限制这些 Core0 guest 批次。
- 本轮末段约 60.3 FPS、30.1 game FPS；进程运行约 100.9 s 后正常退出。
- 受控性问题：与基线相比，本轮 `cpu_clock=100`（基线曾通过菜单设为 50）、Fast Forward 为 4.00（基线约 1.50/2.00），且基线曾切换严格 GPU 同步；因此不能据此判定 CPU limiter 的净效果。
- 结论：第 2 轮结果暂不纳入因果判断；必须用完全相同的运行设置再复测一次。

## 第 2 轮构建记录

- 唯一运行时变量：恢复 Core1 APP/SYS 原始时间片切换。
- 基线保持：`hook_hint_instructions=false`、Fastmem 关闭、HLE IPC 延迟跳过、JIT cache 16 MiB、Vulkan/PICA 不变。
- 诊断构建：`build_switch/azahar-switch-diagnostic.debug.elf`
- NRO：`GBAStation3DSStub-diagnostic.nro`（39,957,176 bytes，SHA-256 `eda2ffc218d92bdac01a4c2cb3e43f24684c2b0435490cc2c1bbf6ac6efd689a`）
- 静态链接审计：PASS；11 个 NVK/VI 符号，未解析符号 0，Mesa 26.2.1
- Switch 实测：已收到并分析 `heartbeat.txt`；因运行设置不一致，结果暂不纳入因果判断

## 第 3 轮构建记录

- 唯一运行时变量：HLE 同步请求恢复 `39,000 ns` 延迟，并进入 `WaitIPC`。
- 基线保持：`hook_hint_instructions=false`、Core1 CPU limiter 禁用、Fastmem 关闭、JIT cache 16 MiB、Vulkan/PICA 不变。
- 诊断构建：`build_switch/azahar-switch-diagnostic.debug.elf`
- NRO：`GBAStation3DSStub-diagnostic.nro`（39,957,176 bytes，SHA-256 `6b8f1e0593fbc8ae1479007fefec23a13c3c750137e8bc40a8bb77aabfbb8bf2`）
- 静态链接审计：PASS；11 个 NVK/VI 符号，未解析符号 0，Mesa 26.2.1
- Switch 实测：已收到并分析 `heartbeat.txt`；结果见下方

## 第 3 轮实测结果（HLE IPC 延迟）

- 日志确认 `jit_hook_hints=0`、`jit_fastmem=0`、`jit_cache_mb=16.0`，且 CPU limiter 保持禁用。
- `tw_src_ipc` 在 108 条 heartbeat 中全部非零，累计约 `85,343` 次，证明 `39,000 ns` 延迟路径实际执行。
- 本轮样本：108 条 heartbeat、67 条 CPU phase maxima。
- CPU 长窗口：14 次 `>=100 ms`、2 次 `>=500 ms`、2 次 `>=1000 ms`。
- 最大 CPU 批次：`2011.17 ms`；`cpu_pc=0x00117814`；对应 `runloop_max_ms=2011.19 ms`。另一处 `1409.87 ms` 位于 `0x00751af0`。
- 严重样本仍为 CPU/guest 执行长尾：`hle_gpu_ms` 低，未伴随 Vulkan 管线队列等待。
- 本轮运行约 116.6 s 后正常退出；末段约 60.3 FPS、30.1 game FPS。
- 结论：IPC 延迟已生效，但没有消除约 2 秒级 CPU 长卡顿；最多只能说明调度让出点存在轻微影响，不能视为根因修复。
- 下一步：恢复 IPC 延迟跳过的基线，仅把 JIT code cache 从 16 MiB 改为 32 MiB，进入第 4 轮。

## 第 4 轮构建记录（JIT code cache 128 MiB）

- 唯一运行时变量：Switch Dynarmic `config.code_cache_size` 从 16 MiB 改为 128 MiB。
- 已恢复基线：Switch HLE IPC 39 µs 延迟跳过、`hook_hint_instructions=false`、Core1 CPU limiter 禁用、Fastmem 关闭；Vulkan/PICA 设置不变。
- 诊断构建：`build_switch/azahar-switch-diagnostic.debug.elf`
- 独立测试 NRO：`GBAStation3DSStub-cache128-diagnostic.nro`（39,957,176 bytes，SHA-256 `fb1d63482a69c3b7853ff4fa7915521ddbb3c23315f0a2cbe9cc7cb3a1eaacc6`）
- 静态链接审计：PASS；11 个 NVK/VI 符号，未解析符号 0，Mesa 26.2.1。
- Switch 实测：约 920 s 后 `std::terminate -> SIGABRT (6)`，不是正常退出；退出日志未出现 `bad_alloc`、Vulkan 编译失败或显式分配错误。
- 运行指标：241 条 heartbeat、78 条 CPU phase maxima；`cpu_ms >= 100 ms` 10 次，最高 344.53 ms，未出现 `>=500 ms` 或 `>=1000 ms`；末次严重样本为 `cpu_pc=0x0015eb74`，并伴 `jit_inv=8`、`jit_inv_kb=3808.0`，Vulkan 队列/编译均为 0。
- 内存证据：`stderr.txt` 登记 8 个 128 MiB JIT CodeMemory（合计 1 GiB）；`exit.txt` 的 `suspect_bytes=0x8d3d4000`，约 2.21 GiB。该数据与高内存压力一致，但当前 terminate 处理器没有记录 C++ 异常类型，不能声称已证明是 OOM。
- 结论：128 MiB 明显压低了长卡顿频率和幅度，但稳定性失败；不作为可用配置。下一步降至 32 MiB，测试折中点。

## 第 5 轮构建记录（JIT code cache 32 MiB）

- 唯一运行时变量：Switch Dynarmic `config.code_cache_size` 从 128 MiB 降至 32 MiB。
- 基线保持：Switch HLE IPC 延迟跳过、`hook_hint_instructions=false`、Core1 CPU limiter 禁用、Fastmem 关闭；Vulkan/PICA 设置不变。
- 独立测试 NRO：`GBAStation3DSStub-cache32-diagnostic.nro`（39,957,176 bytes，SHA-256 `d17fd6b1b7515702451690cd25805be90c71b35eef491542a51e2bba2d708d60`）。
- 静态链接审计：PASS；11 个 NVK/VI 符号，未解析符号 0，Mesa 26.2.1。
- 运行时状态：待 Switch 实测；继续收集同一组日志并重点比较长窗口数量、`jit_inv`、退出内存统计和是否出现 `std::terminate`。

## 第 6 轮构建记录（32 MiB + Vulkan WaitWorker）

- 联合变量：保留 32 MiB JIT code cache，同时将 `Vulkan::Scheduler::WaitWorker()` 改为只等待 command queue 清空，并最多等待 `issued - 2` 个 GPU tick；不再直接持有 `execution_mutex` 等待 worker 当前执行完成。
- 其他条件保持：Switch HLE IPC 延迟跳过、`hook_hint_instructions=false`、Core1 CPU limiter 禁用、Fastmem 关闭；Vulkan fence 错误处理和 worker 优先级改动保留。
- 独立测试 NRO：`GBAStation3DSStub-cache32-waitworker-diagnostic.nro`（39,957,176 bytes，SHA-256 `4e64ccd88551906b7c92b521c40340ddf07019649cee4dccf30a2d57cacf5af4`）。
- 静态链接审计：PASS；11 个 NVK/VI 符号，未解析符号 0，Mesa 26.2.1。
- 运行时状态：待 Switch 实测。重点记录 `runloop_max_ms`、`vk_pipe_q_max_ms`、`vk_compile_max_ms`、CPU phase maxima，以及是否仍在退出时出现 `std::terminate`。

## 第 1 轮实测取证

1. 将 `GBAStation3DSStub-diagnostic.nro` 临时复制到 SD 的 `/GBAStation/core/GBAStation3DSStub.nro`（保留原文件备份）。
2. 使用与基线相同的 3DS ROM、存档、分辨率、CPU 时钟和场景运行；不要同时开启 Fastmem、严格 GPU 同步或修改其他选项。
3. 确认 `heartbeat.txt` 中出现 `jit_hook_hints=1` 且 `jit_fastmem=0`、`jit_cache_mb=16.0`。
4. 复现长卡顿后收集 `sdmc:/GBAStation/3ds/debug/heartbeat.txt`，以及同目录的 `startup.txt`、`azahar_switch.txt`、`azahar_common.txt`、`exit.txt`、`stderr.txt`。
