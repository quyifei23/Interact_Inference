# Active preemption prototype — phase 3 bring-up

保留第二阶段的 generation/原 FD、对照模式、Graph markers、schema 2 与恢复日志。**本次 CUDA 最小 workload 已在 A100/595.58.03 成功；595 observe-only 捕获到同一 TSG 下 8 个 compute channel，绑定按约束拒绝。项目 GET/调度 controls=0，benchmark trials=0。** 真实证据与唯一下一步见 [phase3_bringup](../docs/phase3_bringup.md)。

## 构建与离线检查

从项目根目录执行（沿用现有 build 目录，不修改驱动）：

```bash
cmake -S active-preempt -B active-preempt/build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CUDA_ARCHITECTURES=80
cmake --build active-preempt/build -j4
ctest --test-dir active-preempt/build --output-on-failure
```

默认仍依赖同级 NVIDIA submodule 的 550.120 commit `5e52edb2034de7db4d8ae368dbc7c26b416bfa16`、CUDA Toolkit、C++17、CMake 3.22、Linux x86-64。控制 ABI 使用所选 profile 的官方头文件。工作量针对 A100/sm_80；其他 GPU 的 CUDA probe 可独立尝试，主调度 workload 仍限制 A100。

6 个 CTest：原 ABI/errno/空身份、注册表与 mode/恢复/async gate、profile/transport/授权门槛、Python 分析、runner admission/进程树、实际 C++ CSV/journal 与 Python parser 集成。所有 synthetic 数据只在临时目录，测试结果不是 GPU measurements。

595 使用独立构建；`NVIDIA_595_SOURCE` 应指向已检出的、未修改的 **db0c4e65c8e34c678d745ddb1317f53f90d1072b**。不能将 550 submodule 切到该 tag，也不能复用同一个 build 混入不同头文件：

```bash
cmake -S active-preempt -B active-preempt/build-595 \
  -DAP_RM_PROFILE=595.58.03 -DNVIDIA_SOURCE="$NVIDIA_595_SOURCE" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CUDA_ARCHITECTURES=80
cmake --build active-preempt/build-595 -j4
ctest --test-dir active-preempt/build-595 --output-on-failure
```

实验 adapter 将 `static_abi_reviewed / observation_enabled / binding_observed / readonly_verified / active_experiment_authorized / active_result_measured` 分开记录。build profile 在编译时固定；运行阶段在第一次 CUDA 调用前显式选择。未知版本不解码 payload，FINN/未知布局阻止绑定。静态通过不授权 active；首次 active 不要求已有 active 成功结果，但仍要求本次有效对象、GET_INFO、workload GPU 范围与操作者确认。

## 分阶段探测

输出目录必须是新的，原记录不会被覆写：

```bash
python3 active-preempt/scripts/preflight.py --output results/preflight-new
# 独立 CUDA JSONL；--probe 是兼容别名
active-preempt/build/int_worker --probe-cuda

# 仅在 profile 匹配时捕获/验证；失败仍保存 inventory
LD_PRELOAD="$PWD/active-preempt/build/librm_control.so" \
  active-preempt/build/int_worker --probe-rm-identity --run-dir results/identity-new
LD_PRELOAD="$PWD/active-preempt/build/librm_control.so" \
  active-preempt/build/int_worker --probe-rm-readonly --run-dir results/readonly-new
```

CUDA probe 不先检查 550.120，不依赖 nvidia-smi 成功；最小 workload 有限。RM probe 使用 1 CTA / 256 iterations，不先校准 80 ms BG。GET_INFO 成功后，GET_TIMESLICE / GR_GET_CTXSW_MODES 的可选错误分别保存。没有 reliable binding 则拒绝 active controls，不向 stock driver 发 GPreempt QUERY_GROUP。

595 先使用匹配 build 的 `--probe-rm-observe`，它**不追加任何项目 RM control**。`observed_ioctls.jsonl` 是 libcuda 正常调用的包络元数据；项目 controls 单独计数。成功建立唯一候选后，才在新的进程重新绑定并做 readonly，不能从旧文件读 handle 使用：

```bash
LD_PRELOAD="$PWD/active-preempt/build-595/librm_control.so" \
  active-preempt/build-595/int_worker --probe-rm-observe --run-dir results/observe-new
# 仅在 observe 通过后；同一进程内重新捕获、GET_INFO，再读可选 getter
LD_PRELOAD="$PWD/active-preempt/build-595/librm_control.so" \
  active-preempt/build-595/int_worker --probe-rm-readonly --run-dir results/readonly-new
```

**当前真实 observe 因多 compute channel 拒绝；本轮没有运行上述 readonly 命令或下述 benchmark 矩阵。** 不删除唯一性检查，不擅自选择一个 channel，不填写 `--test-host-confirmed`；下一步先审阅 [TSG/channel 绑定范围方案](../docs/phase3_bringup.md)。

[对象绑定](../docs/object_binding.md) 解释原 FD、generation、父子关系、唯一候选、捕获不完整与 hidden ioctl 的限制。普通 CUDA `none/int-only` 不要求捕获成功；主动模式必须验证本进程对象，同 GPU UUID、不同 process/context 与 hardware TSG ID。CUPTI channel ID 从未当作 RM handle。

## 最小对照矩阵

| 组 | mode | init / interaction |
|---|---|---|
| M0 | int-only | 无 BG process/work；同一 INT launch、observer、marker、correctness 路径 |
| M1 | none | BG+INT，自然跨 context 调度；INT enqueue 后给 BG 一个 no-op CPU 消息 |
| M2 | timeslice | BG request=1 μs、INT request=1,000,000 μs；无 reservation，非完整 GPreempt hint baseline |
| M3 | preempt-wait | INT enqueue → BG owner PREEMPT(wait=true) |
| M4 | realtime-only | init MAKE_REALTIME(INT)，interaction 不调用 restart |
| M5 | realtime-restart | 同 M4 init；interaction 追加 RESTART_RUNLIST(INT channel) |

`realtime` 是 M5 的兼容别名，raw 中统一写 `realtime-restart`。重点比较 **M3−M1**、**M5−M4**。no-op 与 B/D 尽量共用一次 CPU 消息路径；C 在本进程执行 control，不能声称全部路径 CPU 开销完全相同。IPC send/receipt/ack 与实际 RM ioctl 分开记录。每配置是全新进程/context，防止状态污染；不擅自改 runlist policy，未知值写 unknown。

先 CUDA/identity/readonly，再在隔离且获授权的 A100/550.120 主机做 1 次：

```bash
python3 active-preempt/scripts/run_matrix.py --output results/plain-first \
  --trials 1 --modes int-only none

python3 active-preempt/scripts/run_matrix.py --output results/b-first \
  --trials 1 --modes preempt-wait --test-host-confirmed

# 仅在 worker 已有合法 NICE 权限时；工具不会 setcap/sudo
python3 active-preempt/scripts/run_matrix.py --output results/c-first \
  --trials 1 --modes realtime-only realtime-restart --test-host-confirmed
```

`--test-host-confirmed` 表示操作者已确认是隔离测试主机、没有需要保护的任务，并核实 MPS/MIG/GSP/虚拟化等前提；不是授予权限。发现其他 compute processes 或任意 probe/试验错误，runner 保存证据并停止后续组。

从 smoke 的 `*_identity.txt` 读取真实 iterations；跨 invocation 配对时使用同一数值，而不是把各自校准的不同工作量混为一组。一个矩阵内部会冻结已校准工作供后续配置使用。下面的变量应设置为上述实际记录值，不能随意填造：

```bash
# BG_ITERS / INT_ITERS 来自已完成 smoke 的 identity 文件
python3 active-preempt/scripts/run_matrix.py --output results/b-ten \
  --trials 10 --modes preempt-wait --test-host-confirmed \
  --smoke-evidence results/b-first/preempt-wait-f0-b0 \
  --bg-iterations "$BG_ITERS" --int-iterations "$INT_ITERS"

# 只有正确性、观测与恢复通过后，才运行 >=1000 trials
python3 active-preempt/scripts/run_matrix.py --output results/b-statistics \
  --trials 1000 --modes preempt-wait --test-host-confirmed \
  --smoke-evidence results/b-ten/preempt-wait-f0-b0 \
  --bg-iterations "$BG_ITERS" --int-iterations "$INT_ITERS"
```

其他组也各自经过 1 → 约 10 → 统计，并保留匹配的对照。`configuration.json` 保存实际配置；runner 核对 mode、Graph、diagnostic/performance、force/bypass、waves、heartbeat、timeslice 和 iterations。1 → 10 未显式给 iterations 时继承 smoke 数值，不重新校准；1000 次仍要求显式给出匹配数值。更换 GPU/驱动/编译配置后重新 smoke。较大样本要求一个 mode / invocation 与其匹配 smoke、完成/恢复证据；不会仅因 NV_OK 自动升级实验。

## 条件性扩展

- `preempt-async` 保留，但默认不运行；须 `--allow-extended`，在同步路径与完成/恢复边界审查后使用。每次最多一个 pending async，请求之后必须 CUDA drain 才可重发。
- `--force 0|1 --bypass 0|1` 只对 restart 有意义；非默认值也要求 `--allow-extended`，不默认展开四组合。
- `disable` / `disable-split` 保留，默认不运行；同样要求 `--allow-extended`，操作者必须先审查其 owner-side enable、部分失败和 timeout 恢复。始终 rewind=false / event=NULL。
- W2：`--cta-waves 1`；W1：例如 `--cta-waves 8`。分别记录并固定 iterations，实际资源与 occupancy estimate 在 identity 中；只有短 CTA 延迟低不能证明 instruction-level preemption。
- `--diagnostic-progress` 单独运行 device counters/sequence-sum 校验，无 cancellation polling。诊断额外开销不混入 performance 样本。

Graph 入口在第一个真实计算节点，main 在中间节点；详见 [测量契约](docs/measurement.md)。只有普通 primitive 与对照存在可解释证据后，才显式给 `--graph-evidence-reviewed` 和已完成 plain-active `--primitive-evidence`，再加 `--graph`。自动门槛只是数据完整性检查，标志代表操作者已审阅额外机制证据，不能由 RM 返回成功代替。当前只覆盖单次 graph launch 的代码路径；prequeue 1/4/16、连续交互、latest-request-wins、取消及 buffer 回收未实现。

## 证据与异常恢复

每组输出 `raw.csv`（schema 2）、`configuration.json`、identity/inventory、calibration、必要时 BG CTA/heartbeat、`*_control_events.jsonl` / `.bin`、recovery/CUDA cleanup/status 或 failure。没有 GPU 时 runner 停在 preflight，不生成 GPU raw.csv。

控制返回先存入预分配 mmap 槽，再等待完整 trial。CTRL 接受、GPU 行为、延迟变化和正确性分开；CPU 在 RM 后看到 marker 默认 **ordering_ambiguous**。`summarize.py RUN` 输出全部有效应用样本和各维度数量；如需假设性的时钟映射，可显式 `--calibration-margin-ns N`，其 margin 不是测得的硬保证。

```bash
python3 active-preempt/scripts/summarize.py results/b-first/preempt-wait-f0-b0
# 异常/被杀后的日志恢复（使用匹配该 binary 格式的 event_dump）
active-preempt/build/event_dump RUN/bg_control_events.bin RUN/bg_control_events.jsonl
```

错误时先保存证据，再尝试 owner-side enable/demote/timeslice restore；runner 对整棵进程组做有界退出，强杀会明确标记恢复未确认并停止后续测试。有限计算不保证永久 deschedule 后自行完成；没有 GPU reset 或 context destruction 充当正常抢占。
