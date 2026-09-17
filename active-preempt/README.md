# Active preemption prototype — phase 5 group PREEMPT

**阶段五已完成一次获授权的同步 group PREEMPT smoke。** A100/595.58.03：BG/INT 各自重新绑定并 GET_INFO 成功（当前 ID 6/10），BG owner 唯一一次 PREEMPT 返回 ioctl=0、errno=0、NV_OK；两者输出、BG 同一 context/stream 后续计算及 cleanup 通过。M0/M1/M3 各 1 trial；M3 ordering 仍为 ambiguous，不能据此宣称因果抢占或性能 winner。完整原始证据与限制见 [phase5_group_preempt](../docs/phase5_group_preempt.md)。[阶段四 GET_INFO 记录](../docs/phase4_group_binding.md) 保持为历史证据。

## 构建与离线检查

从项目根目录执行（沿用现有 build 目录，不修改驱动）：

```bash
cmake -S active-preempt -B active-preempt/build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CUDA_ARCHITECTURES=80
cmake --build active-preempt/build -j4
ctest --test-dir active-preempt/build --output-on-failure
```

默认仍依赖同级 NVIDIA submodule 的 550.120 commit `5e52edb2034de7db4d8ae368dbc7c26b416bfa16`、CUDA Toolkit、C++17、CMake 3.22、Linux x86-64。控制 ABI 使用所选 profile 的官方头文件。工作量针对 A100/sm_80；其他 GPU 的 CUDA probe 可独立尝试，主调度 workload 仍限制 A100。

8 个 CTest：保留原 ABI/errno、注册表/mode/恢复、profile/transport、group 生命周期/GET_INFO、Python 分析/runner/schema；新增 group PREEMPT 的精确绑定凭据、阶段/授权/owner/FD/scope/失效拒绝、一次同步请求、错误/超时 journal 和双进程命令测试。550/595 独立构建均通过。所有 synthetic 数据只在临时目录，不是 GPU measurements。

595 使用独立构建；`NVIDIA_595_SOURCE` 应指向已检出的、未修改的 **db0c4e65c8e34c678d745ddb1317f53f90d1072b**。不能将 550 submodule 切到该 tag，也不能复用同一个 build 混入不同头文件：

```bash
cmake -S active-preempt -B active-preempt/build-595 \
  -DAP_RM_PROFILE=595.58.03 -DNVIDIA_SOURCE="$NVIDIA_595_SOURCE" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CUDA_ARCHITECTURES=80
cmake --build active-preempt/build-595 -j4
ctest --test-dir active-preempt/build-595 --output-on-failure
```

实验 adapter 将 `static_abi_reviewed / observation_enabled / binding_observed / readonly_verified / active_experiment_authorized / active_result_measured` 分开记录。build profile 在编译时固定；运行阶段在第一次 CUDA 调用前显式选择。未知版本不解码 payload，FINN/未知布局阻止绑定。静态通过不授权 active；首次 active 不要求已有 active 成功结果，但仍要求本次有效对象、GET_INFO、workload GPU 范围与操作者确认。

group 证据使用 `group_binding_observed / group_binding_valid / group_get_info_verified`。旧 `binding_observed/readonly_verified` 仍只代表严格 channel 路径。新 `GroupActive` 阶段只供 `group-preempt-wait`，并将 GET_INFO 成功凭据绑定到完整当前快照/UUID，不能仅凭状态布尔值放行。`GroupInfo` 仍只读；group 不能转换成 channel identity，也不会授权旧 B/C 路径。

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

上述 observe/identity/readonly 保留单 compute-channel 约束，当前多-channel 拓扑仍拒绝。group 查询和新 B 模式使用独立入口；没有强制读 channel-specific getter。

### 仅查询 group（多 channel 可用）

先在已有普通 CUDA workload 使用权限的环境中核对 GPU UUID / 当前 driver；为匹配的 550 或 595 build 选择新的输出目录。595 示例：

```bash
nvidia-smi --query-gpu=uuid,name,driver_version --format=csv,noheader
# TARGET_GPU_UUID 设置为本次核对的一个完整 GPU-... UUID，不能用旧文件中的 RM handles
CUDA_VISIBLE_DEVICES="$TARGET_GPU_UUID" \
LD_PRELOAD="$PWD/active-preempt/build-595/librm_control.so" \
  active-preempt/build-595/int_worker --probe-rm-group-info \
  --run-dir results/group-info-new
```

该入口先确认枚举数量=1 / UUID 匹配，完成固定 256 iterations 的有限预热/核验，再捕获新进程的唯一 compute TSG、完整成员和原 FD。只追加一次 `NVA06C_CTRL_CMD_GET_INFO`；不附带其他 getter、调度配置或 benchmark，不要求/不接受 `--test-host-confirmed`。失败保存证据后退出，不换目标、不重试。observe-only 仍追加 0 条项目 control。

输出 `group_identity.json`、`get_info.json`、完整 `*_object_graph.json/capture.txt`、profile 状态、原始 observed ioctl 元数据、独立 control journal 和 CUDA cleanup。应用自己的 controls、项目 readonly、项目 active 三类独立计数。检查 `before_cleanup_profile_state.json` 的当前有效性；context 正常结束后最终 `group_binding_valid=false` 是预期结果，旧 token 不能再操作。TSG ID=0 也可为有效输出，以成功 status 和 optional 输出标记判断。

[对象绑定](../docs/object_binding.md) 解释原 FD、generation、父子关系、唯一候选、捕获不完整与 hidden ioctl 的限制。普通 CUDA `none/int-only` 不要求捕获成功；主动模式必须验证本进程对象，同 GPU UUID、不同 process/context 与 hardware TSG ID。CUPTI channel ID 从未当作 RM handle。

## 最小对照矩阵

### 阶段五：一次 group B，独立于旧 channel 模式

`preempt_group_wait(const GroupIdentity&, timeout_us)` 只接受当前本进程的已查询绑定，不接收外部 handle。固定 `bWait=true / bManualTimeout=true`，本轮 timeout=SDK 上限 **1,000,000 μs**。校验与 syscall 同处 capture 锁；每个 BG owner 进程最多一次。PREEMPT 没有 hold/resume 或清空队列语义。

普通对照先运行；`TARGET_GPU_UUID` 必须是当前核对的完整 UUID，输出目录必须全新：

```bash
CUDA_VISIBLE_DEVICES="$TARGET_GPU_UUID" \
python3 active-preempt/scripts/run_matrix.py --build active-preempt/build-595 \
  --output results/phase5-baselines-new --trials 1 --modes int-only none

# 仅在操作者明确确认此 GPU 的隔离/使用条件并授权一次主动测试之后：
CUDA_VISIBLE_DEVICES="$TARGET_GPU_UUID" \
python3 active-preempt/scripts/run_matrix.py --build active-preempt/build-595 \
  --output results/phase5-group-new --trials 1 --modes group-preempt-wait \
  --paired-none results/phase5-baselines-new/none-f0-b0 --test-host-confirmed
```

`--paired-none` 核对完成/正确性/后续可用性、当前 GPU、grid/block/shared-memory、instrumentation 和二进制指纹，继承原 BG/INT iterations。只导入工作量配置，**不导入 FD、身份或授权**。该模式拒绝多 trial、Graph、diagnostic、force/bypass 和 extended。首次实测结果已保存，本轮不自动扩大规模或重复请求。

BG/INT 各自完成初始化/预热/分配后，保存独立 group identity，各追加一次 GET_INFO。按同 GPU UUID、不同 PID、匹配的已知 engine 和当前查询所得不同 TSG ID 验证配对；runlist 保持 unknown。BG 先 Prepare，再启动 observer，再 Launch；INT host enqueue 后通过 CPU 消息通知 BG。BG done 已可见则保留跳过事实，不发 PREEMPT、不重试。精确驻留仍 unknown。B 不执行 GET_TIMESLICE 或 GR_GET_CTXSW_MODES。

对照与 B 共用 no-op/控制消息的 send/receipt/ack 字段，但 B 另有身份检查和 syscall 成本。control 返回先写预分配 journal，完整 trial 与之分离。排空后同一 BG context/stream 再做固定 256 iterations 的正确性检查；主 trial 数据已保存、observer 已停止，不把这次复用当成精确 resume 时刻。项目 GET_INFO、PREEMPT、其他 active controls 与 libcuda 正常 controls 分开计数。

### 保留的原 channel 模式（本轮未扩展/运行）

| 组 | mode | init / interaction |
|---|---|---|
| M0 | int-only | 无 BG process/work；同一 INT launch、observer、marker、correctness 路径 |
| M1 | none | BG+INT，自然跨 context 调度；INT enqueue 后给 BG 一个 no-op CPU 消息 |
| M2 | timeslice | BG request=1 μs、INT request=1,000,000 μs；无 reservation，非完整 GPreempt hint baseline |
| M3 | preempt-wait | INT enqueue → BG owner PREEMPT(wait=true) |
| M3-group | group-preempt-wait | 多-channel TSG；BG owner 同步 PREEMPT，当前仅一次 smoke |
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

`--test-host-confirmed` 表示操作者已确认目标 GPU 可用于主动测试、没有需要保护的同卡任务，并核实 MPS/MIG/GSP/虚拟化等前提；不是授予权限。group 模式按所选 UUID 检查 compute processes（其他旧模式保留全机检查）；查询不可用也拒绝。空列表本身不等于隔离或授权。任何 probe/试验错误均保存并停止后续组。

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

只有 1 条 trial 时，summary 只列原始值，不列分位数。schema 2 追加 `T_bg_done_observed / bg_done_at_owner_check / setup_status`，旧字段语义不变；缺失新字段的旧数据不补造测量。精确 BG preempt/context-save/resume 时间仍为空。

```bash
python3 active-preempt/scripts/summarize.py results/b-first/preempt-wait-f0-b0
# 异常/被杀后的日志恢复（使用匹配该 binary 格式的 event_dump）
active-preempt/build/event_dump RUN/bg_control_events.bin RUN/bg_control_events.jsonl
```

错误时先保存证据，再尝试 owner-side enable/demote/timeslice restore；runner 对整棵进程组做有界退出，强杀会明确标记恢复未确认并停止后续测试。有限计算不保证永久 deschedule 后自行完成；没有 GPU reset 或 context destruction 充当正常抢占。
