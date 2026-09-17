# NVIDIA host-triggered active context/TSG preemption

先读 [推荐结论](docs/recommendation.md)，再看 [完整源码考古](docs/source_archaeology.md) 和 [候选设计对比](docs/candidate_designs.md)。

* [最小用户态 prototype 与运行方法](active-preempt/README.md)
* [阶段二实际修改与测试](docs/phase2_changes.md)
* [阶段三真实 GPU bring-up 与当前阻塞](docs/phase3_bringup.md)
* [分阶段运行准备及 550/595 版本边界](docs/runtime_readiness.md)
* [RM 对象绑定与生命周期](docs/object_binding.md)
* [测量口径与无法观测的边界](active-preempt/docs/measurement.md)
* [条件性的最小 KMD 接口提案](docs/minimal_kmd_interface.md)
* [阶段一记录](results/summary.md) / [阶段二实际验证结果](results/phase2/summary.md)
* [版本、调用点、历史及 NVOC 元数据证据](docs/evidence/)

`GPreempt/` 和 `open-gpu-kernel-modules/` 以 Git submodule 固定源码版本；后者保持未修改的 550.120 checkout。没有安装或卸载驱动，没有生成虚假的 GPU benchmark 数据。

克隆本项目并恢复这两个源码目录：

```bash
git clone https://github.com/quyifei23/Interact_Inference.git
cd Interact_Inference
git submodule update --init GPreempt open-gpu-kernel-modules
```

固定版本：GPreempt `249ee3e56d01068451e172e691853b4d055c0dec`，NVIDIA 550.120 `5e52edb2034de7db4d8ae368dbc7c26b416bfa16`。上述命令只初始化本研究使用的两个源码仓库；构建完整 GPreempt 另需按其 README 安装依赖。
