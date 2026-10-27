# A642L CPU 缓存维护删除

日期：2026-10-28。目标是消除 GPU、显示以及相应页表路径中由驱动执行的
CPU clean/invalidate，由硬件完成能够承担的一致性工作。

## 恢复点

实施前的设备提交为 `1a2a489`，沿用 `unixnu-sm-t733` 分支。
它保存既有设计文档，并保留已构建 59b 的全部源码引用：core `59a4391`、
SoC `1829984`、Adreno6xx `ded753f`、AdrenoMetal `fd02456`、
ArmSmmu4K `d92ebe7`、NativeMobileDisplay `0090571`。

## 内存选择与映射

普通自有 BO 默认使用 CPU WB，GPU 采用硬件一致性映射。Metal 的 Shared、
WriteCombined、Private、heap 和实际分配 backing 的 Memoryless 路径优先选择
cached coherent 内存。Turnip 的内存类型、内部 BO 和 CPU 映射保持这一选择。

导入内存在固定页面时读取实际属性，并在建立映射时选择 cached coherent 或
Normal NC。混合属性按物理区间处理。提交阶段使用已经建立的映射。
不能作为普通 RAM 映射的页面在导入阶段返回错误。

DPU 按实际页面属性选择对应描述符；其页表使用 WB 内存及 outer-shareable
WBWA 读取。GPU 的一致性页表读取同样使用 outer-shareable 配置。

## 删除范围

- 普通 BO 的强制 WC 转换、初始 CPU 清理和复用时的属性重置。
- GPU 提交和完成中的 CPU clean/invalidate，以及只服务于维护的范围处理。
- DPU 每次 present 的整张表面清理及其计时状态。
- GPU/DPU 一致性页表的 CPU 逐行清理。
- 硬件一致性提交中的 Metal IOSurface 刷新集合和重复刷新调用。

GPU 自身缓存命令、CPU 内存屏障、SMMU TLB 同步、任务引用和完成 fence
继续承担设备访问顺序与资源寿命管理。

已有导入 NC/混合资源的访问冲突等待规则继续保留，一致性资源沿用免检查路径。
自动审批拒绝删除这一等待规则，理由是可能影响 GPU 数据访问顺序；最终改动只
移除其 CPU 缓存操作。

## Surface 与特殊内存

现有 XNU 补丁 `native-t733-managed-rt-memory.patch` 在所选启动策略下把
managed RT 页面映射为 Normal NC。该规则不能用来认定某个 IOSurface 的属性；
GPU 和 DPU 使用实际页面查询结果。实施不增加 IOSurface 创建 hook。

GMU 和 ZAP 固件对象按其硬件访问约定保留所需映射。XNU 在建立或解除这类
映射时执行的页面属性转换属于 VM 生命周期，不能等同于逐次渲染缓存维护。

## 构建状态

`system-gpu-coherent-60a` 已通过统一构建的九个阶段，产物位于
`build/releases/system-gpu-coherent-60a/`。本轮没有运行测试、设备测量或刷写。

构建源码为设备 `0a363f3`、core `7eefbce`、Adreno6xx `e043d72`、
AdrenoMetal `0d69cf8`、ArmSmmu4K `599ba82`、NativeMobileDisplay `3b922d0`；
SoC 保持 `1829984`。这些仓库均固定不可移动 tag
`build/system-gpu-coherent-60a-source-1`。完整版本和命令保存于产物目录
`source-revisions.json`，阶段退出状态位于 `stages.tsv`。

统一命令在工作区根执行：

```sh
source tools/activate.sh
bash tools/build-t733-recovery.sh system-gpu-coherent-60a --system --payload-only --local-module soc:sm7325=/Users/rigoligo/expm/unixnu-sm-t733/sources/unixnu-sm7325-common
```

前一构建 `system-gpu-coherent-60` 在 Turnip 阶段报告重命名函数缺少前置声明。
修复已同时写入头文件和维护补丁，旧构建日志和源码 tag 保留原身份。
