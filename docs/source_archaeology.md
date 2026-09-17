# NVIDIA host-triggered active preemption：源码考古

研究边界：在一个独立 CUDA context 的长 compute kernel 执行中，由 CPU 事件触发切换到另一个 context；保留 BG 状态和排队工作。触发成功、抢占完成、INT 开始和 BG 持续停止是四个不同事件。本报告中的 **源码事实**、**接口契约**、**推断**、**未验证** 不互相替代。

## 1. 固定版本和验证边界

| 对象 | 本次检查的版本 |
|---|---|
| GPreempt master | `249ee3e56d01068451e172e691853b4d055c0dec` |
| NVIDIA baseline tag `550.120` | `5e52edb2034de7db4d8ae368dbc7c26b416bfa16` |
| NVIDIA upstream `origin/main`，2026-09-17 获取 | `61dcc93722ecb418bb5f2e00923f05b4b8051dd1`，提交标题 `615.71.09`，2026-09-09 |

已完整 clone 两个 repository；NVIDIA 工作树保持在 550.120，未应用 patch。`git apply --check ../GPreempt/patch/driver.patch` 成功。upstream 通过 `git show` / `git diff` 单独检查。版本、完整调用点、历史和元数据存于 [evidence](evidence/)。

本机 nvcc 12.8.61 可用；`nvidia-smi -L` 失败，`/dev/nvidia*` 没有可用 GPU 节点；`/proc/driver/nvidia/version` 报告宿主模块 595.58.03。这既不是已验证 A100 环境，也不是 550.120 运行环境。本报告不包含 GPU 实验结论，不安装或卸载驱动。

以下 `N/` 表示工作区 `open-gpu-kernel-modules/` 的 **550.120**；`G/` 表示 `GPreempt/`。源码行号均指未修改文件。在线原始版本：[GPreempt](https://github.com/thustorage/GPreempt/tree/249ee3e56d01068451e172e691853b4d055c0dec)、[NVIDIA 550.120](https://github.com/NVIDIA/open-gpu-kernel-modules/tree/5e52edb2034de7db4d8ae368dbc7c26b416bfa16)。

## 2. GPreempt 的实际架构

`G/src/cuda-clients/gpreemptclient.cpp:159` 的 `initInThread()` 创建按 priority 共享的 `g_ctx[priority]`，取 `util_gettid()` 查询 TSG，调用 `set_priority()`。BG/BE priority 非零请求 timeslice **1 μs**；INT/LC priority 零请求 **1,000,000 μs**（`G/src/gpreempt.cpp:61`）。因此不是每个 workload 一个独立 TSG，也不是 CUDA stream priority API。

hint 路径（同文件 `preprocess():203`、`infer():235`，`G/src/block.cu`）在 LC stream 提前提交 blocking/reservation kernel；CPU 通过 GDRCopy 标志结束它，后续计算按 stream 次序运行。正式机制是短 BG timeslice 带来的被动轮转，加上隐藏切换开销的提前占用。论文 §3.1–3.2 和 §4 与此一致；论文报告的延迟不是本项目复现数据。[USENIX 正文](https://www.usenix.org/system/files/atc25-fan.pdf)

必须区分 requested timeslice 和 hardware quantum：`N/src/common/sdk/nvidia/inc/ctrl/ctrla06c.h:129` 规定向硬件支持值向下取整，且可能强制切出以更新；`:157` 说明 GET 返回原请求值，**不能**用 GET=1 证明硬件 1 μs。

## 3. driver.patch：全部 13 个文件、全部修改类别

表中 TS/句柄指“GPreempt 当前用户态路径依赖”，不表示 NVIDIA 原生功能一定需要这些修改。所有修改来自首次公开提交 `59ab51e61991412eff897e4f29ebc866f8577dda`；后续公开历史没有修改该 patch。

| NVIDIA 文件（`N/` 下） | 原行为 → 修改；用途/必要性/权限影响 |
|---|---|
| `src/nvidia/arch/nvalloc/unix/include/nv_escape.h` | 原无 QUERY_GROUP → 增加 escape `0x60`。提供私有句柄查询；句柄路径必需，TS 算法本身不需，非 instrumentation。单独增加编号不授权，但其 handler 需要权限审计。 |
| `src/nvidia/arch/nvalloc/unix/src/escape.c` | 增加头文件和全局 `g_clientOSInfo`；成功与否未检查地在 AMPERE_CHANNEL_GPFIFO_A allocation 后保存 `secInfo.clientOSInfo`；增加 QUERY_GROUP 的全局客户端遍历和 TID/8-channel 筛选；CONTROL 改用 `Nv04Control`；另加注释掉的 timing/status 日志。句柄发现 + 放松 FD 归属限制属于功能修改，其余日志只是 instrumentation。安全路径被实质改变。详见下文。 |
| `src/nvidia/generated/g_kernel_channel_group_api_nvoc.h` | 原 KernelChannelGroupApi 无 `threadId` → 增加 NvU64 字段。供查询时匹配创建线程；不改变 timeslice；编辑 generated struct 缺少生成源同步，重新生成可能丢失。 |
| `src/nvidia/kernel/vgpu/nv/rpc.c` | 新增 `unistd.h`/`nv.h`；新增未调用 `_myIssueRpcAndWait`，省略 receive 和结果检查；原 `_issueRpcAndWait` 和 `rpcRmApiControl_GSP` 加注释日志；异步分支本身仍是注释。发布路径仍同步，无已生效低延迟优化。见 §8。 |
| `src/nvidia/src/kernel/gpu/fifo/kernel_channel_group_api.c` | 构造时记录 `portThreadGetCurrentThreadId()`。只服务查询，不是抢占实现或上下文保存修改。 |
| `src/nvidia/src/kernel/gpu/gr/kernel_sm_debugger_session.c` | 增加注释掉的 debugger handles 日志；纯 instrumentation，不改权限/调度。 |
| `src/nvidia/src/kernel/rmapi/alloc_free.c` | 移除运行中的 LEVEL_INFO alloc/free 日志，换成注释的 LEVEL_ERROR 日志。日志变化，非 TS/句柄功能必需；没有更改分配/free 逻辑。 |
| `src/nvidia/src/kernel/rmapi/client.c` | invalid client 与 client OS info mismatch 路径添加错误日志，保留原验证分支。日志可证明作者排查过这些错误，但不能证明 PREEMPT 弃用原因。 |
| `src/nvidia/src/kernel/rmapi/control.c` | 客户端不存在、非 kernel client 等拒绝分支增加日志。未更改拒绝条件。 |
| `src/nvidia/src/kernel/rmapi/resource.c` | 新 include `nv.h`；control prologue 加注释 timing。无行为变化。 |
| `src/nvidia/src/kernel/rmapi/rs_utils.c` | 获取资源/客户端、dynamicCast 失败处加日志和括号。未更改权限或调度。 |
| `src/nvidia/src/libraries/resserv/src/rs_resource.c` | `NV_WARN_NOTHING_TO_DO` 被转换为 NV_OK 处加注释日志。原有转换不变。 |
| `src/nvidia/src/libraries/resserv/src/rs_server.c` | debugger control header 和注释的 `0x83de0317` 参数日志。无运行时功能变化。 |

### 3.1 TID → client → TSG → channel 的真实路径

```text
CUDA client initInThread / test-basic
  GPUCtxCreate (= CUDA context creation)
    RM alloc KEPLER_CHANNEL_GROUP_A (0xa06c)
      kchangrpapiConstruct_IMPL
        threadId = portThreadGetCurrentThreadId()       [patch]
    RM alloc AMPERE_CHANNEL_GPFIFO_A (0xc56f)
      RmIoctl -> g_clientOSInfo = secInfo.clientOSInfo [patch/global]
  NvContext.hClient = util_gettid()                    [此时不是 RM handle]
  NvRmQuery -> ioctl(/dev/nvidiactl, 0xc0204660)
    RmIoctl(NV_ESC_RM_QUERY_GROUP)
      rmapiGetClientHandlesFromOSInfo(g_clientOSInfo)
      serverGetClientUnderLock(g_resServ, each hClient)
      clientRefIter(... KernelChannelGroupApi, DESCENDANTS)
      compare group.threadId with input hClient-as-TID
      clientRefIter(... KernelChannel, CHILDREN)
      require child count == 8                        [脆弱的实验假设]
      output actual hClient + group resource hObject
      output hClientList[] + hChannelList[]
  NvRmModifyTS(actual client, group object)
```

这不是 CUDA context 的稳定官方映射 API。`threadId` 是 RM group 构造线程，未证明总等于应用 CUDA 调用线程；一个线程可创建多个 context/TSG。查询依赖**最后一个**符合 class 的 allocation 保存的全局 OS info，不能可靠支持跨进程并发。TSG/channel handle 是客户端资源名字，hardware TSG ID 是另一个量；应用不能以指针地址、TID 或不同 handle 数值替代硬件 group 身份验证。

源码还显示：QUERY_GROUP case 末尾缺 `break`，会落入 RM_FREE，32-byte NVOS54 与 NVOS00 的大小不符而拒绝；`NvRmQuery` 却忽略 ioctl 返回值，只看结构中的 status，可能把失败当成功。新 case 没有 dataSize 校验、返回列表状态/内存释放的完整处理、唯一匹配检查或明确锁保护；复制的 channels 结构其余字段未初始化。不能直接用于新 prototype。

### 3.2 `WithSecInfo → Nv04Control` 的准确含义

原 `RmIoctl` (`escape.c:285,749`) 构造：USER/USER_ROOT、USER parameters、`clientOSInfo=nvfp->ctl_nvfp` 或 `nvfp`、特定命令的 `gpuOsInfo`，传到 `Nv04ControlWithSecInfo`。

patch 调用 `Nv04Control` → `_nv04Control(pArgs,NV_TRUE,NV_FALSE)` → `XlateUserModeArgsToSecInfo` (`entry_points.c:90,175,532`)；新的 secInfo 清零，仍按 `osIsAdministrator()` 选择 USER/USER_ROOT，**并不提升为 KERNEL**，但 `clientOSInfo`/`gpuOsInfo`/process token 被丢弃。

`rmclientValidate_IMPL` (`client.c:723`) 原 strict 模式比较 `pClient->pOSInfo == secInfo.clientOSInfo`。新 secInfo 的 clientOSInfo 为 NULL，退回 `_rmclientUserClientSecurityCheck`；Unix `osValidateClientTokens` (`os.c:3615`) 接受 PID **或** EUID 相同。结果是原本被严格 FD 归属阻止的同 UID 客户端访问可能变为可行，且影响全部 RM_CONTROL，而不是仅 timeslice。

**为何这样改？** 可证事实是 `NvRmControl` 自行另开 `/dev/nvidiactl`，与 CUDA 的文件归属可能不同；patch 专门打印 OS info mismatch。由此可合理推断作者为解决句柄借用/另开 FD 的验证失败而改路径。公开提交没有解释动机，不能将该推断写成作者声明。

**权限是否扩大？** 文件/客户端隔离被放松；USER_ROOT/RS access/method flags 并未全部取消，因此“全局赋予所有 privileged RM controls”不准确。prototype 选择捕获本进程 allocation 并复用原有控制 FD，保留标准 SecInfo 和 RS 校验；失败就报告，不退回 bypass。

## 4. 用户态 API 与全部 callsites

完整 `rg` 结果见 [gpreempt_callsites.txt](evidence/gpreempt_callsites.txt)。`MAKE_REALTIME` 在 GPreempt 全树没有匹配。`NvRmQuery` 实际调用于 `test-basic.cpp:14`、`gpreemptclient.cpp:173`、`gpreemptclient_wo.cpp:81`；`set_priority` 紧随其后。`NvRmModifyTS` 只有 `set_priority` 的两处分支调用。

| API | RM command / object | hClient / hObject 来源 | target | 550.120 权限 | 实际用途/active 适用性 |
|---|---|---|---|---|---|
| NvRmQuery | 私有 escape 0x60，非 RM control | 输入 TID；输出遍历得到 client/group | 发现 TSG+children | patch 没有完整 owner 检查 | 上述 3 处；需替换发现机制 |
| NvRmModifyTS | A06C SET_TIMESLICE 0xa06c0103 | Query / group | TSG | NON_PRIVILEGED，正常 owner 检查仍需 | set_priority 调用；被动 baseline，更新本身可造成切换 |
| NvRmPreempt | A06C PREEMPT 0xa06c0105 | Query / group | TSG | NON_PRIVILEGED、rights=0 | **experimental/dead/unused path**；仅定义/声明，bWait=false；主动切出，不承诺 INT next |
| NvRmGPFIFOSch | A06F GPFIFO_SCHEDULE 0xa06f0103 | Query / **group handle** | 应为 Channel，代码对象不匹配 | NON_PRIVILEGED | **experimental/dead/unused path**；注释 Not supported yet，bSkipSubmit 未初始化；有注释的 child[1] 替代式 |
| NvRmRestartRunlist | A06F RESTART_RUNLIST 0xa06f0111 | Query / children[1] | Channel 指定所在 Runlist | NICE | **experimental/dead/unused path**；不能假定 child[1] 是 compute channel |
| NvRmDisableCh | 2080 FIFO_DISABLE_CHANNELS 0x2080110b | 第一个 ctx.client / **硬编码 0x5c000003**；list 来自 Query | Subdevice control → 多 Channel | NON_PRIVILEGED；event 仅 kernel | **experimental/dead/unused path**；false onlyScheduling、false rewind、NULL event；vector 无 64-entry 边界检查 |
| NvRmSetPolicy | 2080 FIFO_RUNLIST_SET_SCHED_POLICY 0x20801115 | Query / 硬编码 subdevice | Runlist policy（作用范围见接口） | NICE / legacy privileged | **experimental/dead/unused path**；设置 CHANNEL_INTERLEAVED |
| NvRmSetLevel | A06C SET_INTERLEAVE_LEVEL 0xa06c0107 | Query / group | TSG | NICE / legacy privileged | **experimental/dead/unused path**；改变出现频率，不是切换触发 |
| set_priority | wrapper SET_TIMESLICE | Query | TSG | 同上 | 唯一运行中的 CUDA 调度配置路径；不是 MAKE_REALTIME |

`NvRmControl` 也忽略 ioctl return/errno；其他参数存在未初始化字段。新实现必须同时记录 syscall result 与 RM status，零初始化所有参数，拒绝错误 object 类型。

## 5. 550.120 control 权限元数据

来源：`src/nvidia/generated/g_kernel_channel_group_api_nvoc.c:263–410`、`g_kernel_channel_nvoc.c:510–537`、`g_subdevice_nvoc.c:4260–4362`。完整片段见 [control_metadata.txt](evidence/control_metadata.txt)。

| command | methodId | flags | accessRight mask |
|---|---|---:|---:|
| TSG GPFIFO_SCHEDULE | 0xa06c0101 | 0x10 | 0 |
| SET_TIMESLICE | 0xa06c0103 | 0x10 | 0 |
| PREEMPT | 0xa06c0105 | 0x2210 | 0 |
| SET_INTERLEAVE_LEVEL | 0xa06c0107 | 0x110 | 0x2 |
| MAKE_REALTIME | 0xa06c0110 | 0x210 | 0x2 |
| Channel GPFIFO_SCHEDULE | 0xa06f0103 | 0x10 | 0 |
| RESTART_RUNLIST | 0xa06f0111 | 0x2210 | 0x2 |
| STOP_CHANNEL | 0xa06f0112 | 0x10 | 0 |
| FIFO_DISABLE_CHANNELS | 0x2080110b | 0x810 | 0 |
| FIFO_RUNLIST_SET_SCHED_POLICY | 0x20801115 | 0x2310 | 0x2 |

550.120 `rmapi/control.h`：0x10=NON_PRIVILEGED，0x100=PRIVILEGED_IF_RS_ACCESS_DISABLED，0x200=ROUTE_TO_PHYSICAL，0x800=API_LOCK_READONLY，0x2000=ROUTE_TO_VGPU_HOST。这些位不是硬件寄存器标志。

`rs_access.h:59` 是 access **编号**定义：DUP_OBJECT=0，NICE=1，DEBUG=2；NVOC 字段是**位掩码**，`serverControl_InitCookie(resource.c:162)` 将它复制到 `rightsRequired.limbs[0]`。所以 **0x2 表示 bit1/NICE，不是 DEBUG；0 表示无额外 rights，并不是要求 DUP_OBJECT**。

`serverControl_ValidateCookie(control.c:691)` 检查 RS rights 和 method flags；`rs_access_rights.c:39` 将 NICE 标为允许 privileged、uncached check；`cliresAccessCallback_IMPL(client_resource.c:135)` 调 `osAllowPriorityOverride` → `os_allow_priority_override` (`kernel-open/nvidia/os-interface.c:375`) → `capable(CAP_SYS_NICE)`。管理员路径的 `NV_IS_SUSER` 是 CAP_SYS_ADMIN (`kernel-open/common/inc/nv-linux.h:708`)。因此 ordinary owner 可以尝试 PREEMPT/SET_TIMESLICE/DISABLE(NULL event)，C 需要合法 NICE 权限；不应为 C 直接修改 control access table。

## 6. PREEMPT：完整的可见调用图和不可见边界

```text
G/NvRmControl 或 prototype RmControl::call
  ioctl(control fd, _IOWR('F', NV_ESC_RM_CONTROL, NVOS54_PARAMETERS))
  kernel-open/nvidia/nv.c:2706 nvidia_unlocked_ioctl
    nvidia_ioctl:2320 -> rm_ioctl:2678
  src/nvidia/arch/nvalloc/unix/src/osapi.c:2726 rm_ioctl
    escape.c:285 RmIoctl / NV_ESC_RM_CONTROL
      entry_points.c Nv04ControlWithSecInfo -> _nv04ControlWithSecInfo
      RMAPI_EXTERNAL->ControlWithSecInfo = rmapiControlWithSecInfoTls
      control.c rmapiControlWithSecInfoTls -> rmapiControlWithSecInfo
      control.c _rmapiRmControl
      rs_server.c serverControl / validation, locks, object lookup
      rs_resource.c resControl_IMPL / resource prologues
      resource.c rmresControl_Prologue_IMPL:255
        exported method flags ROUTE_TO_PHYSICAL + IS_GSP_CLIENT
        NV_RM_RPC_CONTROL
        [NVOC slot names kchangrpapiCtrlCmdPreempt_IMPL,
         but CPU build disables its function pointer for these route flags]
        GPU_GET_PHYSICAL_RMAPI->Control = rpcRmApiControl_GSP
        rpc.c:9220 -> _issueRpcAndWait:1696
        rpcSendMessage -> _kgspRpcSendMessage -> GspMsgQueueSendCommand
        rpcRecvPoll -> _kgspRpcRecvPoll -> drain responses/events
  =================== public KMD source ends ===================
  GSP-RM control dispatcher -> physical TSG preemption handler
    -> scheduler/PBDMA/engine context switch                    [未公开]
    -> return actual control status -> KMD -> ioctl
```

不是先在 CPU 执行一个公开的 `kchangrpapiCtrlCmdPreempt_IMPL`，再进入同名底层函数。550.120 tree 只有该 routed method 的 generated declaration/metadata，没有实现正文；`NVOC_EXPORTED_METHOD_DISABLED_BY_FLAG` 将相应 pFunc 置 NULL，但 prologue 先路由 RPC，使其仍可作为 control 使用。不能把不存在的 550.120 HAL 函数名补到图中。

### PREEMPT 的五个语义层

1. **target**：hObject 是 KernelChannelGroupApi 资源，关联 KernelChannelGroup / hardware TSG，不是单 channel 或全 runlist。`kchangrpapiCtrlCmdGetInfo_IMPL:1364` 返回 group.grpID，可读回验证硬件 TSG。TSG 的 channels 共享 runlist；`kchangrpapiCtrlCmdGpFifoSchedule_IMPL:1060` 检查/统一 runlistId。
2. **状态和粒度**：PREEMPT 参数没有 preemption mode，不是 reset/free/cancel 接口；保留计算与队列是 context preemption 的合理预期，但具体寄存器/shared-memory 保存、CILP/CTA/WFI 选择由 GR context/firmware 决定。公开 KMD 包含 preemption buffer 分配/映射/释放（`kernel_graphics_context.c`），不足以证明每次 PREEMPT 都作 instruction-level save。必须做状态连续性校验。
3. **bWait=true**：接口承诺等待 target preempt 完成；不是只等待发送 RPC。GSP response 应包含 handler 等待结果，KMD 又同步等待 response。但 550.120 内部到底轮询哪组 PBDMA/engine pending 位不可见，不能严称“已验证所有寄存器写回 DRAM”。返回后也不承诺 TSG 仍停着。
4. **bWait=false**：接口承诺发出 preempt 后不等 HW 完成；CPU 仍经过同步 GSP RPC 往返。无独立 completion event/poll 字段；A06C PREEMPT/GET_INFO 都不提供 completion token。文档警告未由其他方式确认完成就反复 async preempt 会有 undefined results。本 prototype 每 trial 只发一次，等待 BG 工作流完成后才下一 trial；测量不把 RPC return 当硬件完成。
5. **preempt ≠ suspend**：接口没有 disable、hold、resume 或 next-TSG 参数；不能保证 BG 保持停止，也不能保证 INT next。单 runnable TSG 的立即重选与公开语义相容，需实验观察；有 INT queued 也不能据此排除第三个 context、优先级和轮转影响。

低层相关但**不是本 control 已证实的调用链**：`kernel_fifo_ga100.c:922 kfifoStartChannelHalt_GA100` 清 CHRAM ENABLE 并写 `NV_RUNLIST_PREEMPT`；`:971 kfifoCompleteChannelHalt_GA100` 轮询 RUNLIST_PREEMPT_PENDING。现有 callsite 在 `kernel_gsp.c` 的 RC recovery 路径；不能将 recovery 的 disable/runlist 操作当安全的普通 PREEMPT_SELF 实现直接开放。

## 7. MAKE_REALTIME + RESTART_RUNLIST

### 调用图

```text
MAKE_REALTIME(INT hClient, INT TSG hObject)
  同 §6 ioctl / resource validation
  NVOC kchangrpapiCtrlCmdMakeRealtime slot, flags=0x210, rights=NICE
  rmresControl_Prologue -> NV_RM_RPC_CONTROL -> rpcRmApiControl_GSP
  GSP physical implementation [不公开]
    契约：TSG realtime、runlist 排序、非 RT compute mode=CTA

RESTART_RUNLIST(INT hClient, INT compute-channel hObject)
  同 §6 ioctl / resource validation
  NVOC kchannelCtrlCmdRestartRunlist slot, flags=0x2210, rights=NICE
  rmresControl_Prologue -> NV_RM_RPC_CONTROL -> rpcRmApiControl_GSP
  GSP physical implementation [不公开]
    契约：expire timeslice / restart corresponding runlist / preempt current
```

证据：`ctrla06c.h:367–401`；`ctrla06fgpfifo.h:201–238`。MAKE_REALTIME 是 generic TSG control，没有文档禁止 CUDA group；是否支持该 A100、当前驱动、MIG/runlist 组合仍需检查 NV_STATUS。成功后不必每 interaction 再设一次；它是对象生命周期内的配置，重建 group 后需重新设置。

文档约定：CHANNEL_INTERLEAVED 时 realtime 有最高 interleave level，且在对应 runlist 中位于 non-realtime 前；realtime 加入 runlist 时 non-RT 的 compute mode 设为 CTA，RT 保持 WFI。不是公开 CPU 代码已逐个更新了所有 BG TSG 的证据；初始化顺序、已有/后来加入 TSG、模式迁移限制都应测量。

**CTA 的证据歧义必须保留**：CTA 通常指 thread-block 粒度，而 CILP 指 instruction-level；但 550.120 `ctrl2080gr.h:781–786` 对 COMPUTE_CTA 和 COMPUTE_CILP 都写成 instruction-level，不能仅凭这个注释断言 A100 CTA 的精确 save/drain 行为。CUDA 对计算抢占能力的说明也不是本 control 的 mode 保证。判别实验应固定总 kernel 工作量，比较 resident long-CTA 与更多 short-CTA，记录 GR mode、单 CTA 生命周期及触发延迟。若延迟随最长在途 CTA 增长，C 不满足任意长 CTA 的低延迟目标；若中途保存成功，再验证 register/shared state。这是待证问题，不是预先淘汰 C 的理由。

| 参数 | 精确定义（接口契约） | 不代表什么 |
|---|---|---|
| bForceRestart=false | 若指定 channel 或其 TSG 已在 engine 运行，跳过 restart | 不是“等当前 BG 自然结束”；BG 正在运行而 INT 未运行时仍触发 |
| bForceRestart=true | 不因 target 已运行而跳过 | 不提升 preemption 粒度，也不独占 runlist |
| bBypassWait=false | 等 context switch 完成再返回 | 返回时间不等于首条 INT 指令时间；仍需 GPU start marker |
| bBypassWait=true | 发 HW request 后不等 context switch | 不跳过 KMD/GSP transport response |

`submit INT -> restart(INT channel)` 在正确 runlist、runnable INT、成功 realtime 配置下有明确的 INT-next **设计契约**，比 PREEMPT(BG) 更贴合目标。但多 realtime TSG、INT semaphore/dependency 阻塞、API return 不等于硬件已见 doorbell、WFI realtime 工作过长都可能破坏应用期待。不要把 CUDA launch return 写成已由硬件确认 runnable。

BG 没有被禁用或移出队列，预期 INT 无工作后自然恢复，不需显式 resume；是否整个 INT graph 期间连续独占也不是无条件保证，尤其 graph 含空隙/其他 engine 时。测量方法见 prototype 文档。

## 8. GSP boundary 与其他 primitives

| primitive | KMD-local 可见工作 | KMD→GSP | GSP/HW 可见程度 |
|---|---|---|---|
| PREEMPT | validate object/rights/locks、serialize | routed prologue，NV_RM_RPC_CONTROL | target preempt 契约；物理实现缺失 |
| MAKE_REALTIME | validate NICE、对象 | routed prologue | 排序/模式转换契约；物理实现缺失 |
| RESTART_RUNLIST | validate NICE、channel | routed prologue | HW request/wait 契约；物理实现缺失 |
| DISABLE_CHANNELS | `kernel_fifo_ctrl.c:735`，拒绝用户非 NULL kernel event | handler 显式 NV_RM_RPC_CONTROL | 禁调度和可选同步抢占契约；物理实现缺失 |
| SET_TIMESLICE | `kernel_channel_group_api.c:1291`；RPC 成功后缓存 requested timeslice | handler 显式 NV_RM_RPC_CONTROL | 硬件量化/更新实现缺失 |
| GPFIFO_SCHEDULE | schedulable/UVM checks、runlist 绑定 | 显式 RPC；non-GSP 分支用 INTERNAL control | enable/add 或 disable/remove；非单纯 yield |
| STOP_CHANNEL | RPC 后 `kchannelNotifyRc_HAL` | 显式 RPC 或 INTERNAL_STOP_CHANNEL | disable/unbind/remove；error notification，hard-abort reference |

`NV_RM_RPC_CONTROL` 定义在 `src/nvidia/kernel/inc/vgpu/rpc.h:221`；GSP 使用 `GPU_GET_PHYSICAL_RMAPI->Control`，**不是**照抄 vGPU 的 legacy `rpcCtrlPreempt_v1A_0A` 调度。`rpcRmApiControl_GSP:9220` 序列化 hClient/hObject/cmd/params，`_issueRpcAndWait` 执行 send + receive，区分 transport status 和 `rpc_params->status`；后者才是 GSP control handler 结果。

GSP transport 函数绑定在 `kernel_gsp.c:2106`。`_kgspRpcSendMessage:338` 经 `GspMsgQueueSendCommand` 写命令队列、`kgspSetCmdQueueHead_HAL` 通知；`_kgspRpcRecvPoll:1867` 处理响应和事件、RPC timeout。KMD 中有 register access 并不意味着 KMD 拥有常规 hardware scheduling policy；GSP 是这里的关键边界。

GPreempt `_myIssueRpcAndWait` 的确是一个“发出即返回”的试验实现：注释掉 rpcRecvPoll、rpc_result 检查。**但无 callsite**；`rpcRmApiControl_GSP` 中 `_issueRpcAsync` 也是注释，最终仍调用原 `_issueRpcAndWait`。因此不能称“GPreempt 已将同步 RPC 改异步并降低 active preemption latency”。直接启用该辅助函数还会丢失控制返回值和 response/buffer 生命周期保证，不是可安全复用的优化。

### DISABLE_CHANNELS 的关键参数

`ctrl2080fifo.h:297–350` 的契约很强：

* `bDisable=true, bOnlyDisableScheduling=false`：listed channels 不再在 HW 运行，且不会再调度，直到 bDisable=false。
* `bOnlyDisableScheduling=true`：只阻止以后调度，**不移出当前正在执行的 channel**。若要 active suspend，不能只设 true；可以 false 一次完成，或 scheduling-disable 后另发 PREEMPT（后者多一次 control 且有失败窗口）。
* `bRewindGpPut=false`：不要求丢掉未消费 commands；true 则将 PUT 退到 GET，违背保留排队工作的目标。
* `pRunlistPreemptEvent=NULL`：同步等待；非 NULL 是 KEVENT/kernel-client 接口，不是可直接传 Linux eventfd 的用户 completion API。KMD 显式拒绝用户非 NULL。

这是最接近“持续 suspend → run INT → explicit resume”的公开 primitive。源码契约支持做实验 5，但还没有证明 CUDA runtime/graph 对停调度完全透明；必须覆盖 TSG 的全部有关 channels，检查 stream/graph/状态保留，并处理控制失败和 BG 恢复。不要用 GPreempt 的硬编码 subdevice 或跨客户端列表。

SET_INTERLEAVE_LEVEL 影响 runlist 出现频率；FIFO_RUNLIST_SET_SCHED_POLICY 改调度 policy；二者不是单独的 active trigger。STOP_CHANNEL 产生 RC/error 路径，不能作为正常 suspend/resume。

## 9. 为什么公开 GPreempt 没用 NvRmPreempt？

**可以证实**：全部 6 个公开提交中，`src/gpreempt.cpp` 和 `patch/driver.patch` 只在初始提交加入；`git log -S NvRmPreempt` 和 blame 都回到初始提交。整个可达历史没有 PREEMPT 调用/删除调用的证据，没有基准、失败结果或弃用说明。只有 GPFIFO wrapper 带 Not supported yet 注释，不能扩展解释到 PREEMPT。

**可作为机制解释、不可作为作者动机**：PREEMPT 不给 INT-next/hold；有 GSP 往返；另开 FD 的 ownership 问题确实存在；mode/等待策略会影响延迟。公开材料不足以在这些原因之间作选择。准确答案是：**发布方案确实没有使用它，公开历史无法回答作者为什么未采用它；也没有证据证明它比 timeslice 慢或不可用。**

## 10. 最新 upstream 的单独检查

在本次获取的 `615.71.09` 中，A06C PREEMPT/MAKE_REALTIME 和 A06F RESTART 文档核心语义保持；GPFIFO_SCHEDULE 新增 bSkipEnable，ABI 不应混用。flag 位定义发生变化，不能把最新 flags 用 550.120 的表解释。

最新 `kernel_channel_group_api.c:1467` 出现公开 `kchangrpapiCtrlCmdPreempt_IMPL`：GSP client 仍 RPC；non-GSP 部分 bWait 时临时 disable group，退出时 re-enable，注释明确担心 PBDMA/engine preempt 期间 group 被装回。这为“preempt 非永久 suspend”提供**新版源码证据**，并不证明 550.120 firmware 采用相同实现；公开 non-GSP 代码也未给出完整硬件 preempt 操作。MAKE_REALTIME / RESTART 仍以 routed metadata 为边界。

## 11. 当前可证结论

现有 RM 已经提供 host-triggered、以 context/TSG/channel 为对象的候选接口，不需要先发明 scheduler。最需要硬件判别的是：C 的 mode/长 CTA 延迟；B 的重选和 INT-next；D 的 CUDA 状态/队列恢复；以及本进程句柄/原 FD 捕获在 550.120 libcuda 中是否可行。所有候选的实际 latency、状态保存细节和 graph 透明性在当前环境均未验证。
