# RM 对象绑定与生命周期

## 阶段五：绑定到查询凭据的同步 group PREEMPT（当前）

研究基线 `4eb805569ed754bbdf7d65c81b181a75ff545480` 后的本地修改：`preempt_group_wait(const GroupIdentity&, timeout_us)` 不要求单 compute channel，不选择或伪造子 channel。旧 `Identity/RmControl` 与唯一 channel guard 保留。实测两进程各自捕获 8 个 compute channel 的唯一 TSG，各自 GET_INFO 成功；BG 唯一一次同步 PREEMPT 返回 ioctl=0 / errno=0 / NV_OK。完整事实与边界见 [phase5_group_preempt](phase5_group_preempt.md)。

`GroupInfoOnce::query/verified_for` 私有保存成功查询所对应的完整 `GroupBinding`、UUID、可见设备 scope 和 optional hardware TSG ID。`GroupPreemptOnce::preempt` 逐次核对该凭据与当前 PID/registry/profile、原 retained FD、全部祖先/member generation 和唯一 compute 候选。旧对象查询成功的全局布尔值不能验证复用的 handle、新 registry 或其他绑定。ID=0 仍合法，ID 从不替代 hObject。

`GroupActive` 与只读 `GroupInfo` 分离；需显式授权，且授权 UUID scope 与当前 scope 相同。`GroupOwner::Background` 是唯一可发请求的角色，INT owner 只查询自己的 group。每个 owner 最多一次 PREEMPT、仅 trial 0、固定 wait=true；完整初始化参数并限制 timeout 到当前 SDK 上限。无通用 command/target 入口，无 channel-specific getter，也不改变现有 stock RM ownership/rights。

初始化完成所有预热/分配后才查询；trial 前检查双方快照，BG control 发出前在 capture 锁中重新验证，不在关键路径重查 GET_INFO 或修复对象。当前 `valid_group()` 仍通过扫描/构造完整当前快照比较，验证开销进入 IPC owner 时间、在记录的 syscall 区间外；尚未优化这部分 CPU 分配。捕获锁仅覆盖可见 ioctl，hidden/direct syscall 限制不变。

本次两进程的 group handle 数字恰好相同，hardware TSG ID 分别为 6/10。配对依据当前 PID、GPU UUID、已知 engine=1 和当前 GET_INFO 输出，不以进程间 RM handle 数字作实体判断；runlist ID unknown。清理后两个绑定都失效，不能复用这些数值。GET_INFO / PREEMPT 被接受不证明完整 CUDA context 覆盖、持续暂停、INT-next、硬件粒度或请求导致的性能改善。

## 阶段四：独立 group binding（历史）

基于研究仓库 `4f4cd179965d08f547d36ddd2d0b68ce3ec7ae71` 的本轮修改。**595.58.03 / A100 的一次 group-target GET_INFO 已接受**：ioctl=0、errno=0、NV_STATUS=0，hardware TSG ID=6。该进程中唯一 compute TSG 有 8 个已识别 compute channel，另有 3 个 copy-only TSG。此为本次快照，不是固定拓扑；没有复用阶段三的身份。完整运行记录见 [phase4_group_binding](phase4_group_binding.md)。

两种类型保持独立：

| 类型 / 路径 | 必要条件 | 本轮用途 |
|---|---|---|
| `Binding` / `Identity`，`discover()` | 唯一 compute TSG 且唯一 compute channel | 原严格路径保持不变；多 channel 仍拒绝 |
| `GroupBinding` / `GroupIdentity`，`discover_group()` / `inspect_owned_tsg()` | 唯一 compute TSG；完整保存全部已捕获、已识别 channel 及其 compute children | 仅 `get_group_info()`，不选择子 channel；不能转换为旧类型 |

`GroupBinding` 保存 PID、registry instance、build profile/source commit、client generation、原 allocating control FD 的 retained dup、device/subdevice/group 的 class/token/generation/parent、各成员及 compute-child 关系。group/channel 的 engine 只保存实际捕获值；未指定的 channel engine 在身份 JSON 中为 null，不复制父 group 的 engine 伪装成观察值。源码定义通过当前 profile SDK 校验，显式非 GR/矛盾 engine 拒绝。

FD 的生存期由 Capture 内的 client 管理：首次观察 client 时从原 allocating `/dev/nvidiactl` FD 做 `F_DUPFD_CLOEXEC`；不另开 FD 借用对象。group snapshot 引用该 retained FD/client generation。client FREE 在同一把锁内使对象失效并关闭 dup；控制前重新检查 registry、FD、PID、profile 和单 GPU scope。`GroupIdentity` 还记录最初观察的 allocating FD 数字作为来源证据，该原始数字本身不是可复用句柄。

`valid_group()` 重新比较当前祖先、成员集合与 revision。成员新增/删除/复用/rebind、compute-child 归属变化、第二个 compute 候选、client/device/subdevice/group 释放或复用、incomplete 都拒绝旧身份。revision 能捕获“新增后又删除”的暂态变化，不仅比较最终集合。无关普通对象、无 compute child 的其他 client、另一个 copy-only group 的成员 FREE 不会污染当前 group。PID/registry instance 防止同值历史 token 或 fork 继承的表重新获得操作资格。

`Capture::mutex` 覆盖已观察 alloc/free/bind 的原 syscall、账本更新，以及本次 GET_INFO 前的完整校验和 syscall。`GroupInfoOnce` 仅构造 `NVA06C_CTRL_CMD_GET_INFO`，成功或失败后都不能在该 probe 中重试。discovery、身份构造不发 control；没有任意 cmd/hObject 的公开调用入口。

单 GPU scope 要求 `CUDA_VISIBLE_DEVICES` 是一个完整 GPU UUID，当前 CUDA 枚举数量为 1，实际 UUID 匹配；短 workload 完成后才允许建立身份。对象选择还需 compute-child ancestry，不能仅按 engine=1、顺序或 GPU 张数选目标。CUDA UUID 是 CUDA 侧 scope 证据；GET_INFO 返回 TSG ID，**不验证 GPU UUID，也不证明已覆盖整个 CUDA context**。

独立状态：`group_binding_observed` 是历史捕获事实，`group_binding_valid` 表示当前快照仍有效，`group_get_info_verified` 是这次查询接受的事实。清理前后应区分，结束 context 后 valid=false。`channel_binding_observed/channel_binding_verified` 是原 `binding_observed/readonly_verified` 的明确别名；group 查询不会设置它们，也不会改变 `active_experiment_authorized/active_result_measured`。

hidden/direct syscall、内部创建/导入对象、未知 class/transport、未覆盖多 context 映射的限制仍在。`incomplete=no` 只表示未检测到缺口。stock RM 对本次对象/FD 的查询接受，不代表 PREEMPT 权限或 channel 选择通过。本轮无 KMD、SecInfo、权限或 driver 修改。

新增 `group_binding_test` 与原 tests 在独立 550/595 构建中各 **7/7** 通过；fixture 使用 synthetic handles，覆盖 1/2/3/8/17 个 channel、copy-only 排除、生命周期/暂态修订、历史进程/registry、ABI/ancestry/FD/阶段拒绝、一次正确目标 GET_INFO、ID 0、错误原值/journal 和无 active/channel 解锁。550 无本轮实机查询，595 仅上述一次。

## 阶段二、三的历史设计记录

阶段三补充：独立 595 adapter 已实际捕获对象，但同一 graphics TSG 下发现 8 个 compute channel，仍按本文件的唯一性条件拒绝绑定；没有 GET_INFO 成功记录。原限制没有放宽，证据和后续 TSG 级绑定提案见 [phase3_bringup](phase3_bringup.md)。以下保留阶段二设计说明。

**适用代码 profile：550.120 / Linux x86-64。当前只有离线测试通过；没有实机捕获成功记录。** 本轮基于 Interact_Inference `9d578f24fdaa102ad94e6db2093ec14386416138` 修复；实现见 `active-preempt/src/object_registry.{h,cpp}`、`rm_control.cpp` 的 `observe`、`remember`、`discover_owned_compute_group`、`issue`。

## 绑定方法和证据边界

```text
每个独立 process 显式 cuCtxCreate，预热本次实际 kernel / graph
  → 捕获本进程成功的 NV_ESC_RM_ALLOC (NVOS21 / 未序列化 NVOS64)
  → 保留原 /dev/nvidiactl open-file 的 F_DUPFD_CLOEXEC 引用
  → 当前 client → device(0x80)
                  ├ subdevice(0x2080)
                  └ group(0xa06c) → channel → compute object
  → 唯一候选快照（含各对象 generation、父 generation、FD/client generation）
  → 原 FD 上 A06C GET_INFO → hardware TSG ID
  → 之后每条自有 control 在锁内重新验证快照，再走标准 ioctl / SecInfo / RM 检查
```

数值 class 是固定 SDK class ID，**不是硬编码 hClient/hObject**。没有 QUERY_GROUP、自定义 escape、TID 查询、child[1]、固定 channel 数量、CUPTI channel ID 转 RM handle、全局 SecInfo 放宽或需要安装的 patch。hardware channel ID / runlist ID 尚不可得，写 unknown；engine 来自已观察的 alloc/BIND，零表示 unspecified，不能当成已验证的 GR engine。

**源码证据（NVIDIA 550.120 `5e52edb2034de7db4d8ae368dbc7c26b416bfa16`）：** `src/nvidia/src/kernel/gpu/fifo/kernel_channel.c:kchannelConstruct_IMPL` 解析 channel 的 parent；若 parent 不是公开 TSG，驱动可内部创建包装 TSG。这样的内部对象不一定经过用户 ioctl，prototype 不支持，不能从 device-parent channel 猜 group。`CliGetKernelChannelWithDevice` 同时支持 device/group parent，并不意味着二者可以在本绑定器中互换。`kernel_channel_group_api.c:kchangrpapiCtrlCmdGetInfo_IMPL` 返回 TSG ID；`rmapi/client.c:rmclientValidate_IMPL` 的严格路径比较原 open-file OS identity。调用链与权限含义见 [源码考古](source_archaeology.md)。

**运行证据：** 本机 `cuInit` 失败，没有已验证的 context → TSG 实例。GET_INFO 成功的真实 FD/ownership 行为仍待目标 host 测试；synthetic 对象不是 CUDA 资源。

## 当前表与历史表

原 `remember()` 将旧同 handle 条目标为 dead 后追加，新 FREE 会扫描所有历史 dead parent。这会让复用 handle 后的新孩子被旧 tombstone 污染。现在：

- `current_[(client,handle)]` 仅保存当前节点，其 parent 是 `(handle,generation)`；handle 再分配得到新的 generation，并递归移除旧节点的当前后代。
- FREE 沿当前 generation 关系遍历；历史事件只供 inventory 阅读，从不参与存活判断。释放无关 X 不会删除新 H 的 C2。
- client 释放只删除该 client 的当前节点并关闭其保留 FD。重建同值 client 得到新的 client generation；其他 client 不受影响。
- BIND 使被绑定节点 generation 前进，更新当前孩子的 parent generation；旧快照即使又绑回相同 engine 也无效。
- 同一 group 多个 compute channel、多个可用 TSG、多 subdevice、缺失 device/compute 祖先、未知序列化/捕获 ABI、捕获异常或容量耗尽均拒绝。容量是资源上限，不是 channel 数量假设。
- `RmControl` 验证 identity 的公开数值与 Binding 一致；每次控制在 capture 锁内验证全部 token、当前 channel 集合和候选唯一性。观测到新的相关 channel / compute object 或销毁/重绑后不继续使用缓存身份。

真实观察入口把 RM alloc/free/bind 的 ioctl **及其账本更新**与自有 controls 串行化，避免仅锁事后记录的 TOCTOU。控制关键路径的快照验证不再重新分配向量或发 GET_INFO；初始化阶段做查询。

## 限制与权限

这仍是受限的用户态观测方案，不是通用 CUDA context 查询 API。适用前提为每进程一个目标 context、单 GPU、可捕获的 550.120 allocation ABI。库内部隐藏/direct syscall、未知复制/迁移/导入、未观察到的内部对象或平台代理均可能使观察不完整；检测到不完整即 `OBJECT_BINDING_UNAVAILABLE`，但仅凭 hook 没报错无法证明没有遗漏。没有通用多 context 映射或自动重新绑定。

未审查的驱动 profile 下，hook 仍转发原 ioctl，但禁用私有 allocation payload 解码，inventory 记 ABI_UNVERIFIED；因此 CUDA baseline 无需使用 550 结构体猜测其他版本的对象。

进程内捕获锁只能覆盖经此入口观察的调用；最终 object type、owner、access rights 仍由 stock RM 在 ioctl 内验证。原 FD 的 dup 保留 open-file 身份，不能赋予 NICE 权限。GET_INFO 接受不等于 PREEMPT / MAKE_REALTIME 权限或当前运行状态已通过。`NV_ERR_INVALID_CLIENT` 分类为 OWNERSHIP_REJECTED，但 raw status 保留；仅凭这个状态不能断言唯一原因就是 FD mismatch。

若实机显示 hook 无法可靠绑定，最小缺口是 **self-object 查询/绑定**。先设计在原 RM FD、原 SecInfo 下返回带 generation/ref 生命周期的 owner-scoped opaque cookie，并在 RM 锁内验证 parent、GPU、engine；保留标准 control 权限。不要先新增 preempt primitive。条件方案与回滚边界见 [minimal_kmd_interface.md](minimal_kmd_interface.md)，本轮未生成/安装 patch。

## 离线验证

`phase2_contract_test` 覆盖唯一成功发现、多 TSG、多 compute channel、无 compute child、递归 FREE、H/C1 → free → H/C2 → unrelated FREE、旧 token 拒绝、重绑/新增 channel、client 隔离及复用、容量/不完整/不支持 ABI 拒绝。`rm_contract_test` 保留官方 ABI、EBADF、未更新 status 不能当成功与空身份拒绝。它们证明账本算法和拒绝逻辑，不证明实际 libcuda 的 interposition 完整性。

## 阶段六增量：无干预的 group 对照

`GroupNoop` / `group-bound-none` 复用现有 `GroupBinding/GroupIdentity` 与当前 GET_INFO，不改变 discover/validate 的唯一性、完整成员、generation 或原 FD 边界。新增 `prepare_group_noop` 与 `preempt_group_wait` 共用 `rm_control.cpp:group_owner_action`、`group_query_internal.cpp:GroupPreemptOnce::action` 的锁内准备；共同检查仅执行一次。随后 no-op 明确跳过 syscall，不能授权 PREEMPT、转换 channel 身份或把 `GroupInfo` 只读 probe 升级为 active。实际授权、once gate、timeout 和真实调度 ioctl 仍在独立分支。

本次匹配批次每条 run 都重新创建两个 owner、重新绑定和 GET_INFO；20 run / 40 次 GET_INFO，所有当前捕获的选定 compute TSG 各有 8 个 compute channel。这是该批次的观测形状，不是固定数量要求。完整结果与 hidden/direct-syscall 可见性限制见 [phase6_paired_preempt](phase6_paired_preempt.md)。
