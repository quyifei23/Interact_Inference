# 阶段二测量契约（CSV schema_version=2）

## 阶段五增量：一次 group PREEMPT

`group-preempt-wait` 与 `none` 共用有限单 kernel、相同固定 iterations/grid/block/shared-memory/instrumentation 和一次 owner CPU 消息；独立进程重新初始化，B 另外承担快照校验和真实 syscall，不能称 CPU 路径完全相同。runner 的 `--paired-none` 核对相同 GPU 和二进制，原始配置/二进制指纹保存在 invocation/configuration 中。单次 smoke summary 只报告原始值，不生成 trial 分位数或 winner。

schema 2 在尾部追加 `T_bg_done_observed`（独立 observer 首次看到 BG completion）、`bg_done_at_owner_check`（owner 执行 no-op/PREEMPT 前看到的 done，未知为空）、`setup_status`。旧列含义不变。BG 已完成的设置事实不会丢样本；新 B 在 controller 或 owner 已看到 done 时不发 PREEMPT、不自动重试。没有看到 done **不证明 BG 此刻驻留**，`control_target_running` 仍 unknown。

新增 Prepare 命令在 observer 启动前 reset BG telemetry，避免并发清零；observer 在 BG launch 前已经运行，INT 同步通知 BG 发 RM control 时继续独立观测。GET_INFO 在最终初始化阶段，各 owner 一次；本轮 BG PREEMPT 最多一次，bWait/bManualTimeout=true、timeout=1 s。GET_INFO 和 PREEMPT 的 call begin/end 都只表示各自 control 的 host wall time，不是硬件切换事件。

主 trial 输出和 telemetry 持久化、observer join、BG drain 后，才可在同一 BG context/stream 上执行 256 iterations 的短复用检查；检查 reference 在初始化/绑定前已准备。复用结果独立保存，不覆盖 raw/CTA/heartbeat，不是第二次抢占或精确 resume 观测。普通失败先保存 journal，再进行最长 2 s 的 event drain 检查；driver API 本身卡住仍靠 runner 有界终止本实验进程组，记录恢复未确认，不 reset。`CONFIGURATION_RESTORED` 仅表示本模式没有遗留调度配置，不等于恢复/正确性证据。

本次 [实机结果](../../docs/phase5_group_preempt.md) 中 RM 返回 NV_OK、INT/BG 输出一致且正常排空；INT entry 在 RM call 结束前已被 CPU 看到，但相对 call begin 仍为 ordering_ambiguous。没有将校准最小 RTT 当硬保证；精确 BG preempt completion、context-save duration、resume latency 仍 unmeasured。最终输出一致不能排除确定性重放。

旧版（仓库 `9d578f24fdaa102ad94e6db2093ec14386416138`）的 `T_int_gpu_start_observed` / `T_int_gpu_start_ns` 指主节点 K1。新版使用新字段，不能把旧字段默认为 Graph 首次执行；统计器明确拒绝 schema 1、混合 schema 和混合 diagnostic/performance。

## 入口、时间域和发布顺序

Graph 为 K0 → K1(main) → K2 → done marker。`graph_entry` 在 **K0 真实 arithmetic kernel 内**由 block0/thread0 发布；`main_entry` 在 K1 内发布；普通单 kernel 的两个 marker 对应同一次入口（CPU 读到时间可能不同）。没有另加前置 marker kernel。`graph_done` 仍是全部计算节点后的独立 completion marker，其额外调度成本保留在测量中。

| 字段 | 含义 |
|---|---|
| T_cpu_trigger | CPU interaction，CLOCK_MONOTONIC_RAW ns |
| T_int_submit_begin/end | host launch 调用区间；end 不证明 GPU 已确认 runnable |
| T_ipc_send/received/ack | controller 发消息、BG owner 收到、controller 看到 ack；B/D 的往返包含 control 工作 |
| T_rm_call_begin/end | 真实 ioctl 前后；不包含之前的 IPC/身份检查，不是硬件完成时间 |
| T_int_graph_entry_observed / main_entry_observed / graph_done_observed | 独立 CPU observer 首次看到三个 publication 的 host 时间 |
| T_int_graph_entry_gpu_ns / main_entry_gpu_ns / graph_done_gpu_ns | GPU `%globaltimer` 时间；独立字段，不直接与 CPU 时间相减 |
| int_entry_node / int_main_node / launch_id | marker 所属节点和本进程 launch/trial；不是 CUPTI graph/node ID |
| T_bg_main_observed / main_gpu_ns / done_gpu_ns | BG 长主节点与全部完成；int-only 全部为空 |
| T_bg_preempted_observed / T_bg_resumed_observed | **空值**；精确 preempt completion、context-save duration、resume latency 均 **unmeasured** |

block0/thread0 是明确的入口代理，不保证是 GPU 第一条 warp 指令。BG main marker 用来安排 interaction，不替代 INT graph entry。长 kernel timeline 可以跨越抢占；单 CTA heartbeat 停顿不证明整个 TSG 暂停。

CPU 时间在进程间共享 CLOCK_MONOTONIC_RAW 域；两个 worker 核实同 GPU UUID 后，GPU marker 只作设备 globaltimer 域的 lifetime 比较，依赖目标单 GPU / 非虚拟化环境前提。跨 CUPTI、CPU、GPU 时钟未经模型不能相减。`gpu_overlap=lifetime_overlap` 是区间重叠，不是同时驻留或切换事件。

## mapped memory 的内存模型

`protocol.h` 保证 page-aligned Telemetry、自然对齐 32-bit flags、64-bit payload，并静态检查 CPU 32-bit atomic load/store lock-free。GPU 使用 `st.release.sys.global.u32`（之前 system fence）；CPU 用 GCC acquire load。CPU→GPU ping 用 release store / `ld.acquire.sys.global.u32`。这是 Linux x86-64 + CUDA 的跨设备发布协议，不以 `volatile` 自身保证正确性。

payload 先写，再发布 flag/count；CPU 只读已发布、不会再覆盖的槽位。每个字段有单一 GPU writer，heartbeat buffer 满后停止追加，不循环覆盖。reset 在 CUDA event 已 drain、输出检查完成且 observer join 后进行。CPU-only command/ack 也用 release/acquire，不能仅凭 command 普通写入同步两个进程。

此路径没有对 mapped host memory 做 atomic RMW。CUDA 对自然对齐单次 load/store 的保持要求，与 CPU/GPU 共同 RMW 的支持是不同条件；不能因为 flags 可读写就假设 PCIe 原子加法安全。诊断 progress 的 atomicAdd **只作用于 cudaMalloc 的 device memory**，由 GPU 使用，CPU 在 event 完成后复制读取。[CUDA 12.8 mapped-memory 约定](https://docs.nvidia.com/cuda/archive/12.8.0/cuda-c-programming-guide/index.html#mapped-memory)

该协议的真实传播/平台支持仍需目标机验证。前后各 1000 次 ping 校准记录 send、GPU timestamp、observed、RTT；没有把最小 RTT 当全程硬误差保证，也不从测量中直接减掉一个 overhead 常数。

## 五个独立维度

- `application_valid`：有顺序有效的 host entry/done 观测；即使 BG 或恢复阶段使 trial 不完整，也保留已测 INT 区间。不因 RM 失败、before/ambiguous 或 correctness failure 静默删除时间数据。`trial_state=complete` 才表示整个 trial 流程结束。
- `control_status`：NOT_ISSUED、CONTROL_RESULT_UNAVAILABLE、拒绝/错误类别，或 CONTROL_ACCEPTED_EFFECT_UNVERIFIED。后者要求 attempted + ioctl=0 + NV_STATUS=0。
- `gpu_overlap`：lifetime_overlap / no_lifetime_overlap / unknown / not_applicable。
- `ordering_relative_to_rm`：before_rm / ordering_ambiguous / not_applicable；离线显式启用模型后另可出现 after_rm_under_calibration_model。
- `correctness_status`：PASS / CORRECTNESS_FAILURE / unknown，与能否确认中途抢占相互独立。

观察到 `entry_observed < rm_begin` 足以说明 CPU 在请求前已经看到 Graph 首节点。反向条件不能证明 GPU 晚于请求，默认 **ambiguous**。因此 K0 在 RM 前开始、K1 在 RM 后发布 main marker 的样本不能算作 RM 导致整个 Graph 开始。

`bg_done_before_interaction` 与 `bg_done_before_control_observed` 分别记录 interaction 前和触发消息前 CPU 是否已看到 BG done。后者的 false 不能保证实际 syscall 时 BG 仍未完成。它们不代替 `control_target_running=unknown`：B 的目标是 BG，C 的目标是 INT channel，当前后端不能证明任一 target 正驻留在 engine。

可选 `summarize.py --calibration-margin-ns N` 明确假设：offset 在前后最小 RTT brackets 间线性变化，另加用户选择的 N ns margin，不允许外推。只有映射后的 entry 下界晚于 RM begin 才标 after_rm_under_calibration_model。该模型仍有未证明的漂移/误差假设；一次最小 RTT 不赋予全时段保证。默认不使用模型；after-RM 仍不等于 causal preemption。

统计同时输出全部有效应用样本、结果正确子集、时间条件满足子集，以及各维度数量。默认模型下时间条件子集可为 0，这不是把实验删掉。RM 错误、incomplete trial、缺失观测、状态错误与晚到/提前开始样本均保留。paired 比较是 **M3 vs M1** 和 **M5 vs M4**，不能把 M5 vs M1 全部收益归因于 restart。

## 控制日志、超时和恢复

每 worker 在测量前预分配 `*_control_events.bin` 的固定 mmap 槽位；每次 control 发出前记录 input identity/generation、参数原始字节、操作序号，返回后立即填 syscall/errno/NV_STATUS 并发布 RETURNED。热路径不写 JSON 或同步磁盘；正常 trial 后导出 `*_control_events.jsonl`，异常先导出再做可能阻塞的恢复。达到容量会拒绝后续 control，不丢事件继续测试。

进程中途退出时 binary 仍可由 **同版本** `event_dump` 恢复；IN_FLIGHT 的 end/status 写 null。未收到 owner ack 的 trial 不假装 control 没有发生，而是 CONTROL_RESULT_UNAVAILABLE；已知返回即使 INT timeout 也留在独立 journal。JSON 导出失败报 CONTROL_LOG_EXPORT_FAILED，保留 binary，不阻止 owner 恢复。此机制针对进程退出，不保证掉电后数据落盘。SIGKILL/内核挂起无法保证用户清理函数运行，不能把未确认恢复写成成功。

controller/BG 都处理 SIGTERM；BG 设置 parent-death signal。runner 创建独立 process group，先 SIGTERM 给 owner 清理机会，再有界等待，必要时 SIGKILL 整组并标 RECOVERY_UNCONFIRMED，停止后续组。正常路径 D enable、RT demote、timeslice restore 在 owner 完成；dirty flags 在修改前设置，返回错误也保留恢复责任。配置 restore 接受与 GPU 状态正确是不同检查。context destruction 只在进程清理使用，不作为正常 preemption/resume 操作。

## 工作量和正确性

arithmetic 只有固定、记录的 iterations；暂停时间不算已完成计算，不按墙钟提前退出。每 CTA 的 register/shared-memory 递推和所有 node 的输出与 solo reference bit-exact 比较。host deadline 是错误/恢复触发器，不能保证一个被永久 deschedule 的 TSG 自行结束。

W2 用低 waves、较长每 CTA 工作；W1 用更多 waves、较短每 CTA 工作。`*_identity.txt` 记录 iterations、grid/block、registers、static/dynamic shared memory、CUDA occupancy **估计**、solo / instrumented 时长。runner 在小矩阵内冻结已校准 iterations；跨 invocation 比较必须显式复用数值，不能每组重新校准后宣称工作完全相同。`bg_ctas.csv` 的 CTA wall intervals 包含可能的暂停。

`configuration.json` 保存规范 mode、graph/run kind、force/bypass、waves、heartbeat、timeslice 请求、实际 iterations/grid、GPU UUID 与 profile。1 → 10 的 smoke 准入逐项核对配置，并继承相同的固定 iterations；更大样本要求显式指定且与 smoke 一致。更换 GPU、驱动或编译配置时应重新 smoke；目前配置核对不能替代操作者核实硬件/二进制身份。

`--diagnostic-progress` 是独立诊断组：每 CTA 每 256 次递推之后，在独立 device memory 记录计数和序号和；Graph 每节点各有计数，reset 只在前一 launch 完成后。无补偿的遗漏/重复执行会改变计数，部分同数量替换由序号和发现。它不是无碰撞的完整执行证明，计数/和巧合抵消、连同全局内存一起回滚等未覆盖；确定性输出本身仍不能排除重算。诊断 atomics/内存流量会扰动时延，不混入 performance 分布。

## 观测扩展与范围

本轮仅探测 CUPTI 可用性，未实现强制 trace 后端。将来的 backend 应另存 process/context/kernel/graph-node correlation、timestamp 域、丢事件数，并核查 context-switch START/END 的实际语义；官方类型描述的是 switch operation 起止，不直接定义 BG-out/INT-in 或“所有 execution state 已写回”。[官方 activity 类型](https://docs.nvidia.com/cupti/api/structCUpti__ActivityComputeEngineCtxSwitch.html)

尚未覆盖：prequeue 1/4/16、连续 interaction、旧 INT 的 demote/promote 复用、latest-request-wins、取消/回滚与 buffer 回收。当前只编译了单次 launch 的 Graph preempt/resume 路径，没有实机执行；旧队列不会因 PREEMPT 被删除。旧工作可能访问的内存在排空或可靠回收以前不能复用。
