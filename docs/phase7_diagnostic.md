# Phase 7：限定范围的机制诊断已完成

最终状态：**DIAGNOSTIC_ASSOCIATION_OBSERVED**。源码起点是 `5140fe0c3a3b8781329534e131145520ed25a7b5`，初始工作区干净；没有回退、驱动/权限/时钟/全局策略修改、reset、push、tag 或 PR。当前实现与证据指纹见 [冻结记录](../results/phase7/freeze.json)。该状态表示有真实事件及有边界的关联，不表示已解决完整交互式 Graph 取消系统。

## 工具、环境与授权

本次重新执行 [preflight](../results/phase7/preflight/preflight.json)：cuInit、单设备枚举、有限 kernel/reference 成功。目标 **A100-PCIE-40GB / SM80 / 108 SM**，UUID `GPU-99e4e85f-1945-866c-9e00-130b51df7908`；KMD 与实际 libcuda `595.58.03`，driver API 13020；编译/runtime 为 CUDA 12.8，实际 libcudart 12.8.57。595 experimental 仍使用独立源码 commit `db0c4e65c8e34c678d745ddb1317f53f90d1072b`。没有把 550 的 ABI 判断移植成运行通过。

用户在本轮明确“本轮不需要新的手动授权，可以直接执行”，随后允许“不止一次”：[授权范围](../results/phase7/authorization.json)。实现仍采用最小 D0/D1，没有额外采样、排障重跑或第二次主动请求。两次运行前核对同一 UUID、当前设备与进程快照；空列表不代替授权。另一 GPU 的既有 VLLM 未被操作。

选择 **Nsight Systems 2024.6.2.225-246235244400v0**，路径 `/usr/local/cuda/bin/nsys`。保存了 [版本/CLI/help/environment probe](../results/phase7/backend_probe/probe.json)。当前 UID/EUID=1004、effective capabilities=0、perf paranoid=4，CPU profiling 环境检查失败；关闭 CPU sampling/context-switch 后，GPU ctxsw **实际采集成功**。没有通过这些 CPU 错误推断 GPU 不可用，也未改 RmProfilingAdminOnly=0 的现状。当前 GPU 的 MIG/virtualization 字段与 GSP595.58.03见 preflight/trace；MPS 仍仅有进程和环境线索，不能宣称完全排除。

[官方 2024.6 User Guide](https://docs.nvidia.com/nsight-systems/2024.6/UserGuide/index.html#gpu-context-switch) 同时描述 root 要求与普通用户本人 PID/context 的可见性；采用实际 D0 结果判断。版本化 URL、下载指纹和使用章节见 [official_source](../results/phase7/backend/official_source.json)。没有使用最新版语义替代安装版。该版本 gpuctxsw 是 system scope，没有单 GPU/PID 过滤选项；CUDA/NVTX 采用 application process tree。只派生关联当前两 owner，不解匿名其他进程。

CUPTI12.8（API26）与已安装 cu13/CUPTI13.0（API130001）头文件都没有 `COMPUTE_ENGINE_CTX_SWITCH`，匹配库 GetVersion 的返回已保存。没有尝试 enable、手抄新 enum、安装库、混入 CUPTI subscriber；不使用 PREEMPTION activity 替代目标事件。

## 最小修改和回归

- `diagnostic_trace.{h,cpp}`：可关闭 NVTX、128 槽固定记录、RAW 调用前后 bracket、加载库/注入环境记录；不发 CUDA/RM controls。导出在 trial 外和错误清理中，原控制 journal 先保存。
- `options.h`、两个 worker：`--diagnostic-trace` 只允许具名的单次 group-mode 普通 kernel，独立于 diagnostic-progress；保留 arithmetic、reference、observer、固定工作量与后续复用。
- `group_query_internal.cpp:GroupPreemptOnce::action`：共同准备范围与 `preempt_ioctl_envelope` 分开，后者使用真实 journal operation_seq。参数、权限、generation、原 FD、GET_INFO 凭据、once gate 没有放宽。
- `probe_diagnostic.py`、`run_diagnostic.py`、`analyze_diagnostic.py`：只支持本次 Nsight 路线，检查实际导出 schema3.16.1；D0 数据门槛、D1 reservation、无自动重试、独立诊断分析。
- 两 profile 各 **10/10 CTest** 通过：[550](../results/phase7/tests/ctest550.log)、[595](../results/phase7/tests/ctest595.log)。诊断 Python 8 项契约，以及 C++ 标记关闭/序号对应/once/未授权/D0 保护通过。原 ABI、生命周期、权限、双进程 owner、accepted 后 timeout journal、恢复/进程树测试保留。synthetic 仅在临时测试目录。

一次并行回归中，550 的既有 `runner_contract_test:test_owner_and_descendant_timeout_cleanup` 报 `live orphan remained`，日志保留在 [initial failure](../results/phase7/tests/ctest550_initial_failure.log)。随后未发现匹配的 synthetic 子进程，单独重跑通过；没有修改该测试、断言或timeout来掩盖失败。此间歇性失败的原因未确定，不能写成已修复或已证明是并发导致。真实 D0/D1 无超时、退出/cleanup及事后无遗留owner均另有记录。

## D0/D1 实际运行

两组 BG/INT iterations=1630976/5888，grid=216、block=256、dynamic shared=65536、registers=25、occupancy估计2 CTA/SM，heartbeat2000 ns、trigger delay3000 μs、Graph=false。没有拆 kernel、cancellation polling、资源空置或临时调整相位。Nsight 注入显著影响 reference/CPU API 采集开销；本轮不是性能样本。

| 项目 | D0 group-bound-none | D1 group-preempt-wait |
|---|---|---|
| trials / PREEMPT | 1 / 0 | 1 / 1 |
| 当前 BG / INT PID | 2385047 / 2384964 | 2393380 / 2393301 |
| 当前 GET_INFO | 各1次，均 ioctl0/errno0/NV_OK | 各1次，均 ioctl0/errno0/NV_OK |
| 当前 hardware TSG ID | 6 / 10 | 6 / 10 |
| 提交窗口内 switch context ID | 476279 / 476303 | 476477 / 476501 |
| CUPTI CUDA context ID | 各自进程的1 | 各自进程的1 |
| 原始 ctxsw 总条数 / trial内条数 | 18 / 6 | 18 / 6 |
| reference / BG短复用 / cleanup | 全通过 | 全通过 |

ID 是当次运行结果，未从历史加载。两 owner 的 RM group handle 恰好同值1543503948，不能据此视为同一对象；原 allocating FD、client/generation、PID/GPU 与 GET_INFO 作用域分别保存。每 group 当前捕获8个 compute channel，没有选择其中一个；profile early_unobserved_ioctls=0，当前 group 有效。仍不证明 hidden/direct syscall 的全面可见性。

D1 PREEMPT：BG PID2393380、operation_seq=3、目标当前 **group hObject1543503948**，不是 TSG ID6。参数 `0101000040420f00`（wait=true、manual=true、timeout1000000us）；**syscall=0、errno=0、NV_STATUS=0**。RAW begin/end=3815992268913379 / 3815992269416745 ns，**host wall time503366 ns**，不是硬件抢占耗时。项目合计 GET_INFO4、PREEMPT1、其他调度0；四个 owner cleanup 前观察到的应用/工具正常 RM controls 各749，独立计数。[完整审计](../results/phase7/evidence_audit.json)。

实际 argv、二进制/源码指纹、环境、collection/export 日志、owner JSONL/raw/cleanup 均在 [D0](../results/phase7/D0/invocation.json)、[D1](../results/phase7/D1/invocation.json)。D0/D1 worker 二进制完全相同。D1 的采集环境改成 allowlist，移除无关 agent/SSH/凭据变量，保留工作量所需环境；该差异已记录。D0 原始报告嵌入了继承的敏感环境，原文件保留本地、默认不进 Git，不得未经处理发布。D1 没有 token 环境项。工具临时 per-PID stdio 文件已不可取得；默认 show-output=true 的合并 collection.log 与原 report 保留，见各 `stdio_capture.json`。

## 身份、时间域和事件解释

实际 schema、enum 和 rowid 在 `analysis/actual_schema.json`、`switch_enum.json`。关联先取 **当前 PID/GPU**，用提交 NVTX 范围内 CUDA API correlationId 找到具体 arithmetic activity 和其 CUDA context，再关联同进程当前唯一 compute-group 查询。原生 context-info 的 hwId=0 原样记录，未用它或数值相等去连接 switch ID/TSG ID；这是限定 workload 生命周期的关联推论，不是完整硬件一一映射。

D0 最初离线解析发现 trial 开头有 INT 初始化阶段另一个 switch ID。收窄到每 owner **submit→trial-end** 后可唯一关联；更早记录仍保留并标 outside scope，不猜其含义。初版 assessment 与修正理由也保存：[analysis_review](../results/phase7/D0/analysis_review.json)。没有重新采集 GPU 数据。若提交窗口内仍有多个 context，则当前适配器拒绝 D1。

Nsight 存储的单位为 ns，有效精度未知。导出 normalization=false、shift=0，UTC session origin 和原 trace span 均保留。RAW CPU、GPU globaltimer 与 Nsight 的 timestamp 不直接相减；`marker_links.json` 只记录关联和 bracket，没有拟合跨域时钟。下表仅在 **D1 Nsight 时间轴内**，相对 interaction NVTX 1543368432 ns 作整数差：

| 相对时间 ns | 实际记录 | 原 SQLite 表 / rowid |
|---:|---|---|
|−3012388|BG 原始 arithmetic start|CUPTI_ACTIVITY_KIND_KERNEL /13|
|0|interaction NVTX|NVTX_EVENTS /5|
|92462|preempt_ioctl_envelope begin|NVTX_EVENTS /8|
|308299|BG SAVE_END|GPU_CONTEXT_SWITCH_EVENTS /10|
|402763|INT RESTORE_START|GPU_CONTEXT_SWITCH_EVENTS /11|
|407755|INT arithmetic start|CUPTI_ACTIVITY_KIND_KERNEL /14|
|596879|preempt_ioctl_envelope end|NVTX_EVENTS /8|
|656205|INT arithmetic end|CUPTI_ACTIVITY_KIND_KERNEL /14|
|730413|INT SAVE_END|GPU_CONTEXT_SWITCH_EVENTS /12|
|826637|BG RESTORE_START|GPU_CONTEXT_SWITCH_EVENTS /13|
|61939808|BG 原始 arithmetic end|CUPTI_ACTIVITY_KIND_KERNEL /13|

完整端点与 RAW journal：[D1 timeline](../results/phase7/D1/analysis/timeline.txt)、[events.csv](../results/phase7/D1/analysis/events.csv)。D0 也观测到自然的 BG/INT切换：[D0 timeline](../results/phase7/D0/analysis/timeline.txt)。两组都只使用导出 enum 的 SAVE_END/RESTORE_START 原名，没有把 CUPTI START/END 套进来，不计算寄存器保存耗时、不画 SM 驻留条。

## 九项证据检查与边界

1. **谁运行**：当前 PID、GPU、NVTX/API correlation、arithmetic CUDA context 与 GroupIdentity 记录齐全；具体数值见 `analysis/identities.json`。非本实验身份不派生。
2. **请求前 BG 未完成**：BG entry 已被 CPU 看到；done-before-control/owner-check 均为0，且同一原始 BG activity 在请求标记后才结束。仍不把 entry 解释成请求瞬间的确定驻留。
3. **切换 operation**：两组都有 trial 内真实 SAVE_END/RESTORE_START；不是仅初始化/销毁事件。
4. **请求相关顺序**：D1 表中 BG SAVE_END、INT RESTORE_START、INT start 均落在 PREEMPT NVTX envelope 内。actual syscall 仍在 RAW journal，原 CSV ordering_ambiguous 保持；没有宣称纯硬件顺序全被校准。
5. **entry 是否一致**：普通 kernel 的 entry/main 同一 node0/launch0；CUDA activity 与 API 对应。D1 INT arithmetic lifetime248450 ns，GPU entry→done marker249696 ns，分别在各自域计算；marker位于 kernel 内且 done 属后续 completion，不能当相同端点或精确第一条 warp。
6. **BG 继续执行**：D1 INT activity结束后有BG RESTORE_START；同一个原始BG activity之后结束，与恢复继续执行一致。不以新 kernel 成功单独证明从被中断指令恢复，也不能排除确定性重放。
7. **原工作结果**：两组 BG/INT bit-exact reference 通过。
8. **后续可用性**：两组 BG 原 context/stream 的256-iteration短计算通过；每个 CUDA cleanup返回0，没有恢复未知或超时。
9. **缺口**：两进程均有工具 warning：driver API13.2 不在此 Nsight build 支持范围，工具使用12.8 trace库。没有 Error/已报告 overflow/truncation，ctxsw序号无可见间隙；**drop count仍unknown**。没有原生 switch-ID→TSG-ID 映射，也没有精确保存/恢复硬件时间。

host-observed应用原始值仅作记录：D0 entry/done=1402645/1669231 ns；D1=414070/663386 ns。每组n=1且开启诊断，**不加入 Phase6、不算p95/p99、不声明性能进步或winner**。这次事件序列与 host-triggered preemption 一致，也比仅NV_OK/应用延迟提供了更直接的独立观测；自然调度在D0也存在，单次相邻事件不独立证明唯一因果、持续暂停或无条件INT-next。

本轮不再增加请求。研究原型按现有证据收尾，复现与应用阶段边界见 [prototype_final](prototype_final.md)。
