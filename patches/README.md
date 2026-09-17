没有生成或应用驱动 patch。现有 PREEMPT、MAKE_REALTIME/RESTART_RUNLIST、FIFO_DISABLE_CHANNELS 尚未经过目标 GPU 的可用性/延迟实验，不能先断言需要新增 driver primitive。

条件性方案见 [minimal_kmd_interface.md](../docs/minimal_kmd_interface.md)。禁止照搬 GPreempt 的通用 SecInfo 路径修改。
