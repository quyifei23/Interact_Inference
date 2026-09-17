# Active preemption prototype (NVIDIA 550.120 / sm_80)

两个独立 process 各自显式 `cuCtxCreate`，BG 使用约 80 ms 的有限 arithmetic kernel，INT 使用约 300 μs 的同类计算。没有 sleep kernel；寄存器和 shared-memory 状态参与最终 bit-exact 输出校验。所有 GPU 路径**只编译验证，尚未在 GPU 上执行**。

已实现原生 `PREEMPT`、`MAKE_REALTIME + RESTART_RUNLIST`、`DISABLE_CHANNELS` 和原 GPreempt 的 timeslice 数值配置。没有修改/安装驱动，没有使用 QUERY_GROUP、硬编码 NVIDIA 资源 handle 或全局 SecInfo bypass。

## Build

仓库布局需要同级 `open-gpu-kernel-modules/`，固定为 550.120 commit `5e52edb2034de7db4d8ae368dbc7c26b416bfa16`。使用官方 SDK header，避免手抄 ABI。需要 CMake≥3.22、C++17、CUDA toolkit、Linux x86-64。当前 CUDA 12.8.61 已成功生成 sm_80 cubin；目标主机 runtime compatibility 仍待检查。

```bash
cmake -S active-preempt -B active-preempt/build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CUDA_ARCHITECTURES=80
cmake --build active-preempt/build -j4
ctest --test-dir active-preempt/build --output-on-failure
active-preempt/build/int_worker --probe
```

`--probe` 没有可用控制设备或版本不是 550.120 时退出 77，不创建 raw.csv。probe 成功只表示 CUDA 可以初始化；完整句柄捕获、TSG 验证在 worker 初始化执行。

## 目标测试环境

使用 disposable、无其他任务的 A100；正常多 context compute mode；MPS/MIG 关闭。prototype 检查 sm_80/default compute mode、同 GPU UUID、不同 PID 和不同 GET_INFO hardware TSG ID。它不是完整的 MPS/MIG 管理器，主机配置还需实验管理员预先核实。C 需要当前 worker 的合法 NICE 权限（通常 CAP_SYS_NICE；管理员也是 RS 权限路径），脚本不会自动 sudo、设置 capabilities 或改 runlist policy。

身份来自 `librm_control.so` 对**本进程**成功的 NVOS21/NVOS64 allocation/free/bind ioctl 的捕获；根据 parent-child 关系找到 compute object → channel → A06C group，以及同 device 的 subdevice。保留原 `/dev/nvidiactl` open-file 的 dup。只有唯一 compute TSG 才继续；记录完整 inventory。libcuda 若使用 hidden/direct syscall 或不支持的 serialized allocation，捕获可能失败；此时拒绝执行，不能猜 child[1]、TSG、FD 或 subdevice。

## 普通 kernel 的实验顺序

先小样本 smoke，再每配置 1000 trials。每个配置新进程，初始化与 solo reference 不计入测量。无显式 preempt 的 baseline 仍受 NVIDIA 正常 time-slicing，不等于关掉硬件抢占。

```bash
# 只做 no-explicit-preemption smoke；output 必须是新目录。
python3 active-preempt/scripts/run_matrix.py --output results/smoke-none \
  --trials 10 --modes none

# 原生 timeslice 与 direct PREEMPT 对照
python3 active-preempt/scripts/run_matrix.py --output results/plain-ab \
  --trials 1000 --modes none timeslice preempt-wait preempt-async

# C：自动展开 force × bypass 四种组合；需要预先获得合法 NICE 权限。
python3 active-preempt/scripts/run_matrix.py --output results/plain-c \
  --trials 1000 --modes realtime

# D：一次 disable+preempt，或 scheduling-disable 后单独 PREEMPT。
python3 active-preempt/scripts/run_matrix.py --output results/plain-d \
  --trials 1000 --modes disable disable-split

# CTA 粒度判别：固定总时长，增加 CTA 波次数、缩短每 CTA。
python3 active-preempt/scripts/run_matrix.py --output results/short-cta \
  --trials 1000 --cta-waves 8 --modes none preempt-wait realtime disable
```

timeslice mode 复现 GPreempt `set_priority` 的 **BG request=1 μs、INT request=1,000,000 μs**，通过安全的原生 wrapper 发相同 RM control；未调用其不安全 QUERY/新开 FD wrapper，**不是完整论文 hint/GDRCopy reservation baseline**。原 artifact 的完整实验可作为另外一个结果集，但本项目没有运行或复制它的历史结果。GET 缓存值写到 identity 文件，不称 hardware quantum；变化参数可直接用 `int_worker --timeslice-us`。

手动单配置方式（便于 Nsight 包装）:

```bash
LD_PRELOAD="$PWD/active-preempt/build/librm_control.so" \
  active-preempt/build/int_worker --run-dir results/one-c \
  --mode realtime --force 0 --bypass 0 --trials 1000
python3 active-preempt/scripts/summarize.py results/one-c
```

先 `submit INT`，再发主动 RM 请求；B/D 的 request 通过共享内存交给 BG **自己的 CPU worker** 执行。额外 IPC 调度延迟包含在 interaction→start 中，RM begin/end 在实际 ioctl 执行进程记录。CUDA launch return 仅表示 host enqueue，不能证明 GPU 已看到 runnable work；`int_before_rm_issue` 单独分类。

D always 使用 `rewind=false, event=NULL`；`onlyScheduling=false` 请求禁止再调度且立即 preempt，INT done 后 enable。split mode 额外记录第一条 scheduling-disable control 的时间。错误/退出路径尝试 owner-side enable；不把异常恢复当测试通过。B async 每 trial 只发一次，排空工作并验证输出后才发下一次；不通过重复 async preempt 来轮询完成。

## Graph 阶段

Graph 代码已编译，当前未执行。只有普通 kernel active primitive 在目标 GPU 工作后才运行 graph 测试。runner 要求提供对应 primitive 的已完成普通-kernel结果，并检查 overlap、RM success 和输出正确性；这是最低自动门槛，仍需人工根据 no-preemption 对照/时间线确认因果，不能仅凭 overlap 宣称证明。

```bash
python3 active-preempt/scripts/run_matrix.py --output results/graph-c \
  --trials 1000 --modes none realtime --graph \
  --primitive-evidence results/plain-c/realtime-f0-b0
```

BG/INT graph 都是三个相同 topology 的 arithmetic 节点 + 完成 marker。BG 中间节点是主要长 kernel，CPU 等待该节点的 GPU marker 才触发；每节点写独立 output slice，全部与 solo reference 比较，不能用末尾节点覆盖中间节点错误。每 trial 重复 launch 同一个 graph exec，从而同时检查后续有效性。没有 graph cancellation 或 context destruction 抢占。

## 数据与边界

每配置输出 `raw.csv`、`heartbeats.csv`、`bg_ctas.csv`、前后 calibration、捕获 inventory、identity、status/failure 和 `summary.md`。详见 [measurement.md](docs/measurement.md)。用户指定的 exact BG preempt/resume 列保留为空：单 CTA heartbeat 不足以标定完整 TSG 切出/恢复。

GPU loop 有固定 iteration ceiling，每 CTA 有 2 秒 globaltimer watchdog；触发 watchdog 是无效结果并中止。它能限制正常执行的 kernel，不能恢复 GPU 硬件故障或永久未被重新调度的 channel。脚本对进程有超时，不 reset GPU。构造/测量阶段都不调用 `cudaDeviceSynchronize()`；仅在测量前后/每 trial 排空后使用 event query 和结果复制。

`STOP_CHANNEL` 与直接 CHRAM/runlist register write 不在可执行模式中。候选 E 仅有 [接口提案](../docs/minimal_kmd_interface.md)。
