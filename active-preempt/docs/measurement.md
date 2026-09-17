# 时间戳、校准和正确性口径

## Clock domains

所有 CPU timestamp 是各进程共享 epoch 的 `CLOCK_MONOTONIC_RAW` ns。GPU timestamp 来自 `%globaltimer`，只在同一物理 GPU/context 时间线内直接比较；没有把 `clock64()` 当全 GPU 同步时钟，也没有直接相减 CPU/GPU timestamp。

每 worker 前后各 1000 次 mapped-memory ping-pong：CPU 记录 send，GPU 看到 request 后记录 globaltimer 并 publish ack，CPU 记录 observed。可得 CPU−GPU offset 区间 `[cpu_send − gpu_time, cpu_observed − gpu_time]`。报告最小 RTT 区间和所有 RTT 分位数；后测检查漂移。此区间包含双向传播和观测，不可当作单向 overhead 的精确值。

GPU 用自然对齐的 32-bit system-scope release store 发布 marker/counter；CPU acquire load。只用 load/store，不使用 host-mapped RMW atomics。GPU 先写 sample、system fence 后 publish sample count；CPU 只读已发布元素；每 trial 完全 drain 后才能 reset。heartbeat 仅 thread0/block0 每约 2 μs 记录一次，最多 65536 个，不覆盖旧 samples；overflow 单独记录。

CPU 独立 observer thread 在 host launch 和阻塞 RM call 期间继续 poll，记录 first observed INT marker；迟到读取的多条 BG sample 共享同一个 CPU observation time，保留该事实。请在目标 host 为 observer/INT/BG CPU 线程安排充足 cores，并结合记录的最大 polling gap 判断调度噪声；代码不自动改变主机 affinity/policy。

## 字段与能支持的主张

| 字段 | 含义 / 限制 |
|---|---|
| T_cpu_trigger | 本 trial CPU interaction，紧邻 INT launch 前 |
| T_int_submit_end | host launch + event-record 返回；不是 GPU runnable acknowledgment |
| T_rm_call_begin/end | 实际 ioctl 周围 CPU 时间，含同步 transport/firmware wait；B/D 在 BG process |
| rm_syscall_result / rm_errno / rm_status | 三者分开，syscall failure 不得因 struct.status=0 被视作成功 |
| T_int_gpu_start_observed | observer 看见主计算节点 block0/thread0 start marker；不是第一条 warp instruction 的硬件探针 |
| T_int_gpu_done_observed | observer 看见 stream 后置 completion marker；覆盖全部计算 nodes |
| T_bg_preempted_observed / T_bg_resumed_observed | **空值**；本版本没有全 TSG completion notifier / reliable context-switch trace |
| T_int_gpu_start_ns / done_ns | GPU globaltimer 主节点 start 和后置 marker；与 CPU 时间分开 |
| T_bg_gap_begin/end_gpu_proxy | BG sentinel 两个 samples 夹住 INT start 的区间；不代表所有 SM 停止或控制因果 |
| T_bg_activity_after_int_observed | GPU timestamp 晚于 INT done 的第一条已记录 BG sample 的 CPU observation；只证明之后有 BG activity |
| T_reenable_begin/end | D 的显式 enable control，其他模式为空 |
| preliminary_rm_begin/end | split D 的 onlyScheduling=true 那次 control |
| bg_correct / int_correct | 所有线程、所有 graph 节点的整数 output 与单独执行 reference 完全相等 |
| observer_max_poll_gap_ns | CPU observer 采样间隔噪声上界线索，包含 trial 收尾期间 |

`interaction→INT observed start`、RM syscall duration、INT GPU marker duration 可直接汇总 p50/p95/p99/max。同步 B/C/D 的 RM return 根据接口契约给 preemption/context-switch completion **上界事件**，但不能从 syscall wall time 剥离精确 HW context-save latency。BG sentinel gap 也包含等候 INT 和调度间隔，不能命名为 context-switch cost。

精确的 preemption completion latency、context switch latency、BG resume latency 目前标 **unmeasured**。需要目标 host 上 Nsight Systems/CUPTI/FECS context-switch timeline 或可信 kernel/GSP notifier 才能填充；普通 Nsight kernel 的 start/end 范围本身也可能跨越 preemption，不能单凭一个长条分割硬件 resident 区间。未自动调用 nsys，也未伪造它的时间线。

## 扰动和对照

`*_identity.txt` 包含相同迭代数在单独运行时的无 heartbeat / 有 heartbeat kernel duration。另用 `--heartbeat-ns 0` 做对照（仍有 start/done markers）；校准纯 host clock/poll 成本需要看 mapped ping RTT 和 observer gaps。不要简单减一个均值常数得到微秒级精确数值。

`bg_ctas.csv` 保存主节点各 CTA start/end；跨越抢占的 CTA wall duration 包含暂停，不能当成纯计算时间。不同 `--cta-waves` 使相同目标 kernel 时长由更多短 CTA 组成，用于检验 CTA vs CILP 假设。单 sentinel 在 short-CTA 配置可能提前结束，不能覆盖整个 grid，因此该配置更应依赖 CTA 全部区间和外部 context-switch trace。

每 trial 启动 BG 后，在其主节点 start 后约 1–5 ms 随机相位触发 INT，避免固定 timeslice phase；INT 必须先 host enqueue，再发 RM control。原 requested timeslice 和 mode GET 的返回值及错误单独保存。未观测到精确 hardware quantum。

## 样本分类

* `observed`：控制未报错、CPU marker 有效，且 INT start 不晚于 BG done；只是有效重叠样本。
* `int_before_rm_issue`：INT 在请求前已被 observer 看见；不能归功于本次主动触发。
* `int_after_bg_done`：没有观测到中途让路；保留为调度结果，不篡改为 active success。
* `rm_error`、`invalid_observation`、`state_mismatch`：不进入成功分位数。

“BG 马上被重选”只能由 heartbeat/timeline 相对 INT interval 的活动推断；不将其自动标为 bug。输出 correctness 不足以证明完整寄存器保存实现，结合同一长 CTA 跨越 INT、无重启/丢失、context 持续有效才能增强证据。

async PREEMPT 没有在此次 API 中找到 completion token。本 harness 完成并验证每次 BG 工作后再试下一次，避免对同一个未完成 target 连发 async 请求；如果返回失败则停止，不重试制造虚假的低延迟。
