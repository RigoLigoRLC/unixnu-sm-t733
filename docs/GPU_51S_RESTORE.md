# GPU 51s 源码恢复

2026-10-26，本轮恢复 51s 的 GPU 执行路径，保留当前设备身份、配对、软件钥匙串、加密修复、HUD、触点显示和它们的开机设置。恢复后以 `system-gpu-51s-restored-52a` 统一构建。

51s 的原始构建使用过未提交源码。本轮根据当时工具补丁、上游同步前保存的 Git stash 和补丁文件，撤回 51s 后的修改；各组件以新的正常提交保存恢复结果。旧版原始 FAT 与本轮包含非 GPU 修复的新构建分别记录。

- Adreno6xx 恢复固定 GPU 性能表、ABI v1、16 个 IB、固定内存 span 容量和原提交处理；撤回后加的 DVFS、硬件采样、提交 profiling、索引和 TLB 优化。
- Metal 与 Turnip 恢复 51s 前端和补丁组合，保留当时已用的板级 ABI、空闲池回收及 tile-store 分配错误处理。
- 显示恢复双槽 IOSurface 扫描、原 producer 等待及定时器 vsync；撤回后加的四槽复用、producer 超时处理、硬件帧号 vsync 和显示 profiling。
- 构建恢复 51s 的 core GPU 源码入口，保留原有直接扫描和 CPU copy 回退菜单。
- 身份、配对、加密、Tools、HUD、触点、背光、触控和其他非 GPU 功能保持当前源码。

构建前提交组件、core、SoC 和设备的全部源码，设备提交固定依赖引用。不可移动的同名 Git tag 标记本轮构建；产物目录记录提交号、命令及各阶段退出码。

历史启动 FAT 仅保留 `system-format2-48f` 和 `system-native-install-51s`。本轮新产物单独保存；原始分区备份、Data 和密钥不属于旧构建 FAT 清理范围。
