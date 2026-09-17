# 验证状态：没有 GPU benchmark

2026-09-17，本地完成源码考古、候选设计、用户态 prototype 和离线检查。**GPU trials = 0**。没有创建 `results/raw.csv`，也没有给出任何 GPU latency 分位数。

| 检查 | 实际结果 |
|---|---|
| GPreempt clone / 历史检查 | 完成，HEAD `249ee3e56d01068451e172e691853b4d055c0dec` |
| NVIDIA 550.120 clone / checkout | 完成，HEAD `5e52edb2034de7db4d8ae368dbc7c26b416bfa16` |
| 最新 upstream 单独检查 | 完成，`61dcc93722ecb418bb5f2e00923f05b4b8051dd1` / 615.71.09 |
| GPreempt driver.patch 静态应用检查 | `git apply --check` 成功；没有应用 patch |
| CUDA/C++ build | CUDA 12.8.61、GCC 11.4、sm_80 编译链接成功；见 [build.log](build.log) |
| ABI/错误路径检查 | passed：官方参数大小、ioctl number、EBADF 不当成功、缺失 identity 拒绝 |
| 统计脚本检查 | passed：分位数、失败 syscall/RM 筛除、INT-before-RM 单列、空/截断 CSV 拒绝；fixture 只在临时测试目录，不是 GPU 数据 |
| CTest | 2/2 passed；见 [tests.log](tests.log) |
| CLI / Python / shell | help/非法 mode、Python 编译、shell syntax 检查完成 |
| GPU 环境探测 | `nvidia-smi` 无法通信；无可访问 `/dev/nvidiactl`；harness exit 77，正确 skip |
| 宿主驱动 | `/proc/driver/nvidia/version` 报告 595.58.03；不是目标 550.120 |
| driver module build/install/unload/reset | 未执行 |

环境原始记录见 [environment.txt](environment.txt)；harness 的环境探测目录为 [environment-probe](../active-preempt/results/environment-probe/summary.md)。驱动源码和 GPreempt clone 都保持干净。

## 明确未执行

* context/TSG 实机身份确认、FD interposition 成功率、真实权限/firmware support。
* No explicit preemption、timeslice、PREEMPT wait/async、realtime/restart 四组合、disable/enable GPU 实验。
* register/shared-memory context save、queued commands 保留、BG resume、CUDA Graph 透明性。
* hardware timeslice quantum、CPU→GPU 控制延迟、抢占完成/切换/恢复时间及 p50/p95/p99/max。
* Nsight Systems/GPU context-switch trace。

编译和离线单元测试不证明 GPU 抢占可行。精确 HW completion 和完整 TSG resident 状态本身也不是当前 heartbeat 所能直接观测的，详见 [measurement.md](../active-preempt/docs/measurement.md)。

目标 disposable A100/550.120/GSP host 的复现步骤见 [prototype README](../active-preempt/README.md)。实机运行脚本只在工作实际执行后生成 raw samples；默认每配置 1000 trials，失败不冒充成功。
