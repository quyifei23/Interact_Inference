# 条件性的最小 KMD 接口提案（未实现、未安装）

优先尝试 stock 550.120 的原有 RM controls。prototype 捕获本进程成功的 RM allocations，保留 CUDA 原 control FD；以 A06C GET_INFO 验证 TSG ID，限制唯一 compute TSG 与其子 channels。没有可靠捕获时拒绝 active mode，不能退回全局遍历或猜测句柄。

只有实机证明 libcuda 的 ioctl 无法被捕获、正常 owner 路径仍拒绝、或明确需要 API 封装时，才新增受控接口。没有证据表明必须改 driver，故本次没有 `patches/active_preempt.patch`。

建议的接口只允许 `QUERY_SELF_COMPUTE_GROUP`、`PREEMPT_SELF_TSG`、`PROMOTE_SELF_TSG_REALTIME`、`RESTART_RUNLIST_FOR_SELF_TSG`。在同一原有 nvidiactl open-file 上工作；caller 提供的 resource handle 仅作查找键，必须经过原 `clientOSInfo`/client/resource 归属验证。不得把 PID、EUID 相同或 caller 自报 TID 当作充分 ownership 证据。

实现顺序：

1. 在标准 escape dispatch 中验证 structure size/version、reserved=0、operation allowlist；不接受 raw RM command 或任意 kernel pointer。
2. 可额外要求 CAP_SYS_ADMIN；保持原 SecInfo；在 RM API/resource lock 内解析 client、TSG、channel，验证当前 FD/client、父子关系、GPU/engine/runlist；保留资源引用直到结束，防止 free/reuse。
3. QUERY 输出 owner-scoped opaque cookie，带对象 generation/lifetime。后续操作再次验证 cookie 与 FD，而非接受外部 hClient/hObject 列表；拒绝多候选/迁移/失效。
4. 复用现有 RM control 的合法 dispatch 与权限判定。PREEMPT 的 wait/timeout 有界；realtime/restart 保留 NICE 检查；返回 syscall、RM、completion 语义分离的结果。
5. 不改变通用 `Nv04ControlWithSecInfo`、`rmclientValidate`、NVOC 的全局权限或 RPC receive 路径。若发现本接口需要超出已有 NICE 权限，那是新的授权设计，不能隐藏成 helper。

不建议直接开放 `kfifoStartChannelHalt_GA100`：现有调用是 RC recovery，既清 channel enable 又 restart runlist，不是独立的稳定 self-TSG control；需要和 GSP 账本、恢复、锁及错误通知协调。跳过同步 RPC 也不能仅删除 `rpcRecvPoll`，因为 buffer/response ownership、错误传播和未完成请求串行化仍需解决。

在 disposable A100 host 上完成负例（其他进程/FD/group、失效 cookie、错误 channel、并发 free、权限拒绝）和 B/C/D 正确性实验后，才有依据写可安装 patch。当前只有源码分析与可编译用户态 prototype，不需要 build/install KMD。
