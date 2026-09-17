# 阶段三：真实 GPU 可达性与首次 RM observe

基线 **ceb92b70b5cedeb49b513f32feb7fb49923be0e4**，初始工作区干净，[initial_state](../results/phase3/initial_state.json)。没有回退代码、修改 550 submodule、设备权限、capability、系统隔离或 driver/firmware；没有 reset、远程连接或 push。

## 本次运行事实

[新的 preflight](../results/phase3/preflight/preflight.json) 于 2026-09-17 执行，**cuInit=0 / CUDA_SUCCESS，cuDeviceGetCount=0 且 count=8**。8 张设备均报告 A100-PCIE-40GB / CC 8.0；最小 32-thread、固定 256 次整数 workload 在 device 0 完成且 CPU reference PASS。旧阶段二的 error code 3 不是设备数，这次也没有沿用旧结果。

当前 KMD / 实际 libcuda 是 **595.58.03**；libcuda 路径 `/usr/lib/x86_64-linux-gnu/libcuda.so.595.58.03`。driver API=13020，runtime API=12080，Toolkit=12.8.61，libcudart=12.8.57，CUPTI API=26（无 trace backend）。nvidiactl/nvidia0–7/UVM 可 open。nvidia-smi 报告 MIG Disabled、virtualization None、GSP firmware 595.58.03，compute processes 为空。未发现 MPS daemon 线索，但 MPS 不能仅由环境/进程快照排除。UID=1004，effective/permitted capabilities=0，NoNewPrivs=0、Seccomp=0；这不是主机独占或主动操作授权。KMD 版本字符串也不能证明其实际二进制就是公开 open-KMD 源码构建。

所选路径 **B：595.58.03 experimental adapter**。observe-only 的进程环境仅将 CUDA_VISIBLE_DEVICES 指向已通过 preflight 的 **GPU-99e4e85f-1945-866c-9e00-130b51df7908**，没有修改系统配置。

| 阶段/事实 | 本轮状态 |
|---|---|
| static_abi_reviewed | true；沿用阶段二 ABI/metadata/权限/RPC 对照，补 envelope/class/status 定义检查 |
| observation_enabled | true；build/runtime 均为 595.58.03，阶段在 cuInit 前显式选择 |
| CUDA 当前 context 有限 workload | 完成；参考比较通过 |
| 私有 alloc/control 包络实际捕获 | 1060 条观察记录；173 alloc、37 FREE、720 normal controls、130 其他 metadata |
| 可操作 binding_observed | **false**：multiple compute channels in one TSG |
| readonly_verified | **false / 未执行**；项目 GET_INFO 等=0 |
| active_experiment_authorized | **false**；未替操作者确认测试主机 |
| active_result_measured | **false / 未执行**；PREEMPT 等=0 |
| benchmark / Graph trials | **0 / 0**；probe 不是延迟实验 |

真实 [observe execution](../results/phase3/observe595/execution.json) exit **77**，无 timeout/强杀。计数来自失败前 snapshot；**720 是 libcuda 正常调用，不是项目追加 control**。完整状态见 [profile_state](../results/phase3/observe595/profile_state.json)，对象图见 [object_graph.json](../results/phase3/observe595/object_graph.json)，原始包络见 [observed_ioctls.jsonl](../results/phase3/observe595/observed_ioctls.jsonl)。失败后 `cudaFree`、event/stream destroy、host unregister、cuCtxDestroy 全部返回 0，见 [cleanup](../results/phase3/observe595/probe_cuda_cleanup.log)；这是普通 probe 清理，不是 BG 抢占/恢复成功。

## 最早阻塞：真实对象关系超过单 compute-channel 假设

捕获 snapshot 有 116 个当前对象，registry 自报 `incomplete=no`；这仅表示未触发已检测的遗漏/格式错误，**不保证没有 hidden/direct syscall**。已解码的 allocation/control flags 均为 0；本次未观察到 BIND control 或 FINN payload，不能把未运行路径算作实机通过。

同一 client 下有 4 个 `0xa06c` TSG：一个 engine=1 的 graphics TSG，包含 **8 个 `0xc56f` channel，各有 `0xc6c0` compute child**（另有 `0xc6b5` copy child）；另 3 个 TSG 的 engine=11/12/13，各有 4 个 copy channel，没有已识别 compute child。`0x9067` 是 context-share 对象，不是第九个 compute channel。**未确定主 arithmetic launch 使用其中哪一个 channel；未读取 hardware TSG ID。** 不把八个 channel 解释成八个 CUDA context，也不猜 libcuda 创建它们的原因。

本轮在 `ObjectRegistry::discover()` 的多 compute-channel 检查处停止，未删除该检查、未选最后一个、未运行必然同样失败的 readonly/benchmark，也未将观察到 alloc 当成 stock RM ownership 验证通过。

**源码依据（NVIDIA 595.58.03，db0c4e65c8e34c678d745ddb1317f53f90d1072b）：**

- `src/nvidia/src/kernel/gpu/fifo/kernel_channel.c:kchannelConstruct_IMPL`（约 344–424）按 parent 查询 `KernelChannelGroupApi`，channel 保存 `pKernelChannelGroup`；同一 TSG 可以含多个 channel。约 696–740 将 engine type 从 parent TSG 继承给 channel。
- `src/common/sdk/nvidia/inc/class/cl2080_notification.h` 定义 engine 1=GRAPHICS、11/12/13=COPY2/3/4；`clc56f.h`、`clc6c0.h`、`clc6b5.h`、`cl9067.h` 定义上述 classes。
- `kernel_channel_group_api.c:kchangrpapiCtrlCmdGetInfo_IMPL`（约 1360–1374）从当前 group 返回 grpID；`ctrl/ctrla06c.h` 的 GET_INFO / PREEMPT 参数不含选择子 channel 的字段。该源码契约不是本次 GET_INFO 运行成功证据。

**下一步最小方案（未实现、不是放宽当前 guard）：** 审阅一个独立的 *TSG 级绑定* 类型，要求唯一拥有的 compute TSG、保留它的完整 channel/generation 集合及原 FD；先只允许 group-target GET_INFO。现有要求唯一 compute channel 的绑定类型继续拒绝歧义，C 的 RESTART 和 channel getter 仍需独立 channel 选择证据。不能用 group 唯一性替代 stream→channel 映射，也不能用历史 handles 发请求。这是已有用户态绑定范围的缺口；当前证据没有表明必须新增 KMD primitive。

## Adapter 实际修改与验证

- `CMakeLists.txt` / `build_profile.h.in`：550 与 595 分 build、分 headers，分别校验 pinned commit；550 submodule 保持原状。595 源码来自独立 `/tmp/interact-inference-nvidia-595.58.03` checkout。
- `driver_profile.{h,cpp}`：独立事实/权限字段；`configure_rm` 在首次 CUDA ioctl 前明确选择阶段。若有提前发生的 ioctl，记捕获不完整，而不是静态 Capture 忽略后来参数。readonly/observe 成功不会授权 active，也不要求过去 active 成功才可首次授权。
- `rm_observation.{h,cpp}`：仅按对应 SDK 解码 NVOS21/plain NVOS64、NVOS00、NVOS54/BIND；先检查 size/flags，FINN/未知格式不解引用 payload，拒绝绑定。记录原 syscall/errno/NV_STATUS，不改变 libcuda 返回值。数值 class 有本 profile SDK static_assert。
- `rm_control.cpp` / control journal：新操作仍需当前 generation/原 FD/同进程；GET_INFO 成功才设置 readonly；主动操作另需当前验证绑定、GPU 范围和授权。状态解释使用所选 SDK 的 `NV_ERR_*`，journal 记录实际 profile/source commit。
- `int_worker` 新增 observe-only probe，无项目 GET 或修改；readonly 每次重新捕获与验证；BG/INT 原有 owner、恢复、schema 2 路径保留。

550 与 595 均编译通过，分别 **6/6 CTest**。新增 tests 围绕 profile/阶段授权、未知 ABI/FINN、历史 identity、设备缺失明确原因，以及真实八-channel 形状的 synthetic 拒绝回归；原 tests 保留。离线 fixtures 不放入实机结果目录。精确命令/退出码见 [commands.json](../results/phase3/commands.json)，[550 tests](../results/phase3/validation/final_ctest550.log)、[595 tests](../results/phase3/validation/final_ctest595.log)。没有大样本统计或性能 winner。

## 执行交接

需要已有授权、CUDA 可访问、版本匹配的 Linux x86-64 / A100 环境；先核实目标 GPU 与其他任务，不假设部署方式。以下从仓库根目录执行，输出目录需为新的；源码工作树用已有 checkout，不升降级驱动：

```bash
# NVIDIA_595_SOURCE 指向未修改的 db0c4e65... checkout
cmake -S active-preempt -B active-preempt/build-595 \
  -DAP_RM_PROFILE=595.58.03 -DNVIDIA_SOURCE="$NVIDIA_595_SOURCE" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CUDA_ARCHITECTURES=80
cmake --build active-preempt/build-595 -j4
python3 active-preempt/scripts/preflight.py \
  --binary active-preempt/build-595/preflight_cuda --output results/preflight-next
LD_PRELOAD="$PWD/active-preempt/build-595/librm_control.so" \
  active-preempt/build-595/int_worker --probe-rm-observe --run-dir results/observe-next
```

**对当前已确认的同一环境，不需重复上述失败 probe；下一步唯一最小动作是审阅前述 TSG 级绑定范围方案。** 只有 observe 能合法建立可操作身份后，才使用 `--probe-rm-readonly` 在新进程重新绑定。之后再单独取得测试主机隔离及一次主动操作授权，依次 1 次 int-only、none、preempt-wait；不自动填写 `--test-host-confirmed`。当前不进入该阶段，也不因无 NICE 权限而尝试 sudo/setcap。

不能声称：GET_INFO/权限通过、PREEMPT 被接受、INT 因此更早开始、BG 已抢占/恢复、CTA/CILP 粒度、p99、优于 timeslice、Graph 透明性或连续请求已解决。
