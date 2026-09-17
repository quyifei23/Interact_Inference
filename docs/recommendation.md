# 推荐：复用现有 RM，先测试 C/B/D，不新增 driver primitive

对“CPU 事件到来后，让独立 INT context 尽快获得 GPU，BG 状态仍有效”的目标，**`MAKE_REALTIME(INT_TSG) + RESTART_RUNLIST(INT_CHANNEL)` 是调度意图最匹配的现有接口组合**。550.120 文档明确把 realtime 排前，并让 client 手动 restart 以触发当前工作抢占。它值得优先验证，但尚不能称实测最优方案。

`PREEMPT(BG)` 是更简单、且 550.120 元数据不要求 NICE 的主动触发器。它不包含持续 hold 或指定 next context 的参数；不能据此承诺 `PREEMPT(BG) → RUN(INT)`。如果需要“INT 完成以前 BG 一定不再调度”，`FIFO_DISABLE_CHANNELS(disable=true, onlyScheduling=false, rewind=false, event=NULL)` 加显式 enable 的契约更接近目标。

| 需求 | 当前最贴合的 primitive | 尚缺的决定性证据 |
|---|---|---|
| 向当前 BG 发主动 context/TSG preempt | PREEMPT(BG_TSG) | A100 mode/granularity、完成等待、BG 重选概率 |
| 指向更高优先级 INT next | MAKE_REALTIME(INT) + RESTART_RUNLIST(INT channel) | NICE/firmware 支持，CTA 文档歧义、long-CTA 延迟，实际 queue readiness |
| BG 保持停止直到应用恢复 | FIFO_DISABLE_CHANNELS + enable | 所有相关 channels、CUDA queued work/stream/graph 状态连续、失败后恢复 |
| 避免每次 interaction 发同步 RM RPC | GPreempt 初始化 timeslice 配置 | 真实 hardware quantum、尾延迟和与主动触发的公平比较 |

比较不能只看控制 API 名称或论文采用了什么。timeslice 的事件路径可以没有 ioctl/GSP 往返；C 的明确调度语义也可能受 CTA 粒度限制；B 可以切出但马上被重选；D 提供更强暂停语义但增加恢复 control。这些是源码支持的机制差异，**没有真实延迟数据，不能宣布性能 winner**。

两点需要特别纠正：

1. GPreempt 的发布代码只调用 Query + SET_TIMESLICE；PREEMPT/RESTART/DISABLE 等 wrapper 没有 callsite。六个公开提交没有作者弃用 PREEMPT 的解释，不能将 GSP latency、重选或权限问题伪装成作者实验结论。
2. 其 `Nv04ControlWithSecInfo → Nv04Control` 改动丢弃 clientOSInfo，放松 FD 归属校验，但仍保留用户权限等级和 RS rights；不能说成所有 control 变 kernel-privileged。新实现没有复制此改动。RPC patch 中 skip-wait helper 未被调用，正式 RPC 仍同步。

## 推荐 prototype 和实施状态

已提供 [active-preempt](../active-preempt/README.md)：两个 process，各自显式创建 context；从本进程 allocation 捕获 group、compute child channel、subdevice，并复用原控制 FD；读回 hardware TSG ID、GPU UUID；拒绝身份歧义。用户态先用 stock RM owner/权限路径，不能发现句柄或不获授权就报告拒绝，不进行绕过。

实现包括：有限 arithmetic BG/INT、单独 reference 校验 register/shared-memory/output、mapped start/done/heartbeat、1000 次前后 ping 校准、RM errno/status 分离、B wait/async、C force×bypass、D disable/enable 和 split 对照、原 timeslice 数值 baseline、CTA waves 对照、可选三节点 CUDA Graph、raw/分位数脚本。Graph 代码已编译但未测试，runner 在普通-kernel evidence 之前拒绝 graph 阶段。

**已验证**：CUDA 12.8.61 / sm_80 编译与链接、host-side ABI/错误处理/身份缺失拒绝检查、统计过滤测试；详见 [results/summary.md](../results/summary.md)。**未验证**：libcuda 捕获可用性、真实 A100 TSG 身份、权限成功、GPU preempt、state save/restore、任何延迟或 graph 透明性。

本机 GPU 设备不可访问，宿主模块版本还是 595.58.03，因此没有 GPU benchmark，也没有 `results/raw.csv`。没有 build/install/unload kernel module；没有生成 `patches/active_preempt.patch`，因为尚未证明需要新 KMD primitive。条件性窄接口方案见 [minimal_kmd_interface.md](minimal_kmd_interface.md)。

## 下一步如何改变判断

先在 disposable A100/550.120/GSP host 上做 10 次 smoke，再每配置 1000 trials。先检查 capture inventory / GET_INFO，再记录 C init 前后 `GR_GET_CTXSW_MODES` 的返回（失败同样保留）。普通 owner 先测 B/D；有合法 NICE 权限后测试 C 四组合。long-CTA 和多 short-CTA 都必须保留，避免把 CTA 等待误判成 context-save 成本。

若 C 在 long-CTA 内部仍低延迟切换、INT next 且 BG 正确恢复，就采用 C。若 C 受 CTA 限制而 B 可 instruction-level 切出，则需要解决 B 的 next/hold 调度语义，D 可作强语义对照。若 RPC 是主导成本，才讨论 E 的窄接口和 transport 设计；单纯删 receive 不成立。若 A 的实际尾延迟更好且满足交互需求，也应接受 A 的实验结果。

精确 GPU context-save completion 仍需可靠的 GPU/context-switch trace；本 prototype 保留 exact BG preempt/resume 列为空，只输出明确标注的 sentinel 线索和 observation uncertainty。它不会用 ioctl return、单次 heartbeat 静止或 CUDA kernel timeline 长条替代这一证据。
