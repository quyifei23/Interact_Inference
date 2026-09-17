# 主动 GPU preemption 研究原型：最终基线与边界

研究原型于 Phase7 收尾，状态 **DIAGNOSTIC_ASSOCIATION_OBSERVED（限定关联）**。稳定起点 commit `5140fe0c3a3b8781329534e131145520ed25a7b5` 加本地 Phase7 修改；源码、实际运行二进制、关键证据和回归记录见 [freeze.json](../results/phase7/freeze.json)。没有自动 commit/push、release 或 tag。这里的冻结是记录可重建实现和证据边界，不是承诺一个通用生产调度器。

## 目标与实际实现

研究问题是：CPU 收到交互后，能否复用 stock NVIDIA RM primitive，让已执行/排队的 BG context 尽快让出 GPU，同时保留继续运行的能力。本轮实现并实机验证的范围是 **两个进程、同一显式 GPU、每 owner 一个受控 CUDA context、有限普通 arithmetic kernel、一次同步 group PREEMPT**。不要求 stream→八个 compute channel 中某一个的映射，也不修改每个业务 kernel 来完成驱动抢占。

当前路径：

```
owner 进程明确选择 profile / GPU scope
→ 观察本进程 allocation/bind/free，原 allocating nvidiactl FD 留引用
→ 当前唯一 compute-containing GroupBinding（完整成员/generation 快照）
→ 同绑定 GET_INFO 成功凭据
→ INT enqueue，CPU 命令发给 BG owner
→ BG 在锁内重验 PID/profile/FD/GPU/完整拓扑/授权/once gate
→ PREEMPT(group hObject, wait=true, manual timeout=1s)
→ 结果先入独立 journal
→ 双方原工作排空与 bit-exact reference
→ 同 BG context/stream 后续短计算与正常 cleanup
```

PREEMPT 没有本项目实现的 hold/resume 或永久取消语义。既有 scheduler 可以重新选择 BG；没有发明 resume ioctl。没有复制 GPreempt 全局 SecInfo 放宽，没有 QUERY_GROUP patch、新 KMD、GSP 修改、reset 或 stop-channel fallback。

## 必须分开的六类结论

| 证据层 | 当前结论 | 不外推到 |
|---|---|---|
| **源码契约** | pinned NVIDIA `ctrla06c.h` 定义 group PREEMPT、wait/manual timeout；本地窄入口保留 SDK 布局及完整当前凭据检查 | 公开 KMD 不能揭示所有 GSP/硬件内部细节；返回不保证 INT-next |
| **实机接口结果** | A100/595.58.03：多-channel compute TSG 当前绑定、原 FD GET_INFO、同步 PREEMPT 均实际接受 | GET_INFO 不能授权任意对象/其他 controls；一次对象图不是全部 CUDA 资源 |
| **应用层配对** | Phase6 10对、20run；entry9对改善/1对慢0.448μs，done10对改善；所有输出/复用/cleanup通过 | 无 p99/SLA、无普适 winner；没有改变 ambiguous 样本或删掉不利对 |
| **独立调度观测** | Phase7 D0/D1 各1次；真正 ctxsw 记录和 BG/INT kernel/NVTX 可在同一 Nsight 时间轴关联；D1 PREEMPT envelope内有 BG SAVE_END→INT RESTORE_START→INT start，随后 BG RESTORE_START 和原工作完成 | 不将 operation 等同 SM residency；不把 wrapper/control wall time 等同硬件保存耗时 |
| **推论** | 在这组 workload/环境，观测与 host-triggered context preemption、BG 之后继续执行一致 | 单次邻近事件不证明排除自然 timeslice 等其他原因；进程/workload关联不是原生 switch ID↔TSG ID证明 |
| **尚未验证** | 任意模型/kernel、Graph/多队列、重复抢占、最新请求优先、cancel/reclamation、长期稳定性 | 不把研究原型描述成完整交互式 Graph 取消系统 |

源码依据：NVIDIA550.120 `5e52edb2034de7db4d8ae368dbc7c26b416bfa16` 与595.58.03 `db0c4e65c8e34c678d745ddb1317f53f90d1072b` 的 `src/common/sdk/nvidia/inc/ctrl/ctrla06c.h`；已有 [版本审查](evidence/phase2/version_audit.json) 和 [Phase5 控制契约](phase5_group_preempt.md)。项目起点 `5140fe0` 的 `group_query_internal.cpp:GroupInfoOnce::verified_for / GroupPreemptOnce::action`、`rm_control.cpp:group_owner_action`、`object_registry.cpp:valid_group` 保留；Phase7 只追加可关闭标记与诊断适配。精确最终内容由 freeze 指纹记录，不以行号变化冒充新驱动考古。

## 已实机验证的组合与证据索引

GPU为 **NVIDIA A100-PCIE-40GB，SM80/108SM，UUID GPU-99e4e85f-1945-866c-9e00-130b51df7908**。实际 KMD/libcuda595.58.03，CUDA编译/runtime12.8，GSP595.58.03。595是experimental profile；550是独立构建/离线审查通过，**没有本机550驱动实测**。

| 阶段 | 可复核记录 |
|---|---|
| Phase4，group GET_INFO | [报告](phase4_group_binding.md)、[返回](../results/phase4/group_info595/get_info.json)、[当前绑定](../results/phase4/group_info595/group_identity.json) |
| Phase5，一次同步 group PREEMPT smoke | [实现/原始返回/正确性](phase5_group_preempt.md) |
| Phase6，匹配准备路径与10对小批量 | [计划](../results/phase6/paired595/plan.json)、[所有对](../results/phase6/paired595/pairs.csv)、[统计](../results/phase6/paired595/paired_summary.md)、[审计](../results/phase6/paired595/evidence_audit.json) |
| Phase7，机制诊断 | [报告](phase7_diagnostic.md)、[本轮计数/验收](../results/phase7/evidence_audit.json)、[D0事件](../results/phase7/D0/analysis/events.csv)、[D1事件](../results/phase7/D1/analysis/events.csv)、[D1时间线](../results/phase7/D1/analysis/timeline.txt) |

Phase6 control/treatment 的 entry中位数1144.457/408.222μs，配对差中位数736.691μs；done中位数1400.677/666.979μs，配对差中位数733.891μs。这些是原10对结果，Phase7未重算或覆盖。全部treatment ordering仍ambiguous。Phase7诊断n=1/组、PREEMPT1次，独立保存，不混入性能统计。

Phase7 的受限关联是 **当前PID/GPU + NVTX提交范围→API correlation→arithmetic CUDA context + 当前唯一compute group**。两个进程CUDA context ID都为1，在各自进程范围内有效；switch ID和hardware TSG ID不同。metadata hwId=0 没有提供可用原生 join；初始化的其他switch ID保留在范围外，submit窗口内多context会拒绝。详见两组 `analysis/identities.json`、`marker_links.json` 和 lossless `raw_records.jsonl` 选定原始行（表名/rowid可回查原SQLite）。

Nsight2024.6.2 对driver API13.2显示版本警告，使用其12.8 CUDA trace库；这不能写成官方完整支持该驱动。真实两组均有18条ctxsw/20条kernel activity，关键窗口记录、reference、复用、cleanup齐全，没有已知error/overflow/truncation；**drop count和有效时间精度仍unknown**。ctxsw使用system scope，派生数据只关联本实验PID，不解匿名其他进程。RAW CPU、GPU globaltimer、Nsight session ns分开，没有为了消除ambiguous做未经证明的时钟转换。

## 重建、离线复核与实际复现

从当前工作树构建。对应 NVIDIA source 必须是上述两个独立、未修改的 commit：

```bash
cmake -S active-preempt -B active-preempt/build \
  -DAP_RM_PROFILE=550.120 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CUDA_ARCHITECTURES=80
cmake --build active-preempt/build -j6
ctest --test-dir active-preempt/build --output-on-failure
cmake -S active-preempt -B active-preempt/build-595 \
  -DAP_RM_PROFILE=595.58.03 -DNVIDIA_SOURCE=/tmp/interact-inference-nvidia-595.58.03 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CUDA_ARCHITECTURES=80
cmake --build active-preempt/build-595 -j6
ctest --test-dir active-preempt/build-595 --output-on-failure
```

`/tmp/...` 是本机已存在 checkout 的位置，迁移时指定对应新位置，不能自动切换/安装驱动。最终两组各10/10 CTest；保留了一次550并行回归的既有超时子进程检查失败，独立/完整重跑通过，原因未确定，详见Phase7报告。实际 D0/D1 的完整 argv、输出和二进制指纹在各 `invocation.json`；[README](../active-preempt/README.md#phase-7-最小诊断复现) 给出新目录的最小入口。D1须先有当前构建匹配的D0关联数据和主动授权；每个owner新捕获并GET_INFO，不导入历史handle。

只做离线复核、不会发CUDA/RM controls的入口（原SQLite须仍在本机）：

```bash
python3 active-preempt/scripts/analyze_diagnostic.py \
  --run results/phase7/D1 --output /tmp/interact-phase7-review-new
```

原report/SQLite/info保留本地，默认不进Git，因为Nsight会嵌入环境/系统元数据，D0含敏感继承变量。版本化的字段选择原始行、schema、标准事件表、owner/journal/raw/reference/cleanup与原件指纹支持安全审阅；需要完整报告时必须先处理敏感元数据，不能直接上传。D1采用最小环境allowlist；没有为此更换驱动/runtime/libcuda。最终基线没有远端release或未经授权的tag。

## 运行、安全和可移植性边界

仅测试显式单GPU、受控双进程/context、唯一compute-containing TSG；多channel支持不等于完整context资源覆盖。channel-target的RESTART仍缺选择依据，本轮不扩展。未安装KMD或改变SecInfo/ownership；权限、driver profile、对象generation/成员/FD和进程作用域每次重验。未知ABI、歧义、stale binding、不可确认恢复均拒绝；被隐藏/direct syscall绕过的动作仍是可见性限制。

同步PREEMPT每BG进程至多一次；合法timeout、CPU独立observer、预分配journal和有限工作不等于任意GPU故障都能安全回收。错误先留证、有界清理，不能确认恢复时停止；不以reset或销毁context充当正常抢占。普通CUDA清理发生在已排空之后。profiler用wait-all/kill-none/no-duration，并保留外层超时与owner恢复证据。

bit-exact最终结果、原长kernel lifetime跨越切换、后续同context/stream成功是不同证据，不能单独或合并冒充全部register/shared-memory保存、排除重放或instruction-level粒度证明。PREEMPT接受也不保证永久停止BG、INT必定下一个或任意场景的低尾延迟。

## 应用阶段交接：只定义后续问题

研究原型收尾不等于最初完整的交互式Graph取消系统已实现。以下留给应用阶段，**本轮均未实现/新增验证**：

- 真实模型与库kernel：资源占用、外部库的context/stream行为及正确性约束。
- CUDA Graph与多个已提交graph：运行中节点和排队工作是否透明保留、何时安全复用资源。
- 多stream与资源归属：如何识别当前工作所属实体，不能把CUPTI channel ID当RM handle。
- 连续交互、旧INT变BG：context复用、请求排队与目标选择。
- latest-request-wins：新请求优先与过期输出屏蔽的应用语义。
- preempt与永久cancel的区别：当前原型保留旧队列，不删除、回滚或取消旧工作。
- 过期输出屏蔽：即使拒绝结果，旧kernel仍可能写内存。
- 显存/KV/workspace安全回收：完成、排空或有可证明安全点之前不能复用。
- 多次抢占后的可靠性：长期错误率、恢复责任与不同workload/driver组合。
- 真实负载尾延迟/SLA和维护成本：需要独立应用实验，不能从10对与一次诊断外推。
