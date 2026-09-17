# 阶段五：一次同步 group PREEMPT 的实机记录

**运行结论：首次 GroupIdentity PREEMPT(wait=true) 已被 stock RM 接受；INT/BG 完成、最终输出一致、BG 后续可用性和清理通过。** 本轮 M0/M1/M3 各 1 trial，项目 GET_INFO=2、PREEMPT=1、其他调度修改=0。M3 的时间顺序仍为 `ordering_ambiguous`，没有证明精确硬件抢占、INT-next 或因果性能收益。

研究仓库起始 HEAD 为 `4eb805569ed754bbdf7d65c81b181a75ff545480`，工作区干净：[initial_state](../results/phase5/initial_state.json)。实现是该 commit 后的本地修改，没有回退或 push。运行二进制及当时源码指纹见 [execution.json](../results/phase5/execution.json)；最终 rebuild 的工作二进制与实际运行时字节一致，见 [comparison](../results/phase5/validation/final_binary_comparison.json)。未修改 KMD、firmware、系统权限或全局调度策略，没有 reset、切换 primitive 或操作其他任务。

## 源码依据（不是新增实机结论）

沿用既有 [version_audit.json](evidence/phase2/version_audit.json)，补核 NVIDIA 595.58.03 commit **`db0c4e65c8e34c678d745ddb1317f53f90d1072b`**：

- [`ctrla06c.h:177–213`](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/db0c4e65c8e34c678d745ddb1317f53f90d1072b/src/common/sdk/nvidia/inc/ctrl/ctrla06c.h#L177)：PREEMPT=`0xa06c0105`，target 为 channel group。`bWait=true` 等待接口所定义的 preempt completion；`bManualTimeout=true` 使用 `timeoutUs`，最大 **1,000,000 μs**。本轮使用该上限，不以极短 timeout 制造低数字。所选 SDK 编译断言 sizeof=8、alignof=4、字段 offset=0/1/4，包含 padding 的全部字节先清零。
- [`g_kernel_channel_group_api_nvoc.c:266–278`](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/db0c4e65c8e34c678d745ddb1317f53f90d1072b/src/nvidia/generated/g_kernel_channel_group_api_nvoc.c#L266)：`KernelChannelGroupApi` / `kchangrpapiCtrlCmdPreempt_IMPL`，flags=`0x10248`，accessRight mask=0。按 **595** 定义解码为 NON_PRIVILEGED / ROUTE_TO_PHYSICAL / ROUTE_TO_VGPU_HOST / GSP_PLUGIN_FOR_VGPU_GSP；没有额外 NICE mask，不等于跳过 ownership、对象状态或 stock RM 校验。
- 595 `rmapi/client.c:rmclientValidate_IMPL` 仍验证 client/open-file 归属，`rmapi/client_resource.c:cliresAccessCallback_IMPL` 保留 access checks；`rmapi/resource.c:rmresControl_Prologue_IMPL` 的 routed FW-client 路径进入 RPC，`vgpu/rpc.c:rpcRmApiControl_GSP` 处理控制传输。沿用 [runtime_readiness](runtime_readiness.md) 的版本审查，不把公开 KMD 调用链当成本次已观测的 GSP/HW timeline。

550.120 继续单独使用 commit `5e52edb2034de7db4d8ae368dbc7c26b416bfa16` 的头文件/状态定义，没有在本次 595 宿主运行 550 私有 controls。

## 实际修改与隔离

以下均为上述研究基线之后的工作区实现；主要入口不接受外部 command/hObject，也不导入历史身份：

| 文件 / 类型或函数 | 修改 |
|---|---|
| `rm_control.{h,cpp}`：`preempt_group_wait`、`verified_group_current` | 独立 GroupIdentity 路径；当前 PID/profile/原 FD/UUID 检查与 control 共用 capture 锁；没有 channel 转换 |
| `group_query_internal.{h,cpp}`：`GroupInfoOnce::verified_for`、`GroupPreemptOnce::preempt` | GET_INFO 凭据绑定完整快照与 scope；固定同步/手动 timeout、SDK 上界；每 owner 进程最多一次、仅 trial 0；结果先发布到现有预分配 journal |
| `driver_profile.{h,cpp}` | 新 GroupActive 阶段、BG/INT owner、授权 UUID scope；group 查询/PREEMPT/其他 active 计数独立；只读 GroupInfo 和旧 channel 证据不解锁 active |
| `worker_common.cuh`：`bind_worker_group`、`prepare_short_reference/check_short_reuse`、`error_drain` | 预热/分配后各 owner 一次当前 GET_INFO；独立身份/查询输出；同 context/stream 后续检查；错误先存证据再有界检查 event |
| `bg_worker.cu` / `int_worker.cu`、`protocol.h` | `group-preempt-wait` 仅由 BG 自行控制；双进程配对；Prepare→observer→BG launch；INT enqueue→IPC→BG PREEMPT；已见 BG done 则跳过；raw 先保存再做复用检查 |
| `mode_plan.h` / `options.h` | group mode 不构造旧 RmControl，不要求 compute_channel/GR_GET_CTXSW_MODES；只允许一次、固定 iterations、plain kernel |
| `trial_record.h` / `summarize.py` | schema 2 尾部追加 BG host completion、owner done-check、setup_status；保留 ambiguous/独立维度；单次只报告原始值 |
| `run_matrix.py` | `--paired-none` 复用实际工作量并核对 scope/grid/build；跳过不适用的 strict-channel probes；按所选 GPU 检查进程；未知过程状态拒绝 |
| `group_preempt_test.cpp`、Python tests、CMake | 多-channel/授权/生命周期/当前查询凭据/一次请求/失败与 journal/owner process/配对和单样本统计测试 |

`ObjectRegistry::valid_group` 逻辑未放宽，仅修正其旧注释：现在 group control 也使用完整扫描。拓扑变化（含暂态变化）、新增第二 compute TSG、incomplete、generation/FD/PID/profile/scope 变化仍拒绝，不能在 interaction 路径自动反复 GET_INFO 或换对象。扫描分配和检查时间包含在 IPC owner 开销，真实 syscall 时间另记。hidden/direct syscall 可见性限制仍在。

## 构建与离线测试

两套 profile 编译成功，各 **8/8 CTest**：

- 550：[final_build550](../results/phase5/validation/final_build550.log)、[final_ctest550](../results/phase5/validation/final_ctest550.log)。
- 595：[final_build595](../results/phase5/validation/final_build595.log)、[final_ctest595](../results/phase5/validation/final_ctest595.log)。

相邻 JSON 保存 argv/返回码。保留原 ABI/EBADF/NV_STATUS、registry、profile/transport、mode、只读 group 一次查询、schema/Graph entry/ambiguous、进程树清理测试。新增 synthetic fixture 覆盖 1/3/8/13 compute channels、旧单-channel 仍拒绝、无授权/错误阶段/INT owner/timeout 越界、stale PID/profile/FD/成员/rebind/历史 GET_INFO、另一个 registry、每进程仅一次、syscall/RM 独立错误、CUDA timeout 后恢复 **PREEMPT 那一条** journal，以及两个 CPU 进程的 BG owner 命令路由。所有 fixture 只在临时目录，没有伪造 GPU 样本。

[8 项 CLI 检查](../results/phase5/validation/cli_checks.json) 验证两套 help、缺失授权拒绝、多 trial 拒绝、group-info 与 active flag 混用拒绝。没有通过这些解析测试发 CUDA/RM 请求。

## 当前环境与授权

[本轮 preflight](../results/phase5/preflight/preflight.json)：`cuInit=0`，`cuDeviceGetCount` **返回码0/count1**；固定最小 workload reference PASS。所选设备为 **NVIDIA A100-PCIE-40GB、CC8.0、108 SM**，UUID **GPU-99e4e85f-1945-866c-9e00-130b51df7908**。KMD、实际 libcuda 为 **595.58.03**；Toolkit 12.8.61、runtime 12.8.57/API12080，GSP query=595.58.03，MIG disabled、virtualization None（查询证据；MPS 线索不构成排除证明）。本轮没有更改权限。

目标当前未列出其他 compute process；另一 GPU 上的 VLLM process 保持运行。这个快照没有自行授予主动权限。先完成代码/测试/M0/M1，再由用户明确回复 **“确认上述条件，授权这一次测试”**，其 UUID、固定工作量和最多一次同步请求的 scope 记录在 [active_authorization.json](../results/phase5/active_authorization.json)。执行前 runner 又重新 preflight/核对目标。

第一次 M3 runner invocation 因新增 parser 把同名属性的 API 成功描述与数字输出一起比较，错误报 WORKLOAD_GPU_UNREVIEWED，**实验 worker=0、项目 controls=0**。原结果保留在 [group_preempt_smoke](../results/phase5/group_preempt_smoke/summary.md)，[diagnosis](../results/phase5/group_preempt_smoke/failure_diagnosis.json) 区分软件解析错误与真实 GPU 能力。修复为只读数字输出、重复数字仍拒绝，并加入真实日志形状的 fixture（[11 个 runner tests](../results/phase5/validation/runner_parser_regression.log)）。随后在新目录执行了尚未执行的那一次授权请求；没有重复 PREEMPT 筛选成功样本。

## 真实执行、返回和观测

从仓库根执行的实际命令（授权仅适用于当时这一次，不可从历史记录继承）：

```bash
CUDA_VISIBLE_DEVICES=GPU-99e4e85f-1945-866c-9e00-130b51df7908 \
python3 active-preempt/scripts/run_matrix.py --build active-preempt/build-595 \
  --output results/phase5/baselines --trials 1 --modes int-only none

# 在本任务收到明确授权后执行；已执行完毕，不要重复使用旧输出目录/身份：
CUDA_VISIBLE_DEVICES=GPU-99e4e85f-1945-866c-9e00-130b51df7908 \
python3 active-preempt/scripts/run_matrix.py --build active-preempt/build-595 \
  --output results/phase5/group_preempt_smoke_ready --trials 1 \
  --modes group-preempt-wait --paired-none results/phase5/baselines/none-f0-b0 \
  --test-host-confirmed
```

完整 argv、源码/二进制身份、process 返回码分别见各目录 invocation/attempts JSON。三次 trial 都 exit0，无 timeout/强杀。原始结果：[M0 raw](../results/phase5/baselines/int-only-f0-b0/raw.csv)、[M1 raw](../results/phase5/baselines/none-f0-b0/raw.csv)、[M3 raw](../results/phase5/group_preempt_smoke_ready/group-preempt-wait-f0-b0/raw.csv)。机器可读核对见 [evidence_summary](../results/phase5/evidence_summary.json)。

| M3 初始化身份 | BG owner | INT owner |
|---|---:|---:|
| PID | 2294414 | 2294413 |
| hClient / client generation | 3252373396 / 3 | 3252373401 / 3 |
| 当前 group hObject / generation | 1543503948 / 76 | 1543503948 / 76 |
| allocating FD → retained dup | 11 → 12 | 10 → 11 |
| compute TSG 候选 / 已捕获 compute channels | 1 / 8 | 1 / 8 |
| group engine / runlist | 1 / unknown | 1 / unknown |
| 当前 GET_INFO hardware TSG ID | **6** | **10** |
| GET_INFO ioctl / errno / NV_STATUS | **0 / 0 / NV_OK** | **0 / 0 / NV_OK** |
| GET_INFO host wall time | 182.542 μs | 2439.760 μs |

两个 group handle 的数值相同，但归属不同 client/PID；没有凭数字相同判成同一实体。配对记录在 [group_pair.json](../results/phase5/group_preempt_smoke_ready/group-preempt-wait-f0-b0/group_pair.json)，完整祖先/成员 token 在各 owner 的 `*_group_identity.json`。BG 的 ID6 是本次新查询输出，非硬编码或复用阶段四身份。channel engine 未显式记录，仍 unknown；不会用父 engine 伪造子 channel 值。

唯一 PREEMPT 是 [BG journal](../results/phase5/group_preempt_smoke_ready/group-preempt-wait-f0-b0/bg_control_events.jsonl) 的 operation_seq=2 / trial0，target hClient=3252373396、hObject=1543503948、retainedFD12，hChannel=null。参数原始字节 `0101000040420f00`：wait=true、manual=true、timeout=1,000,000。**ioctl=0、errno=0、NV_STATUS=0(NV_OK)**；call begin/end=3809097694867499 / 3809097695424479 ns，host wall time **556.980 μs**。INT 仅有一条初始化 GET_INFO，没有主动 control。

BG/INT 的工作量在 M1/M3 完全复用：BG iterations=**1630976**，INT=**5888**；grid216、block256、dynamic shared65536 bytes、registers25、static shared0，occupancy 估计2 CTA/SM；heartbeat period2000 ns。同一真实 arithmetic kernel + 原有 completion marker，非 Graph，没有 cancellation polling/提前退出。M1 BG instrumented solo=73.3686 ms；校准目标80 ms。INT-only 的 iterations 同样5888，实际 solo 时长随运行状态变化；不能把相同工作量等同于相同频率/调度条件。

| 原始观测，每组 n=1 | M0 int-only | M1 none | M3 group-preempt-wait |
|---|---:|---:|---:|
| interaction → INT entry（host observed） | 22.748 μs | 2264.814 μs | **485.469 μs** |
| 项目 GET_INFO / PREEMPT | 0 / 0 | 0 / 0 | **2 / 1** |
| control_status | NOT_ISSUED | NOT_ISSUED | CONTROL_ACCEPTED_EFFECT_UNVERIFIED |
| ordering_relative_to_rm | not_applicable | not_applicable | **ordering_ambiguous** |
| GPU lifetime overlap | not_applicable | yes | yes |
| INT / BG 输出一致 | PASS / N/A | PASS / PASS | PASS / PASS |

M3 中 CPU 在 RM return 前 **171.035 μs** 已看到 INT entry；但在 RM begin 后才看到，不能排除 observer 迟到，故不标 after-RM。BG completion 在 INT completion 之后，BG GPU marker interval=80.625664 ms；这些是 lifetime/完成事实，不是驻留、持续暂停或 resume 时刻。相对于 M1，这一次 host-observed 间隔较小；**一对样本不足以归因或排序**，没有报告 trial 分位数，也未测 timeslice baseline。

## 正确性、清理与结束边界

三个 trial 的 reference 比较通过。M1/M3 还在 BG 排空后，以同一 context/stream 提交固定256 iterations 的短计算，reference 通过；M3 [usability](../results/phase5/group_preempt_smoke_ready/group-preempt-wait-f0-b0/bg_context_usability.json) 记录 extra_preempt_requests=0。没有虚构 group resume API，正常依赖原调度完成排空。输出一致与中途抢占是独立验收，不能排除确定性重放或证明全部寄存器/shared-memory 保存。

各 worker 的 cudaFree、event/stream destroy、host unregister、cuCtxDestroy 返回码均0；原 context 销毁只发生在工作排空后的普通清理。group 清理前有效、清理后失效；GET_INFO 成功事实保留为历史。两个 M3 owner 各观察到清理前720/清理后724条 libcuda 自有 controls，与项目2条 GET_INFO/1条 PREEMPT 分开。5 个原始 mmap journals（含三个空 baseline journals）无损压缩为 `.bin.gz`，解压及同版本 event_dump JSON 一致，见 [journal_archive](../results/phase5/journal_archive.json)。没有丢弃停止记录、失败样本或不利时间顺序。

**仍未验证：** 纯硬件 context-save/preempt completion/resume latency、instruction/CTA 粒度、完整 context 资源覆盖、INT-next/持续 hold、因果或稳定低时延改善、queued Graph、prequeue、连续交互、取消和 buffer 回收。GSP 物理操作没有独立 trace；NV_OK 不能代替该观测。没有把 GPreempt 未用 PREEMPT 的原因解释成作者已证明其无效或更慢。

本轮到这里结束。下一步唯一最小动作是**审阅这对 M1/M3 的 raw、journal、正确性与 cleanup 证据**，再决定是否另行授权后续配对验证；当前授权的一次 PREEMPT 已使用，不自动执行新请求、10/1000 trials、其他 primitive 或 Graph。
