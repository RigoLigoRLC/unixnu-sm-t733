# Metal-to-Vulkan 上游同步

2026-10-29。本轮在继续 GPU tracing 前同步 Metal-to-Vulkan 运行时与必要的 Turnip 依赖。

## 固定上游

| 来源 | 固定版本 | 本轮用途 |
| --- | --- | --- |
| UniXNU main | `066c8d8315f83749d28bcae2faf3595718646be5` | 合入公共主分支更新 |
| UniXNU GPU 开发分支 | `e5f729d1ba299f1fe1ffd6e8dc68dbbaa321fe0c` | 读取匹配的运行时版本、编译选项和 Mesa 补丁 |
| AdrenoMetal GPU 开发分支 | `547312c` | 移植新运行时接口及导入、纹理和堆修复 |
| metal2vulkan development | `624bab3918de5b978e1101517abca2b4fa426565` | 与上游桥接匹配的新运行时 |

`development` 已发生强制更新。本轮使用上游 UniXNU 实际固定的提交，构建继续以完整提交号定位源码。同步前版本保存在各仓库的 `sync/upstream-m2v-2026-10-29-before` 标签。

## 合入边界

本机 `NativeBridgeMetalFrontend` 保留资源声明、阶段同步、硬件一致性查询和系统 BGRA8 sRGB 策略，在此基础上适配新运行时。上游开发分支还包含 iPadOS 27、A740 显示、编码线程和诊断代码；本轮按具体接口与依赖移植。

新运行时以配套 AdrenoMetal 的 20 份补丁为基础。SM7325 的缓存一致性内存选择、内存池增长协调、空闲命令池限制和按 CPU 消费者导出 GPU 写入继续由本机补丁提供。新 `FenceUse` 按 draw 阶段表达依赖，本机同步适配遵循这个实现。

Turnip 保留本机异步提交、资源范围、BO 缓存及硬件一致性补丁，追加导入附件的行跨度与 GMEM 加载/存储修复。未填满 GMEM 块的外部线性附件使用 draw load 和 2D store，将传输限定在有效像素。缓存和渲染模式统计通过现有状态导出。

`unixnu-sm-t733` 保留优化与 SDR 策略。用户要求 Apple3 暂不处理，`unixnu-sm-t733-apple3` 各仓库保持本轮之前的提交及固定版本。独立目录中的 Apple3 迁移准备没有进入分支。GPU 内核继续使用 64a 的 `83a0453`，本轮不改变寄存器采集路径。

## 构建输入

`AdrenoMetal/bridge/patches/series` 记录补丁顺序。构建器支持同一文件上的顺序补丁，用临时副本逆向还原后逐字节比较固定源码。运行时的 shader cache 标识继续随提交和补丁内容变化。Rust release 采用上游的 fat LTO 和单 codegen unit。

设备 bootstrap 在运行时版本改变时保留旧展开目录，再准备新的固定提交。原始系统输入、Data 种子和软件钥匙串密钥保持原有位置。

主分支已登记上游 20 份补丁与本机 `0021` 至 `0023`，分别承载内存池及一致性选择、空闲命令池边界、资源阶段和 CPU 可见性。64a 的实机采样仍见 [硬件采集记录](GPU_HARDWARE_CAPTURE_64_2026-10-29.md)。

## 65a 构建输入

| 仓库 | 提交 |
| --- | --- |
| core | `d16b3384aee7908234831ff1f28d8beff03c3810` |
| AdrenoMetal | `6fb945121e11bd851d236becc7204e8690c50322` |
| SM7325 | `7f308116117a92bc315de97ed3c5e3a6b308f999` |
| Adreno6xx | `83a0453e981567a30cecd6486bdb8b3b33d64683` |

设备及组件使用不可移动标签 `build/system-gpu-upstream-65a-source-1`。统一命令：

```sh
source tools/activate.sh
bash tools/build-t733-recovery.sh system-gpu-upstream-65a --system --payload-only \
  --local-module soc:sm7325=/Users/rigoligo/expm/unixnu-sm-t733/sources/unixnu-sm7325-common
```

65a 的前五阶段退出 0，Metal 阶段退出 1：本机 render pass 切分补丁仍直接比较新的 `Versioned<PassShape>` 包装。修正为比较解引用后的 `PassShape`，对应固定源码保存在 65b 的构建记录。65a 原始输出与标签保留。实机仍运行 64a；本轮未部署。
