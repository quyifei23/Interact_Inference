# 阶段六：匹配 group 绑定路径的 10 对实验

**实现、两套离线测试与获授权的 10 对实机实验已完成。** 在本批次固定条件下，追加同步 PREEMPT 的组在 9/10 对有较短的 INT entry 观测延迟、10/10 对有较短的 done 观测延迟；包含一条 entry 变慢样本，没有删样或加样。全部 treatment 的时间顺序仍为 `ordering_ambiguous`。这提供了值得进一步验证的应用延迟改善证据，未测得具体硬件抢占完成时刻/粒度，也没有 p99、SLA 或稳定性能 winner 结论。

## 起点、实际实现和测试

研究仓库起始 HEAD **`2abdb065a010e978fbca2cb28fe812d20bfe878a`**，工作区干净：[initial_state](../results/phase6/initial_state.json)。以下为该基线之后的工作区实现；没有回退、修改 KMD/GSP/权限/时钟/全局调度、reset、push 或新增 primitive。阶段五旧 none 不做 group 查询与准备，因此只保留为历史背景，不进入新配对。

| 文件 / 关键入口 | 本次改动 |
|---|---|
| `mode_plan.h`、`options.h`、`driver_profile.*` | `group-bound-none` / 独立 `GroupNoop` 阶段；同一 reviewed profile 和捕获路径，不能设置 active 授权。增加 delay、batch/pair/order/condition/run metadata |
| `rm_control.*:group_owner_action` | 两种 group 操作共用一次 capture 锁内 PID/profile/UUID/原 FD 检查；取锁前开始准备计时 |
| `group_query_internal.*:GroupPreemptOnce::action` | 两组共用完整 registry/generation/ancestry/成员快照、确切 GET_INFO 凭据和准备元数据；随后分为无 syscall 的 skip 或受授权/once/timeout 约束的同步 PREEMPT |
| `control_events.*` | 复用已有预分配 journal，增加本地准备事件；无 syscall 不冒充 NV_OK，二进制 schema2 布局不变 |
| `bg_worker.cu`、`int_worker.cu`、`protocol.h`、`trial_record.h` | 相同 owner 初始化、当前 group GET_INFO、IPC、独立 observer、结果/短计算复用/cleanup；增加 owner prepare/action 与计划/实际 trigger 时间，外层 pair_id 不改变本地 trial0 |
| `run_paired.py`、`pair_contract.py` | 固定 10 对计划、冻结配置/二进制指纹、spawn 前 reservation、已用/未知额度、逐组清理核查、错误停止；不重试/补样/恢复批次 |
| `analyze_pairs.py`、`summarize.py` | 整数 ns 先相减；全部原始值/状态、配对差值、CT/TC、正常正确子集和分段开销；不生成小批次尾分位数 |
| `group_preempt_test.cpp`、`test_pairs.py`、CMake | no-op 与 active 共用门槛、false attempted/null status、once/trial0、旧 guard；固定计划/预算/配置/未知失败/整数统计/不利结果保留 |

未放宽严格单-channel guard、原 group-info 一次只读查询、原 FD/ownership、完整成员 generation 或 hidden/direct syscall 的限制。唯一候选仍只是捕获到的一个 compute-containing TSG，不证明覆盖全部 CUDA context 资源。真实 PREEMPT 的 SDK 参数仍全初始化、wait/manual=true、timeout=1,000,000 μs；B 没有 hold/resume 语义。

共同 preparation 包括锁、环境/FD读取、完整对象校验、凭据和 journal metadata；不把整段都解释为注册表扫描。真实分支额外有授权/额度检查、最终 dispatch、PREEMPT event 和 syscall，不能声称 CPU 指令逐条相同。control 不人工等待一个“匹配时长”。

550.120 和 595.58.03 各 **9/9 CTest 通过**：[550 build](../results/phase6/validation/build550.log)、[550 tests](../results/phase6/validation/ctest550.log)、[595 build](../results/phase6/validation/build595.log)、[595 tests](../results/phase6/validation/ctest595.log)。新增 Python **16/16**：[test_pairs](../results/phase6/validation/python_pairs.log)。原 ABI/EBADF/NV_STATUS、registry、多-channel、profile、GET_INFO、权限/once、timeout journal、observer/Graph entry、进程树和 schema 回归仍通过。另有 [10 项 CLI 拒绝/help 检查及阶段五 binary journal 兼容恢复](../results/phase6/validation/cli_and_legacy_journal.json)。synthetic 数据只在临时目录，未写入实机 raw。

NVIDIA source/header 继续分 build：550=`5e52edb2034de7db4d8ae368dbc7c26b416bfa16`；595=`db0c4e65c8e34c678d745ddb1317f53f90d1072b`。未在 595 宿主运行 550 私有 RM 请求。沿用阶段五对 `ctrla06c.h`、NVOC metadata/ownership/RPC 的审查；本轮没有新增其底层语义结论。

## 当前环境、非主动准备、计划与新授权

[新 preflight](../results/phase6/preflight/preflight.json)：cuInit=0、设备枚举 API 返回0/count1、最小计算与清理通过；所选 **A100-PCIE-40GB / CC8.0 / 108 SM**，UUID **GPU-99e4e85f-1945-866c-9e00-130b51df7908**，实际 KMD/libcuda **595.58.03**。runtime12.8.57、toolkit12.8.61；GSP/MIG/虚拟化 raw query 保存在 preflight，MPS 排除未由程序证明。每条正式样本前后另记录当前驱动、进程、clock/温度/利用率，均在关键路径之外；没有修改环境。

构建有变化，先做 **1 次非主动准备**：[preparation](../results/phase6/preparation/group-bound-none-f0-b0/summary.md)。新 BG/INT PID2339687/2339686 各自当前查询成功（各一个 TSG、8 channels，查询 ID6/10），项目 GET_INFO=2、PREEMPT=0、其他 controls=0。BG instrumented solo=84.649 ms，INT=294.912 μs，固定 iterations 保持1630976/5888。输出、后续短计算、cleanup通过；该样本不进入正式10对。

随后冻结 [plan.json](../results/phase6/paired595/plan.json)，seed20260917，5 TC/5 CT、每对同一预选 delay。在任何新 PREEMPT 前，用户明确回复 **“确认条件，授权该批最多 10 次尝试”**，确认所选 GPU 隔离/同卡任务保护条件：[active_authorization](../results/phase6/paired595/active_authorization.json)。阶段五授权没有复用，空进程快照也没有代替授权。另一 GPU 上的 VLLM 未被操作。

正式执行 **20/20 run、10/10 pair**，每 run 两个新进程/context，当前身份和 GET_INFO 重建。每个 BG owner 仅一次 trial0，下一组启动前上一组进程树退出且 cleanup 核实。批次 **GET_INFO=40，PREEMPT=10，其他项目 controls=0**；libcuda observed RM controls 在 owner cleanup 前快照合计28800，与项目 controls 分开。计入非主动准备时本阶段是21个单 trial run、GET_INFO42、PREEMPT10。没有额外 active smoke、重试、替代样本、未知或超出额度：[attempts](../results/phase6/paired595/attempts.json)、[逐条审计](../results/phase6/paired595/evidence_audit.json)。

实际命令（当前批次已结束、授权额度已用完；不得重复执行）：

```bash
cmake --build active-preempt/build -j 6
ctest --test-dir active-preempt/build --output-on-failure
cmake --build active-preempt/build-595 -j 6
ctest --test-dir active-preempt/build-595 --output-on-failure

CUDA_VISIBLE_DEVICES=GPU-99e4e85f-1945-866c-9e00-130b51df7908 \
python3 active-preempt/scripts/run_matrix.py --build active-preempt/build-595 \
  --output results/phase6/preparation --trials 1 --modes group-bound-none \
  --bg-iterations 1630976 --int-iterations 5888 --trigger-delay-us 3000

python3 active-preempt/scripts/run_paired.py plan --build active-preempt/build-595 \
  --preparation results/phase6/preparation/group-bound-none-f0-b0 \
  --gpu GPU-99e4e85f-1945-866c-9e00-130b51df7908 \
  --batch-id phase6-20260917-a --output results/phase6/paired595

# 收到本批次明确用户授权后才执行，已完成：
CUDA_VISIBLE_DEVICES=GPU-99e4e85f-1945-866c-9e00-130b51df7908 \
python3 active-preempt/scripts/run_paired.py execute \
  --plan results/phase6/paired595/plan.json --test-host-confirmed
```

## 匹配配置和全部原始配对值

两组每 pair 的共同配置指纹一致。BG/INT iterations=**1630976/5888**，grid均216、block256、dynamic shared65536、registers25、static shared0、occupancy估计2 CTA/SM；heartbeat2000 ns，plain arithmetic + 原有 completion marker，Graph=false，无 cancellation polling。二进制（含 kernel）、595 profile/source、初始化/reference/1000 ping、同 context 后续256次短计算均相同。两 owner 实际 CPU affinity都是0–95，不另设 affinity/优先级。未直接比较不同进程的数值 handle 或复用历史TSG ID。

正式20 run各当前查询的group含8个compute channel，是本次观测形状，不是数量规则；两个owner各同GPU/显式engine范围内查询到不同当前TSG ID，runlist仍unknown。plan冻结配置和binary hashes、每run configuration/identity/journal/cleanup都保留；结束后重新核对二进制仍与plan一致。

单位 μs。Δ=control−treatment，正值表示treatment观测间隔较短。以下没有排除任何 pair：

| Pair | 顺序 | delay | C entry | T entry | Δ entry | C done | T done | Δ done |
|---|---|---:|---:|---:|---:|---:|---:|---:|
|0|TC|3255|1132.679|408.592|724.087|1387.731|666.554|721.177|
|1|TC|2344|2037.684|403.284|1634.400|2289.445|675.729|1613.716|
|2|CT|4102|280.412|280.860|**−0.448**|537.595|535.993|1.602|
|3|CT|4562|1918.537|400.796|1517.741|2174.624|658.134|1516.490|
|4|CT|3228|1156.234|406.940|749.294|1413.622|667.017|746.605|
|5|TC|4825|1656.571|422.123|1234.448|1913.889|688.843|1225.046|
|6|TC|3130|1255.725|409.687|846.038|1512.696|666.499|846.197|
|7|CT|1855|433.622|427.525|6.097|690.868|689.520|1.348|
|8|CT|3661|723.341|418.627|304.714|982.310|678.561|303.749|
|9|TC|1875|413.596|407.852|5.744|671.240|666.941|4.299|

机器可读全量字段：[pairs.csv](../results/phase6/paired595/pairs.csv)；全部独立维度、分段开销、顺序子组和 raw：[paired_summary](../results/phase6/paired595/paired_summary.md)、[paired_analysis](../results/phase6/paired595/paired_analysis.json)。每run的 raw 与 journal 都在对应目录；binary journal 在导出并校验无损后 gzip 保存。

| 指标 | control中位数 | treatment中位数 | 配对Δ中位数 | 配对Δ均值 | Δ范围 | 改善/变慢/相同 |
|---|---:|---:|---:|---:|---:|---|
|entry|1144.457|408.222|736.691|702.212|−0.448…1634.400|9/1/0|
|done|1400.677|666.979|733.891|698.023|1.348…1613.716|10/0/0|

CT/TC各5对：entry配对Δ中位数 **304.714 / 846.038 μs**；done **303.749 / 846.197 μs**。这只是记录的顺序子组差异，不能从5对推断顺序效应。pair2/7/9的改善很小或略变慢，保留原值，不把微小差异当强机制证据。预设host delay不保证硬件调度相位相同；trigger lateness中位数 C/T为0.079/0.085 μs，实际值逐条记录。

## 返回、控制分段、正确性与证据边界

全部10次PREEMPT来自各自BG owner，target为当前group hObject，不是子channel或hardware ID。均 **ioctl=0 / errno=0 / NV_STATUS=NV_OK**，参数字节`0101000040420f00`。各control首先进入预分配journal，之后才等待CUDA完成。20次共同准备均有效；control对应`SKIPPED_BY_DESIGN`，真实RM时间/status留空，GET_INFO不计作主动请求。

| host区间中位数（μs） | control | treatment |
|---|---:|---:|
|INT submit|39.920|39.697|
|IPC send→owner received|0.268|0.307|
|owner received→prepare begin|0.310|0.469|
|共同prepare|37.927|38.215|
|prepare end→RM begin|不适用|1.468|
|PREEMPT syscall wall time|不适用|472.165|
|IPC往返|39.066|512.866|

PREEMPT host wall time范围297.329–479.718 μs，**不是纯硬件抢占/context-save时间**。INT entry/done为独立observer的host观测；CPU/GPU时间不混减，未启用校准映射模型。全部10次treatment都是ambiguous，不能由“RM后看到marker”推断GPU一定在RM之后开始，也不能保证INT-next或BG持续暂停。

20/20应用观测有效，20/20最终输出bit-exact、BG同context/stream短计算复用和cleanup通过；BG在interaction/owner检查前已完成为0。lifetime overlap为20/20；驻留情况unknown。错误、超时、缺失、不完整、unknown尝试、强杀均为0；正常正确子集与全批次相同。最终输出一致不能证明没有重放、完整寄存器保存或instruction-level preemption。

**受限结论：** 追加PREEMPT这项干预在本GPU/profile/有限固定workload、匹配准备的小批次中出现重复应用延迟改善，值得后续验证。它不证明硬件事件细节、所有调度相位/负载下稳定改善，或优于尚未匹配实测的timeslice方案。CUDA Graph、多队列、连续交互、latest-request-wins、cancellation/安全回收均未扩展。

**下一步唯一最小研究动作：** 另行设计一次独立的调度时间线诊断，先确认目标观测后端能区分context-switch事件并关联当前两个TSG，再申请新的单次运行授权。诊断开销不混入本批次；本轮不再追加任何PREEMPT。
