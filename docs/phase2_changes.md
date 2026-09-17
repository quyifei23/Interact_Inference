# 阶段二实现变更：对象、测量与证据保留

基线为 Interact_Inference **`9d578f24fdaa102ad94e6db2093ec14386416138`**；初始工作区干净，[状态记录](../results/phase2/initial_state.json)。本轮沿用已有 prototype/build 与研究文档，保留此前新增的 preflight 工作；没有修改两个 upstream submodule、安装 KMD、放宽 SecInfo、授予 capability 或推送远端。以下“旧版”均引用该基线的实际源码；新实现对应包含本文的本地工作版本。**GPU trials=0**。

## 问题、修改和测试

| 修改前的具体问题（源码证据） | 修改文件 / 关键函数 | 本轮修复与验证 |
|---|---|---|
| `rm_control.cpp:remember` 追加同 handle 新对象，FREE 又扫描历史 `!live` parent；旧 H tombstone 可使新 H 的 C2 在无关 FREE 时失效 | `object_registry.{h,cpp}:allocate/erase_tree/free/valid`；`rm_control.cpp:remember/observe` | 当前 `(client,handle,generation)` 表与历史日志分离，父关系包含 generation。H1/C1 → free → H2/C2 → free X、client 隔离/复用、递归释放均有纯 CPU 成功路径测试 |
| `discover_owned_compute_group` 对同 TSG 下多个 compute channel 反复覆盖 `compute_channel`，最终选最后一个 | `ObjectRegistry::discover/valid` | 多 compute channel、多 TSG、多 subdevice、缺失 compute child 或祖先都拒绝。缓存身份遇新 channel/subdevice、free/rebind 或 generation 变化即失效 |
| 未知 allocation ioctl 布局、serialized alloc、捕获异常可被忽略；构造后的 control 不再验证缓存 | `rm_control.cpp:observe/ioctl/issue` | 捕获不完整显式 poison；支持范围仅 pinned NVOS21 / plain NVOS64。观察 syscall 与当前表更新、自有 control 验证共用锁；每次发出前验证 token。未知 ABI 不产生猜测身份；hidden syscall 的完整性仍待实机证实 |
| 550.120 门槛在 CUDA probe 前，混淆 CUDA 不可用与私有 ABI 未适配 | `preflight.cu`、`preflight.py`、`int_worker.cu:probe_rm/main`、`options.h:parse` | CUDA 独立 executable；`--probe` alias CUDA。RM identity/readonly 最小 1 CTA/256 iterations，失败仍 inventory；GET_INFO 与可选 getter 分别记录。CUDA=100，RM=ABI_UNVERIFIED 的独立结果已保存 |
| 缺少 M0 与仅 realtime 配置的 M4，不能区分 restart 的增量作用 | `mode_plan.h`、`options.h`、`int_worker.cu`、`bg_worker.cu`、`run_matrix.py` | 新增 int-only/realtime-only，规范 realtime-restart 并保留 alias。baseline 不要求 RM identity；主动模式缺 identity 拒绝。mode tests 保证 M4 不 restart、M5 必须已成功 promote |
| Graph 主 start marker 在 K1，可能把 K0 已在 RM 前运行误判为整个 Graph 被此次请求启动 | `worker_common.cuh:arithmetic/enqueue`、`protocol.h:Telemetry`、`TrialRecord`、`summarize.py:ordering` | 第一个真实 K0 内发布 entry，K1 保留 main，完成单列；schema 2 不复用旧语义。实际 C++ writer + Python parser 集成测试 K0-before / K1-after，判 before_rm |
| observer 在 RM 后看到 marker 不足以证明 GPU 在 RM 后启动 | `trial_record.h:write`、`summarize.py:CalibrationModel/ordering/summarize` | application/control/overlap/ordering/correctness 分维度；默认 ambiguous。显式 affine clock 模型只给条件性 after，不外推、不保证全程硬误差。全部有效应用区间都统计，错误与 ambiguous 不自动筛除 |
| 只在完整 trial 最后写 raw；control 返回后发生 INT/BG timeout 可能丢最早事实 | `control_events.{h,cpp}`、`rm_control.cpp:issue`、`event_dump.cpp`、`TrialRecord` | preallocated mmap 槽，返回立即发布；JSON 单独导出，在途字段 null。独立 binary 可在被杀后恢复；incomplete trial 保留已知 INT 区间与控制结果。测试 accepted→timeout、failed→restore、enable failed、in-flight 和 `/dev/full` |
| 只杀 controller 可能留下 BG，错误路径易丢配置恢复责任 | `recovery.h`、两 worker、`run_matrix.py:run_process_group` | 修改前置 dirty flags；owner enable/demote/restore；信号、parent death、整组 TERM→有界 KILL、恢复未确认后停止后续组。CPU 子进程树超时测试；真实 GPU 恢复未验证 |
| 校准重做与配置复用可能混入不同工作；固定时间上限会把暂停时间算入执行进度 | `worker_common.cuh:arithmetic/GpuWorker`、`run_matrix.py:validate_smoke`、`configuration.json` | arithmetic 固定且有界 iterations，不取消/提前 return。记录资源、grid、solo；smoke 核对完整已记录调度/workload 参数，后续复用 iterations。变 waves/heartbeat/timeslice/force/work量时旧 smoke 拒绝 |
| 确定性 checksum 不能单独排除从头重算 | `worker_common.cuh:arithmetic/correct` | 保留 bit-exact reference；新增可选 device-memory 每 CTA count + sequence-sum，Graph 节点分开，独立 diagnostic run。能发现未被抵消的遗漏/重复；不是无碰撞执行证明，实机未运行 |

完整对象约束与源码关系见 [object_binding](object_binding.md)，时间域/原子范围/局限见 [measurement](../active-preempt/docs/measurement.md)。不再另建重复的 measurement 或 findings 文档。

## 保留的正确设计

仍然捕获每个 owner 自己的 allocation/BIND/FREE，沿 compute → channel → TSG 寻找；dup 原 nvidiactl FD，不另开 FD 借用 handle；GET_INFO 检查与 hardware TSG ID 读取；BG/INT 各自发控制。所有 control 参数初始化，ioctl/errno/NV_STATUS 分开，未更新 status 不当 NV_OK。每 trial 至多一次 async PREEMPT，排空后才能重发。CPU observer、前后 mapped-memory 校准、有限 arithmetic/输出 reference、单次 Graph 路径均保留并修正计时。

**新增代码不等于运行成功。** ownership 的最终裁决仍在 stock RM；hook 不可能仅靠自报完整来证明没有 hidden/direct syscall。源码 metadata 权限、GET_INFO 接受、修改 control 接受、engine 驻留、因果延迟收益、恢复结果是不同证据。

## 版本和已有研究结论的变化

NVIDIA 550.120 commit **`5e52edb2034de7db4d8ae368dbc7c26b416bfa16`** 保持 pinned。对宿主 595.58.03 独立检出 commit **`db0c4e65c8e34c678d745ddb1317f53f90d1072b`**，`audit_profiles.py` 实际编译比较 13 个结构体布局/字段和 10 个 control ID；本轮所用布局/ID 一致，但 NVOC flags、权限回调与 RPC 实现有变化。[逐项证据](runtime_readiness.md)

关键来源分别是两个 tag 的 `src/nvidia/generated/g_kernel_channel_group_api_nvoc.c` / `g_kernel_channel_nvoc.c`，`rmapi/client.c:rmclientValidate_IMPL`、`rmapi/client_resource.c:cliresAccessCallback_IMPL`、`rmapi/resource.c:rmresControl_Prologue_IMPL`，以及 595 的 `vgpu/rpc.c:rpcRmApiControl_GSP`。对照结果没有自动开启 595 adapter；实际 allocation/control transport 与权限/firmware 前提未核验，CUDA 本身也尚不可访问。

B/C 仍优先验证：**M3 vs M1** 和 **M5 vs M4**，timeslice 是没有 reservation 的配置 baseline。D 延后，未新增 driver primitive。公开源码没有证明 PREEMPT 一定使 INT next，也没有证明 C 在本机可用。没有对 GPreempt 作者为何不调用 PREEMPT 添加未经证实的解释；原始 GPreempt commit **`249ee3e56d01068451e172e691853b4d055c0dec`** 的 dead wrappers / 公开历史结论仍见 [source_archaeology](source_archaeology.md)。性能与作者动机方面的候选原因仍是推测。

## 已验证、未验证和交付

最终 CUDA/C++ 构建通过，**CTest 5/5**；Python analysis **7/7**、runner **7/7**（也包含在 CTest 中）。原 ABI/EBADF/空身份测试保留。实际 CSV/journal writer 集成测试采用临时 synthetic fixtures，不是 GPU data。精确命令、退出状态与日志见 [阶段二结果](../results/phase2/summary.md)。

实机入口独立返回 cuInit **100**、枚举 **3**；最小 kernel 未运行。identity/readonly 因 595 profile 未适配返回 **77 / ABI_UNVERIFIED**。**真实 RM GET/修改 controls=0，GPU trials=0，恢复正确性实机次数=0**。不存在性能 winner，也未证明低延迟抢占。

只实现/编译单次 kernel 或同拓扑 Graph 的测试路径。prequeue 1/4/16、连续 interaction、旧 INT 变 BG、latest-request-wins、永久 cancellation / reclamation 未覆盖。精确 BG preempt completion、context-save duration、resume latency 仍 unmeasured。下一步最小实机步骤见 [README](../active-preempt/README.md)，不能在当前 session 删除门槛强跑。
