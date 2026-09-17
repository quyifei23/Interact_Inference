# 阶段二实际验证记录

**GPU trials = 0；没有 GPU raw.csv、性能分位数或实测 winner。** 本记录区分离线通过、设备调用失败与 RM 未执行。初始 HEAD/工作区见 [initial_state.json](initial_state.json)，实现变更见 [phase2_changes](../../docs/phase2_changes.md)。既有阶段一记录没有覆盖。

## 构建与离线验证

| 检查 | 实际结果 | 记录 |
|---|---|---|
| CMake configure，CUDA 12.8.61 / C++17 / sm_80 | exit 0 | [configure.log](validation/configure.log) |
| 最终 CUDA/C++ targets 增量构建 | exit 0 | [build.log](validation-final/profile_guard_build.log) |
| 最终 CTest | **5/5 passed** | [ctest.log](validation-final/profile_guard_ctest.log) |
| Python analysis | **7/7 passed** | [python_analysis.log](validation-final/python_analysis.log) |
| Python runner | **7/7 passed** | [python_runner.log](validation-final/python_runner.log) |
| CLI help / 非法 mode | exit 0 / exit 1，符合预期 | [help](validation/help.log)、[invalid_mode](validation/invalid_mode.log) |
| CUDA 静态资源信息 | cuobjdump exit 0；不是实际 occupancy / 运行观测 | [cuda_resources.log](validation/cuda_resources.log) |

精确命令和 exit codes：[首次完整验证](validation/commands.json)、[最终代码验证](validation-final/commands.json)。两组 Python tests 也包含在 CTest 内，不应把它们重复相加为独立 GPU 实验。最终 CTest 还运行原 ABI/EBADF/空身份测试、纯 CPU 生命周期/路由/恢复测试、实际 C++ CSV/journal writer 与 Python parser 的集成测试。

离线成功路径包括唯一对象发现、handle/client generation 复用和递归释放；拒绝路径包括多 TSG、多 compute channel、多 subdevice、未知/不完整捕获及旧身份。临时 fixtures 验证 K0-before/K1-after、ambiguous、已测 INT 区间但 BG 未完成、INT timeout、已返回/在途 control 崩溃日志、enable 失败责任保留、missing executable errno 和整个进程组超时清理。`/dev/full` 测试确认 JSON 导出错误可检测。全部 synthetic 数据仅在测试临时目录，均不属于 GPU measurement。

## 分阶段 probe

| 阶段 | 真实调用/结果 | 结论 |
|---|---|---|
| 独立 CUDA | cuInit(0)=100 / CUDA_ERROR_NO_DEVICE；cuDeviceGetCount=3 / CUDA_ERROR_NOT_INITIALIZED | 当前会话不可访问 CUDA 设备；最小 kernel NOT_RUN |
| `--probe` 兼容别名 | 同样 exit 77 | 对应 CUDA 阶段，不再先拦截 RM profile |
| RM identity | exit 77 / ABI_UNVERIFIED | KMD=595.58.03，当前运行 adapter 只支持 550.120；GET_INFO 未执行 |
| RM readonly | exit 77 / ABI_UNVERIFIED | 可选 getter 未执行；不是 NV_ERR_NOT_SUPPORTED 的运行证据 |
| 默认 int-only/none runner | exit 77，在独立 preflight 停止 | 未进入调度/性能 trials |

对应原始记录：[CUDA probe](validation/cuda_probe.log)、[alias](validation/probe_alias.log)、[identity inventory/status](identity-probe/probe_status.txt)、[readonly inventory/status](readonly-probe/probe_status.txt)、[runner attempts](matrix-readiness/attempts.json)。捕获失败时 inventory 已保存，未用猜测 handles 发请求。

[完整 preflight JSON](matrix-readiness/preflight/preflight.json) 保存调用、返回码、stderr、errno、设备节点/权限与版本。nvidia-smi=9；/dev/nvidiactl、nvidia0、UVM open=-1/ENOENT。实际 libcuda=595.58.03，driver API=13020，runtime API=12080。CUPTI GetVersion 成功、API=26；没有采集 GPU trace。UUID/型号/CC、GSP/MIG/MPS GPU 状态未取得，写 unknown；无相关 capability。不能由这些结果断言整个宿主没有 GPU。

## RM 与 GPU 实验计数

| 操作/证据 | 本轮实际计数或状态 |
|---|---|
| 真实对象 capture / ownership / GET_INFO 成功 | **0** |
| PREEMPT wait / async | **0 次**，没有 RM 返回值 |
| MAKE_REALTIME / RESTART_RUNLIST | **0 次**，没有 RM 返回值 |
| SET_TIMESLICE / DISABLE / ENABLE | **0 次** |
| M0–M5 GPU trials / Graph trials | **0 / 0** |
| BG 恢复和输出/状态连续性实机检查 | 未执行 |
| 精确抢占完成、context-save、resume latency | unmeasured |

原 ABI 测试故意调用 `ioctl(fd=-1)` 得到 EBADF，这不是向 GPU RM 发控制。不存在 CONTROL_ACCEPTED，更不存在 PREEMPTION_CONFIRMED。恢复通过只指离线 Mock 与 CPU 进程退出测试，不能推广为 GPU 恢复已验证。

550.120 与 595.58.03 的静态 ABI/metadata/权限/RPC 对照见 [runtime_readiness](../../docs/runtime_readiness.md) 及 [version_audit.json](../../docs/evidence/phase2/version_audit.json)。本轮没有安装、卸载、升级/降级驱动，没有修改系统权限或测试 GPU reset。

下一步是在已审查 profile 且 CUDA 可访问的隔离测试主机上执行独立 probe，再进行 1 次 int-only/none、1 次 preempt-wait；有合法 NICE 权限后测试 realtime-only/realtime-restart。具体命令及后续 10/1000 阶段门槛见 [prototype README](../../active-preempt/README.md)。当前 595.58.03 会话不能用删除版本检查绕过 adapter 缺口。
