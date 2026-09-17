# 阶段四：多 channel TSG 绑定与首次 GET_INFO

**运行结论：一次项目追加的 group GET_INFO 成功。** 项目 active controls=0，benchmark trials=0；没有执行 PREEMPT、MAKE_REALTIME、RESTART、SET_TIMESLICE、DISABLE/ENABLE、Graph 或多队列实验。

研究仓库起始 HEAD：`4f4cd179965d08f547d36ddd2d0b68ce3ec7ae71`，起始工作区干净，见 [initial_state](../results/phase4/initial_state.json)。本次在该基线上修改，未回退、安装 KMD、修改权限/firmware、reset 或 push。运行二进制与实际源码文件的 SHA-256、HEAD、工作区状态和完整 argv 在 [execution.json](../results/phase4/group_info595/execution.json)；不能把原始基线 commit 本身当成本次新增代码。

## 源码依据与实现范围

NVIDIA **595.58.03 / `db0c4e65c8e34c678d745ddb1317f53f90d1072b`**：

- [`ctrl/ctrla06c.h:215–235`](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/db0c4e65c8e34c678d745ddb1317f53f90d1072b/src/common/sdk/nvidia/inc/ctrl/ctrla06c.h#L215)：GET_INFO=`0xa06c0106`，参数只有输出 `NvU32 tsgID`；所选 SDK 的 sizeof/alignof 均为 4。
- [`kernel_channel_group_api.c:kchangrpapiCtrlCmdGetInfo_IMPL:1360–1374`](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/db0c4e65c8e34c678d745ddb1317f53f90d1072b/src/nvidia/src/kernel/gpu/fifo/kernel_channel_group_api.c#L1360)：检查 group 存在后返回 `pKernelChannelGroup->grpID`。没有要求选择 compute channel，没有抢占操作。因此本次 GET_INFO 成功不能证明 GSP preemption 路径。

本轮修改（研究仓库上述基线后的工作区代码）：

| 文件 / 函数或类型 | 修改 |
|---|---|
| `object_registry.{h,cpp}`：`GroupObject/GroupMember/GroupBinding`，`discover_group/valid_group/touch_topology` | 独立完整 group 快照；PID/registry/profile、generation、祖先与单 compute TSG 检查；成员/compute 候选 revision 捕获暂态变化 |
| `rm_control.{h,cpp}`：`GroupIdentity`，`inspect_owned_tsg/get_group_info` | 不发 control 的发现；原 allocating FD dup、当前 scope/快照检查；与旧 `Identity/RmControl` 不可转换 |
| `group_query_internal.{h,cpp}`：`GroupInfoOnce::query` | 内部窄 GET_INFO backend 和 mock seam；参数初始化、锁内验证、一次 syscall、原值 journaling；无任意 cmd/target 入参 |
| `driver_profile.{h,cpp}` | 独立 GroupInfo 阶段、group/channel 证据、readonly/active 计数与 UUID scope |
| `int_worker.cu:probe_group_info`、`options.h` | `--probe-rm-group-info`；短 workload、一次查询、清理前后快照；失败先存证据，拒绝混用 probe/active 参数 |
| `control_events.{h,cpp}` | 原 schema 2 binary 布局保持；group 目标和返回 ID 写入 journal，JSON 的无选中 channel 为 null |
| `group_binding_test.cpp`、`CMakeLists.txt` | 新的 CPU-only fixture，分别链接 pinned 550/595 headers |

旧 `discover()` 的唯一 compute-channel 条件未删除。新的 group 类型没有 `compute_channel=0/first/last` 占位；全部成员只属于该捕获 group，不代表整个 context。原 `binding_observed/readonly_verified` 仍专指严格 channel 路径。状态/生存期细节集中在 [object_binding](object_binding.md)。

## 离线验证

独立 550/595 构建均成功，各 **7/7 CTest**：

- [550 build](../results/phase4/validation/final_build550.log)、[550 CTest](../results/phase4/validation/final_ctest550.log)
- [595 build](../results/phase4/validation/final_build595.log)、[595 CTest](../results/phase4/validation/final_ctest595.log)

每条构建/测试 argv 和返回码在相邻 `.json` 文件中。原 ABI/EBADF/NV_STATUS、生命周期、mode、profile/transport、Python 分析/runner/schema 测试保留。新增 fixture 覆盖 1/2/3/8/17 channels、copy-only 排除、两个 compute TSG、无 compute child、错误 ancestry、释放/复用/rebind、暂态成员/compute 变化、无关 FREE、不完整/溢出/未知 ABI、历史 registry/PID/fork、原 FD 校验、profile/阶段拒绝、一次正确目标 GET_INFO、失败 syscall/RM 原值、ID=0、无 channel/active 解锁。observe stage mock 的项目 controls=0。fixtures 在临时目录，无 synthetic GPU raw.csv。

双 profile 的 help、拒绝 `--test-host-confirmed`、拒绝混用 readonly/group-info 共 [6 个 CLI 检查](../results/phase4/validation/cli_checks.json) 符合预期，仅解析参数，没有 CUDA/RM 调用。[证据核对](../results/phase4/validation/evidence_checks.json) 确认实际 probe 的源码/二进制哈希与当前实现一致，JSON 可解析，benchmark raw.csv 数量为 0。

## 本次真实过程与结果

[新 preflight](../results/phase4/preflight/preflight.json)：设备可 open，`cuInit=0`，`cuDeviceGetCount` **返回码 0 / count 1**（设置了单 UUID 可见性），独立有限 kernel 的 CPU reference PASS。实际 A100-PCIE-40GB / CC 8.0，KMD 与实际 libcuda 595.58.03，runtime/Toolkit 12.8。查询前再次核对 [GPU 与 compute-process 列表](../results/phase4/group_info595/device_checks.json)，没有观察到该 GPU 上的其他 compute process；这不是主动测试主机授权或所有隔离条件的证明。

实际命令从仓库根目录执行，使用全新输出目录：

```bash
CUDA_VISIBLE_DEVICES=GPU-99e4e85f-1945-866c-9e00-130b51df7908 \
python3 active-preempt/scripts/preflight.py \
  --binary active-preempt/build-595/preflight_cuda --output results/phase4/preflight

CUDA_VISIBLE_DEVICES=GPU-99e4e85f-1945-866c-9e00-130b51df7908 \
LD_PRELOAD="$PWD/active-preempt/build-595/librm_control.so" \
  active-preempt/build-595/int_worker --probe-rm-group-info \
  --run-dir results/phase4/group_info595
```

probe PID **2248248**，exit **0**，无 timeout/强杀。短 workload 为固定 256 iterations、1 block × 256 threads；使用现有有限预热及输出 reference 比较，没有 80 ms 校准、BG/INT benchmark 或新 channel 选择推断。

| 查询前对象事实 | 本次快照 |
|---|---|
| 当前捕获节点 | 116；registry 未检测到 incomplete |
| compute TSG 候选 | **1**；8 个 C56F channel，各有 C6C0 compute child |
| copy-only TSG | 3；各 4 个已识别 channel，没有已识别 compute child |
| group engine | 捕获值 1；channel engine 未显式捕获，保持 unknown |
| client / client generation | 3252373130 / 3 |
| device / subdevice | 1543503874 / 1543503875 |
| group handle / generation | 1543503948 / 76 |
| allocating FD → retained dup | 10 → 11，同一原 allocating open-file |

完整 token、class、parent/generation 和成员关系在 [group_identity.json](../results/phase4/group_info595/group_identity.json)，全图在 [before_cleanup_object_graph.json](../results/phase4/group_info595/before_cleanup_object_graph.json)，仅从该实测图派生的计数见 [object_graph_summary.json](../results/phase4/group_info595/object_graph_summary.json)。这些值只描述已结束的进程，不能在新进程使用；即使数值与历史相同，也不代表同一对象。

**项目查询结果**见 [get_info.json](../results/phase4/group_info595/get_info.json) 和 [control journal](../results/phase4/group_info595/probe_control_events.jsonl)：

| 字段 | 实际值 |
|---|---|
| 追加 GET_INFO 尝试 | **1**，operation_seq=1 |
| hClient / hObject | 3252373130 / **1543503948（group）** |
| ioctl / errno / NV_STATUS | **0 / 0 / 0（NV_OK）** |
| hardware TSG ID | **6**，有效查询输出；不是 GPU UUID |
| 项目 readonly / active | **1 / 0** |
| 观察到的 libcuda 自有 RM controls | 清理前 720，清理后 724；与项目计数分开 |
| channel binding verified / active authorized / active result measured | **false / false / false** |

原始 mmap journal 已无损压缩保存为 `.bin.gz`，roundtrip 与 event_dump 恢复 JSON 均核对一致，见 [archive record](../results/phase4/group_info595/journal_archive.json)；没有过滤、补写 control 事件。

[cleanup](../results/phase4/group_info595/probe_cuda_cleanup.log) 的 cudaFree、两个 event destroy、stream destroy、host unregister、cuCtxDestroy 全部 code=0。`group_binding_valid` 清理前 true，清理后 false；`group_get_info_verified` 保留 true 作为历史证据。普通 context 结束清理不是 BG 抢占后的恢复测试。

## 结论与下一步边界

**运行证据支持：** 当前环境中，一个新进程能够捕获具有多个 compute channel 的唯一 TSG，在保留的原 FD 上用该 group handle 追加一次合法接受的 GET_INFO，并取得 hardware TSG ID。当前 group 查询访问路径已打通。

**仍不能声称：** 完整捕获所有 hidden/direct syscall 或 CUDA context 资源；知道具体 arithmetic launch 的 channel；GSP 抢占路径/PREEMPT 权限通过；INT 提前执行；BG 正确恢复；任意抢占粒度或性能优势。

已具备讨论 **B 的 group-target 绑定前提**的依据。后续需要单独审查/实现受限的 GroupIdentity PREEMPT(wait=true) 路径、在每次运行中重新绑定/GET_INFO、明确主动测试授权和可观察的恢复流程。当前没有此 group PREEMPT 接口，不能直接把现有旧 channel 路径的 benchmark 命令当作可用。C 的 channel/runlist 选择仍是独立问题。本轮到这里结束，不追加第二次查询或任何主动实验，无新增 KMD 的运行证据要求。
