# 分阶段运行准备与版本边界

**当前阶段五：** 本次重新 preflight 为 CUDA_SUCCESS、count=1、A100-PCIE-40GB / 595.58.03。获得操作者对明确 UUID 的一次主动授权后，M0/M1/M3 各完成 1 trial；两个当前 group GET_INFO 成功，BG 同步 PREEMPT 一次返回 ioctl=0 / errno=0 / NV_OK，输出/后续可用性/cleanup 通过。ordering ambiguous，未证明硬件抢占时刻或因果时延收益。见 [phase5_group_preempt](phase5_group_preempt.md)。以下“当前阶段四”等文字属于历史时点。

**当前阶段四：** 单 GPU CUDA preflight 成功；595/A100 的独立 group-info probe 已追加一次 GET_INFO，ioctl=0、errno=0、NV_OK，返回 TSG ID=6；清理全部成功。active controls=0、benchmark trials=0。channel 身份仍未验证；不继承主动授权。当前证据见 [phase4_group_binding](phase4_group_binding.md)，以下保留历史时点。

**阶段三更新：** 本次新 preflight 已能访问 8 张 A100，CUDA 最小 workload 成功。独立 experimental 595 adapter 已编译并执行 observe-only，但真实对象图含 8 个 compute channel / 同一 graphics TSG，故未进入 GET_INFO。项目 RM controls=0，benchmark trials=0。阶段三记录及交接命令见 [phase3_bringup](phase3_bringup.md)。下列设备不可访问结果保留为阶段二的历史时点，不代表当前会话。

阶段二基线为 Interact_Inference `9d578f24fdaa102ad94e6db2093ec14386416138`，启动时工作区干净，见 [initial_state.json](../results/phase2/initial_state.json)。本轮开始前的新增 preflight 文件已保留并接入；没有重建研究仓库或修改 upstream 550.120 工作树。

## 三个入口

| 阶段 | 行为 | 不通过时 |
|---|---|---|
| `--probe-cuda`（`--probe` 兼容别名） | 独立 executable：cuInit、枚举/UUID/属性、固定 256 次整数计算、event query 与 CPU reference；不检查 RM profile，不改调度 | 输出具体 CUDA API / numeric code / name / message；前置失败的 kernel 标 NOT_RUN |
| `--probe-rm-identity --run-dir NEW` | 先检查 reviewed profile；一 CTA、256 iterations 的短预热；捕获自己的对象并 GET_INFO | 保存 capture、probe_status、已知 control events；不先做 80 ms 校准 |
| `--probe-rm-readonly --run-dir NEW` | 同上，再 GET_TIMESLICE 和 GR_GET_CTXSW_MODES | GET_INFO 是必要检查；可选 getter 失败单独保留，不否定所有 RM 路径 |

`preflight.py` 另外记录 KMD、实际 libcuda/runtime 映射、Toolkit、CUPTI version、nvidia-smi 原始查询、设备 open 的 errno、MPS 环境/进程线索、MIG/GSP/虚拟化可用信息、UID/capabilities/NoNewPrivs。nvidia-smi 失败不阻止独立 CUDA 调用。baseline `none/int-only` 不依赖 RM 捕获；CUDA 成功但 RM 设备不可访问时也不能把前者判为失败。

未审查 profile 下的 CUDA baseline 还会禁用私有 ioctl payload 观察/解码，只转发调用，避免隐含使用 550 allocation ABI 解释新版 libcuda。

## 本机会话的运行证据

原始调用见 [preflight](../results/phase2/preflight/preflight.json)，最终验证入口与日志见 [阶段二结果](../results/phase2/summary.md)。

- KMD `/proc/driver/nvidia/version`：**595.58.03**。此字符串没有证明当前是 open KMD / GSP 模式。
- 实际 libcuda：`/usr/lib/x86_64-linux-gnu/libcuda.so.595.58.03`；driver API version **13020**。Toolkit **12.8.61**；runtime API **12080**，实际 libcudart **12.8.57**。
- `cuInit(0)` = **100 / CUDA_ERROR_NO_DEVICE**；`cuDeviceGetCount` = **3 / CUDA_ERROR_NOT_INITIALIZED**；GPU UUID、型号、CC 未取得；最小 kernel **未运行**。
- `/dev/nvidiactl`、`/dev/nvidia0`、`/dev/nvidia-uvm` 的 open = **-1 / errno 2 (ENOENT)**；nvidia-smi = **9**。结论是当前会话无法访问 CUDA 设备，不是断言整个宿主没有 GPU。
- CUPTI `cuptiGetVersion` = **0**、API **26**，加载 `libcupti.so.2025.1.0`。本地 12.8 `cupti_activity.h` 没有 compute-engine context-switch activity 类型。本轮未开启 CUPTI trace 后端；“库可用”不等于“事件可采集”。
- UID/EUID **1004**，CapEff/CapPrm/CapBnd 等均 **0**，NoNewPrivs=1。未授予权限。
- MPS/MIG/GSP 和 GPU 虚拟化状态 **unknown**。环境/`ps` 缺少某个线索或 `systemd-detect-virt` 输出 none，不足以排除它们。

RM identity / readonly 在 profile 门槛报告 **ABI_UNVERIFIED**；没有执行 GET_INFO 或调度修改。GPU trials = **0**。

## 550.120 与 595.58.03 的独立对照

| Profile | 源码 commit | 执行状态 |
|---|---|---|
| 550.120 artifact 对照 | `5e52edb2034de7db4d8ae368dbc7c26b416bfa16` | 默认独立 build；阶段三 6/6 CTest 通过，无 550 实机验证 |
| 595.58.03 宿主源码对照 | `db0c4e65c8e34c678d745ddb1317f53f90d1072b` | 阶段三独立 experimental build；6/6 CTest，实际 observe 已执行；**readonly / active 未验证** |

`audit_profiles.py` 对两个 checkout 实际编译 x86-64 `sizeof/alignof/offsetof/member-size` 程序。结果 [version_audit.json](evidence/phase2/version_audit.json) 中 **13 个参数结构体及使用字段、10 个 control ID 无差异**，包括 NVOS00/21/64/54、group allocation、PREEMPT、MAKE_REALTIME、RESTART、timeslice、GET_INFO、BIND、CTXSW_MODES 和 DISABLE。不能从这个结果推出语义/权限不变。

| Control | 550 flags / rights | 595 flags / rights |
|---|---|---|
| PREEMPT | 0x2210 / 0 | 0x10248 / 0 |
| MAKE_REALTIME | 0x210 / 0x2 | 0x48 / 0x2 |
| RESTART_RUNLIST | 0x2210 / 0x2 | 0x48 / 0x2 |
| SET_TIMESLICE | 0x10 / 0 | 0x10008 / 0 |
| DISABLE_CHANNELS | 0x810 / 0 | 0x10108 / 0 |

**595 源码证据：** `src/nvidia/generated/g_kernel_channel_group_api_nvoc.c` / `g_kernel_channel_nvoc.c` / `g_subdevice_nvoc.c` 是该 tag 的 metadata。`src/nvidia/inc/kernel/rmapi/control.h` 将 NON_PRIVILEGED 改为 0x8、ROUTE_TO_PHYSICAL 改为 0x40；0x10000 是 GSP_PLUGIN_FOR_VGPU_GSP。不能用 550 flag 表解码。`src/common/sdk/nvidia/inc/rs_access.h` 独立确认 NICE index=1，故 mask 0x2 是 NICE。

595 `rmapi/client_resource.c:cliresAccessCallback_IMPL` 改走 `osCheckAccess(RS_ACCESS_NICE)` → `kernel-open/nvidia/os-interface.c:os_check_access` → CAP_SYS_NICE；管理员宏仍是 CAP_SYS_ADMIN。`rmapi/client.c:rmclientValidate_IMPL` 仍有严格 clientOSInfo/open-file 比较。**没有继承 550 权限结论来跳过新路径审查。**

595 `rmapi/resource.c:rmresControl_Prologue_IMPL` 使用 IS_FW_CLIENT + routed flags，进入 NV_RM_RPC_CONTROL；RPC 源码移动至 `src/nvidia/src/kernel/vgpu/rpc.c:rpcRmApiControl_GSP`，存在 FINN 序列化及 `_issueRpcAndWaitLarge`，普通路径仍 `_issueRpcAndWait`，传输 status 与 GSP control status 分离。不能通过相同宏名声称相同 transport 开销。PREEMPT / MAKE_REALTIME / RESTART 物理 GSP handler 仍不是本轮已验证的公开调度实现。

阶段二时绑定器只解码 550 的未序列化 NVOS21/NVOS64/BIND，尚缺 595 实际 transport / 对象与权限核对。阶段三沿用上述对照，实现分别使用各自 pinned headers 的 adapter；状态码改用各自 `nvstatus.h` 定义，新增私有包络观察与分层 gate。真实 595 observe 仅出现 flags=0 的已解码 alloc/control，没有 FINN 路径；这不证明其他运行方式也无 FINN 或 hidden syscall。最早新阻塞为多 compute channel 绑定歧义；ownership、GET 和 firmware 调度支持仍未验证。版本门槛没有删除。

## 实机准入

先 CUDA → identity → readonly；确认隔离 A100、无须保护的其他 GPU 任务、MPS/MIG 状态与权限。`--test-host-confirmed` 是操作者对测试主机的确认，不授予系统权限；runner 看到现有 compute processes 会停止主动组。1 次后才使用其 evidence 运行约 10 次；更大样本要求匹配 smoke、恢复证据及显式固定 iterations。C 仍由正常 RM 决定 NICE 权限；脚本不 sudo/setcap。async、force/bypass、D 须额外 `--allow-extended`，Graph 须普通 primitive 的可解释证据与明确 review 标志。
