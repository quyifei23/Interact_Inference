# 推荐：先验证 B/C 的增量作用，保留 timeslice 对照

阶段三进展：CUDA 已在实际 A100/595.58.03 可达；experimental adapter 已执行 observe-only，停于同一 TSG 下 8 个 compute channel 的绑定歧义。项目 GET/主动 controls 仍为 0；下一步是审阅 TSG 级绑定范围，而不是比较性能。详见 [phase3_bringup](phase3_bringup.md)。下文的不可达描述为阶段二时点。

**没有实测性能 winner。** 对“CPU 收到交互后使独立 INT 尽早运行、BG context 保持有效”的意图，550.120 `MAKE_REALTIME(INT) + RESTART_RUNLIST(INT channel)` 有最明确的 realtime-next 契约；`PREEMPT(BG)` 是更直接、没有额外 NICE access-right 要求的触发器。B 不包含持久 hold 或指定下一个 context 的参数，C 也仍受 runnable 状态、runlist/policy、其他 realtime 工作、粒度与 firmware 支持制约。[固定版本源码证据](source_archaeology.md)

阶段二的决定性对照是 **PREEMPT vs none（M3/M1）**、**realtime-restart vs realtime-only（M5/M4）**。M5 对 M1 的总体改善不能全部归因于主动 restart。timeslice 初始化可能避免每次交互的 RM/GSP 往返，因此不因“被动”而提前淘汰；请求/读回 timeslice 不等于真实 hardware quantum。D 的持续 disable/enable 契约仍有价值，但本轮排在恢复责任审查之后，不是默认试验。

已修正 prototype 的 generation 生命周期、多 compute channel 歧义、构造后缓存身份、失败日志丢失、Graph 主节点冒充入口、observer 迟到误判以及不完整 trial/进程树清理。新增 independent CUDA/RM probes、int-only/realtime-only、schema 2 和 progress 诊断。详情见 [phase2_changes](phase2_changes.md)，运行边界见 [runtime_readiness](runtime_readiness.md)。这些代码与离线测试提高了可测性，不是 GPU 抢占成功的证据。

当前会话 `cuInit=CUDA_ERROR_NO_DEVICE`，KMD/libcuda 为 595.58.03；本项目只保留 550.120 的执行 profile。已独立比对 595 的参数布局、control ID、NVOC/权限/RPC 路径，但未完成新 runtime adapter。没有发出调度 RM control，没有实际 latency、恢复正确性或 Graph 抢占数据。

后续只有在 CUDA 最小 workload、owner binding、只读 RM、隔离测试主机和权限条件满足后，才做 1 → 约 10 次同步 B，再做合法 NICE 下的 M4/M5。固定/复用工作量，分别测试长 CTA 和多轮短 CTA。根据完整应用样本、ambiguous 分类、可靠时间线与状态正确性来判断，而不是只选较快且看似成功的样本。

本轮没有新增 driver primitive / patch。若 binding 在实机失败，先解决最小 self-object 查询/绑定缺口，保留原 FD/SecInfo/权限；不以全局 bypass 修复可达性。若 B/C 后续被证实不足，再讨论 D 或已有 internal primitive 的受控封装。

历史结论没有改变：公开 GPreempt 调用 Query + SET_TIMESLICE，其他 wrapper 的存在不能证明作者已实验并淘汰 PREEMPT。其公开历史没有解释为何弃用；GSP latency、重选等只能作为待验证机制解释。其 RPC skip-wait helper 未被实际调用，SecInfo 修改放松 FD 归属而非赋予全部 kernel 权限。[既有考古和证据](source_archaeology.md)

单次 preempt/resume 不会取消旧 Graph 队列。固定 realtime INT context 也不能自动解决 INT-1 未结束时 INT-2 到来的问题；这需要明确旧 INT 如何 demote/被选为目标、context 复用、队列与 buffer 生命周期，以及独立的取消/回收机制。本轮没有声称这些问题已解决。
