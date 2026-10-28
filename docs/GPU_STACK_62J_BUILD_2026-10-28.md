# GPU stack 62j 构建交付

2026-10-28，SM-T733 / A642L，iPadOS 17.7.11 21H461。`system-gpu-stack-62j` 的 configure、resolve、kernel、menu、turnip、metal、wallpaper、debug-tools、payload 九阶段均退出 0，生成 536870912 字节 FAT。MSC 枚举返回空列表，当前未刷入；被动 USB collector 正在等待连接。

## 源码身份

不可移动标签：`build/system-gpu-stack-62j-source-1`。以下源码在构建前已提交，均已推送 `unixnu-sm-t733` 分支。

| 仓库 | 提交 |
| --- | --- |
| 设备 | `4916133b41c7590168269122aba1337735ffe33a` |
| core | `128261cb55c34c8a06f2159517e0ba60c539e3fe` |
| SM7325 | `3c94061dda872b59d81a72d1210ed20a67235223` |
| Adreno6xx | `512567f430e4a35f58a3c06aaedc1b2c45e24868` |
| AdrenoMetal | `6a0eb2a3e5e3fe96e4e98145c140c739be26b4b3` |
| NativeMobileDisplay | `d15d6c91dd01698262404755d0568bd85277e46b` |
| ArmSmmu4K | `599ba82f96f3e10b0afbd784c040127d38e2683a` |

从工作区根执行的构建命令：

```sh
source tools/activate.sh
bash tools/build-t733-recovery.sh system-gpu-stack-62j --system --payload-only \
  --local-module soc:sm7325=/Users/rigoligo/expm/unixnu-sm-t733/sources/unixnu-sm7325-common
```

产物与逐阶段日志在 `build/releases/system-gpu-stack-62j/`，其中 `source-revisions.json` 保存完整身份和命令，`stages.tsv` 保存各阶段返回值。普通部署只更新 512 MiB FAT，保留 Data。

## 本包实现

- GPU 异步任务队列、timeline、完成中断、按完成点睡眠等待，以及按 IOSurface 生产者和 DPU 读占用交接。
- 系统 sRGB 声明、21H461 QuartzCore/RenderBox 格式选择修正、MobileGestalt 缓存策略版本刷新。
- Metal 参数编码器、间接 draw/dispatch、ICB、细分曲面、阶段和资源依赖、resolve 参数、默认采样位置和系统 IO 队列接入。
- 256M texture buffer、PVRTC GPU 解码、XR 采样/混合/resolve、静态纹理配 argument-buffer sampler、depth/stencil resolve。

具体调用过程见 [GPU 栈实施](GPU_STACK_IMPLEMENTATION_2026-10-28.md)、[Apple3 前端](APPLE3_FRONTEND_IMPLEMENTATION_2026-10-28.md)和[显示格式策略](DISPLAY_FORMAT_POLICY_2026-10-28.md)。DVFS 和 Apple4 起的完整 tile/imageblock/ROG 继续延后。

62a 至 62i 的失败日志保留在各自 release 目录。构建修正覆盖 GPU volatile 计数器和动态数组遍历、Mesa ABI 头文件登记和 descriptor state 传递、Rust 缓存枚举、render 状态复制与初始化、Vulkan flags 接口和 bridge Region 构造。最终 62j 完整编译链接成功。

## 剩余交付

待设备进入 MSC 后刷入，随后记录启动、系统 BGRA8 表面选择、GPU/DPU 完成交接和 Metal 调用结果。格式策略首次刷新磁盘缓存后，需要完整重启使常驻消费者重新读取。当前没有本包的实机结果，也没有执行额外测试。AppleFamily3 约 90% 是实现与交付工作量估算；功能覆盖仍以本机实际调用结果记录。
