# 候选设计：先判别语义，再比较延迟

基线版本、控制元数据和来源见 [source_archaeology.md](source_archaeology.md)。本表的“保持”是接口意图/预期，不是已通过 CUDA 正确性测试；没有实测 latency 排名。

## 1. 语义对照

| 字段 | A：GPreempt timeslice | B：PREEMPT(BG) | C：MAKE_REALTIME(INT)+RESTART(INT) |
|---|---|---|---|
| target object | BG/INT TSG | BG TSG | INT TSG 配置 + INT channel 所属 runlist |
| preemption granularity | 当前 GR mode，未固定 | 当前 GR mode；PREEMPT 参数不选择 mode | 文档将 non-RT compute 改 CTA、RT 保持 WFI；CTA 精确语义有文档歧义 |
| context preserved? | 预期是；论文使用上下文切换 | 预期是；非 reset/free | 预期是；非 reset/free |
| BG automatically resumes? | 是，按 runnable/policy | 没有 hold，可能立即重选 | 预期 INT 无工作后自然继续 |
| guarantees INT next? | 否；等待轮转 | 否；没指定 INT 或 runlist 起点 | 文档明确表达 realtime next；须 runnable、同 runlist、正确 policy，多个 RT 时非独占保证 |
| requires privileged RM control? | SET_TIMESLICE 无额外 rights | PREEMPT 无额外 rights | NICE；合法 CAP_SYS_NICE/管理员授权路径 |
| requires GPreempt patch? | 原 artifact 需要；同样 timeslice 配置本身不需 | 否；安全的 owned-handle discovery 必须解决 | 否；同左 |
| requires new KMD patch? | 句柄发现可行则不需 | 同左 | 有合法 NICE 和句柄则不需 |
| requires GSP modification? | 无已知需要 | 无已知需要，仍受 firmware 支持限制 | 无已知需要，可能 NOT_SUPPORTED |
| kernel modification? | 不改业务 kernel；hint variant 增加 reservation kernel | 不改业务 kernel | 不改业务 kernel |
| expected CPU→GPU control latency | 事件路径无 RM RPC；等待剩余量化 timeslice + 切换 | 每 interaction 一次 RM/GSP 往返 + HW switch；false 不等 HW | init 一次配置 RPC，事件一次 restart RPC；bypass 不免 transport |
| failure modes | quantum 下界、轮转、状态大小、hint 不及时 | BG 重选、其他 TSG 获胜、async 未完成重入、mode 粗、错误句柄 | NICE 拒绝、CTA 延迟、wrong channel/runlist、多个 RT、公平性、INT 依赖未就绪 |
| portability | 私有 ABI/量化差异；论文方法可跨架构概念迁移 | RM/GSP/驱动版本相关 | RM/GSP/policy/RT 支持相关 |
| CUDA Graph transparency | context 层面预期透明；本项目未测 | 同左 | 同左；不能将 graph 内空隙解释为持续独占 |

| 字段 | D：DISABLE_CHANNELS | E：最小 self-scoped KMD 接口 | F：STOP_CHANNEL |
|---|---|---|---|
| target object | 自己 subdevice + 全部有关 BG channel 列表 | 经原 FD 验证的自己 TSG/channel | channel |
| granularity | 当前 GR mode，false onlyScheduling 请求 preempt | 取决于复用的 B/C/D；接口不会创造新粒度 | 等 idle 或强制 preempt；可进入 RC |
| context preserved? | false rewind 时预期保留；须测 CUDA 状态 | 不能强于所复用 primitive | 不能承诺正常 CUDA 恢复 |
| BG automatically resumes? | **否**；必须 bDisable=false | B/C 可自然恢复，D 显式恢复 | 需重新 schedule/bind/enable，CUDA 还可能已报错 |
| guarantees INT next? | BG 不再 runnable；不排除第三方 context | 取决于 underlying primitive | 否 |
| requires privileged RM control? | NULL event 可由普通合法 owner 调；非 NULL event 仅 kernel | prototype 可额外 CAP_SYS_ADMIN；原 NICE/owner 验证保留 | metadata NON_PRIVILEGED，但仍有 ownership |
| requires GPreempt patch? | 否 | 否，禁止全局 bypass | 否 |
| requires new KMD patch? | 发现句柄可行则不需 | 是，但须先证实现有接口的缺口 | 不需 |
| requires GSP modification? | 无已知需要 | 复用 RM control 则不需；新低延迟 transport 未证可行 | 不需 |
| kernel modification? | 不改业务 kernel | 不改业务 kernel | 不改 kernel，但破坏应用 execution |
| expected control latency | disable/preempt + re-enable 两次同步控制；split variant 三次 | ownership wrapper 开销 + 原 RPC，**不能预言更快** | 无可比正常交互延迟 |
| failure modes | 漏 channel、恢复丢失、控制失败后部分状态、event 误用、queued work 丢失 | TOCTOU、句柄复用、跨 owner、GPU/GSP 状态不同步 | error notifier、unbind/remove、RC、不可恢复 |
| portability | channel list/ABI/firmware 相关 | 维护 pinned KMD 的成本最高 | 不是目标语义 |
| CUDA Graph transparency | 关键待测对象 | 继承 underlying primitive | 不符合本项目正常 graph preemption |

## 2. 三个最值得先做的主动候选

**C 是最明确表达“INT next”的接口组合**。依据是 550.120 MAKE_REALTIME/RESTART 的明确契约，而不是性能猜测。优先测试 long-CTA 与 short-CTA 对照、四种 force/bypass 组合、NICE 权限和队列 readiness。若长 CTA 的抢占延迟不满足需求，不因存在 realtime 名称就继续主推。

**B 是成本最低、最直接的 owner-side active trigger 判别**。权限元数据不要求 NICE，事件只需一次 control，适合测试硬件能否中途保存当前 compute 状态。但只能当 trigger，不能把它包装成持续 suspend 或 INT-next 保证。若 BG 被重选，应记录为调度语义结果。

**D 是有持续暂停语义的对照**。`disable=true, onlyScheduling=false, rewind=false, event=NULL` 的文档承诺 blocked-until-enable；可区分 B 的触发不足和实际 context save 问题。其成本、恢复责任与 C/B 不同，不能只比最快数字而忽略语义。

A 必须保留为实测 baseline；其初始化式配置可能避免事件 RPC 开销。E 只有 discovery/授权或 transport 瓶颈得到实证后才进入实现。F 只作 hard-abort 文档参照，不在默认 benchmark 执行。

## 3. 判别实验与停止条件

| 要区分的机制 | 实验 | 观察与解释 |
|---|---|---|
| 主动请求 vs 自然轮转 | A、B、C、D 与 no-explicit-preemption，同样两个进程/同一工作量 | no-explicit-preemption 仍有驱动正常 time-slicing；不把 INT 能运行本身当主动机制证明 |
| long CTA vs kernel 边界 | 固定 BG 总 50–100 ms，增多 CTA 并缩短每 CTA | latency 随单 CTA 时长变化提示 CTA 限制；INT 插入后 BG bit-exact 输出检查验证状态连续 |
| B 只切出 vs 保持暂停 | queued INT 后一次 B，与 D 比较 | BG heartbeat 在 INT 前/中继续是重选线索；单个 sentinel 停止不证明整个 TSG idle |
| RPC vs HW 等待 | B wait=true/false；C 四组合 | issue→return 与 trigger→GPU-start 分开；async return 不代表完成 |
| timeslice 请求值 vs 实际行为 | 请求 1/10/100/200/1000 μs，多次相位随机触发 | GET 仅缓存值；记录 heartbeat gaps/latency 分布，Nsight timeline 复核 |
| instrumentation 扰动 | solo、有/无 heartbeat、mapped ping-pong 校准 | 输出原始 overhead/不确定度；不盲目减常数得到“精确 switch latency” |
| Graph 透明性 | primitive 先在普通 kernel 成功，再多节点 BG/INT graph | 检查 graph 中间长 kernel 的重叠、BG 后续节点、runtime errors、bit-exact 输出和再次 launch |

默认每配置 1000 trials，先小规模 smoke。任何 RM/CUDA 错误、watchdog、identity 歧义均单独计数/中止，不进入成功样本分位数。INT start 早于 trigger 的样本无效。INT start 早于 RM issue 说明自然调度已发生，不能归功于该次 trigger。所有数据保留这一分类。

本机没有 GPU，以上实验未执行；因此不能选出测量意义的 winner。
