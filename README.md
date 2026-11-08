# UniXNU-SM-T733

Samsung Galaxy Tab S7 FE Wi-Fi (`SM-T733`, `gts7fewifi`, Qualcomm SM7325) 的 UniXNU 设备仓库。

**删除旧工作区后重新构建，从 [完整重建教程](docs/REBUILDING.md) 开始。**
教程包含固定源码、主机环境、私有身份恢复、Apple/Samsung 输入重取、系统构建、MSC 更新和固件打包。
只需另存不能重新生成的身份、密钥、固定 Data 种子和设备专有原始备份。

GitHub 入口为 [RigoLigoRLC/unixnu-sm-t733](https://github.com/RigoLigoRLC/unixnu-sm-t733)。设备引用 14 个独立驱动和 kit 子模块；来源与固定提交见 [GitHub 仓库清单](docs/GITHUB_REPOSITORIES.md)。SM7325 共用配置在 [unixnu-sm7325-common](https://github.com/RigoLigoRLC/unixnu-sm7325-common)，由 `feeds.toml` 固定提交。

当前采用官方 format 2 的 ARM64、SoC、OS、设备四层配置，76b 的官方 core 基线为 `350ddc2`。公共适配由 `support/UniXNU.rev` 固定，Samsung UEFI 单独由 `support/Mu-Silicium.rev` 固定。本地目录名可以是 `UniXNU-gts7fewifi`，公开仓库名为 `unixnu-sm-t733`。

## 实机状态

2026-11-04，`system-gpu-upstream-76b` 完成九阶段构建和完整 512 MiB FAT 写入、sync。
首次启动发布了 Metal 加速器并运行 SpringBoard/backboardd，保存的日志记录 1247 次显示完成、
failed/errors 为 0，DPU 持续翻转。颜色和交互体验尚待用户反馈。
源码和实机证据见 [76 记录](docs/GPU_UPSTREAM_REBASE_76_2026-11-04.md)。

47a 用户记录约 58 秒到 Apple logo，再约 12 秒到锁屏。既有 46b 内存实测为 4548722688 字节，约 4.236 GiB；48f 本轮没有另做原生内存采集。下表保留已取得的各项运行结果。

| 功能 | 状态 |
| --- | --- |
| UEFI、UniXNU 菜单、XNU 启动 | 可用，菜单可滚动 |
| 八核、GIC、架构定时器 | 八核已运行；三个 Edge 簇、perfcontrol 和 27 路 TSENS 的详细记录见既有 45f |
| UFS 系统卷与持久 Data | 可启动桌面，96 GiB Data 跨重启保留 |
| 显示 | Adreno Metal 合成，DPU 直接扫描 IOSurface；保留 CPU 调试回退 |
| FT8203 触屏、物理按键 | 点击、滑动可用；电源及音量上/下完整 DOWN/UP 已取得 |
| SM5714 电量计 | 电量、电压、充电状态可读，插拔中断即时更新 |
| 背光与关机 | 46l 用户已确认正常调光、锁屏熄灯、唤醒和正常关机 |
| USB 调试与 ECM | 日志、状态和调用栈采样可用；原生 debugserver 采集及 detach 已运行 |
| MSC | 日常更新写入完整 FAT 并 sync，保留 OS/SystemOS 和 Data |
| 桌面与设置 | 47a 已进入图形界面，再开设置未复现旧故障，原始原因待记录 |
| S Pen、无线、音频、系统休眠 | 按后续驱动计划实现 |

详细状态和可勾选的后续工作见 [驱动计划](docs/DRIVERS.md)。当前硬件记录对应 `T733ZCS9DYE1/CHN` 与 FT8203 面板配置。

## 构建

首次在新目录恢复时执行 [完整重建教程](docs/REBUILDING.md)。已有匹配输入和工具后，
在设备仓库目录执行：

```sh
export UNIXNU_RECORD_HASHES=0
export UNIXNU_BASE="$(cd ../core && pwd)"
bash scripts/build.sh system-local-1
```

完整步骤、首次安装与日常更新的区别见 [构建说明](docs/BUILDING.md)。每次使用新 tag，产物位于 `build/releases/TAG/`，包括内核、菜单、含 ADT 的 FAT 和各阶段日志。该命令不写设备。

默认 `board.toml` 和 `gts7fewifi_defconfig` 构建完整系统。早期平台诊断采用 `board.platform.toml` 和 `gts7fewifi_platform_defconfig`，使用 `--platform` 选择。菜单每次从源码构建。

默认中间输出位于 `build/work/gts7fewifi/ipados-17.7.11-21H461/gts7fewifi/`；平台配置最后一级为 `gts7fewifi_platform/`。构建包装器读取官方 `status --json` 的 `output`，取得所选配置的实际目录。公开 board 提供十六进制 `chip_id = "733"`，私有 `local.toml` 提供 `hardware.unique_chip_id`，包装器将其传入官方 `UNIXNU_UNIQUE_CHIP_ID`。

## 源码位置

| 目录 | 内容 |
| --- | --- |
| `board*.toml`、`components.toml` | 两份完整 format 2 配置、社区组件身份与 archive 顺序 |
| `dts/`、`feeds.toml` | 板级 DRAM 跨度及固定 SM7325 共用配置 |
| `drivers/` | 在公共驱动基础上的设备覆盖；保留独立模块边界 |
| `kits/` | 本机需要的预链接与早期诊断工具覆盖 |
| `patches/arch/`、`patches/os/`、`patches/t733-xnu/` | 显式有序的公共来源引用、OS 与设备补丁；SoC 补丁由共用仓库提供 |
| `package/` | T733 板级服务、原始 ABI 接入与固定存储布局 |
| `scripts/` | 官方构建命令包装、输入准备与 Samsung 封装 |
| `support/` | 公共 UniXNU 基线、尚未上游的补丁与来源记录 |
| `tools/` | 主机 USB 日志采集和调用栈符号化 |
| `.cache/`、`build/` | 私有输入、编译缓存与生成物，Git 忽略 |
| `porting.toml` | 按子系统记录实现、来源和构建交付事实 |

模块覆盖、后续独立驱动仓库的划分见 [仓库结构](docs/REPOSITORY_LAYOUT.md)。现行补丁选择和输入失效处理见 [构建说明](docs/BUILDING.md)；迁移前的事实保存在 [构建记录](docs/archive/BUILDING_2026-10-24.md) 与 [上游审计](docs/UPSTREAM_AUDIT.md)。

## 输入与许可

Apple 输入固定为 `iPad7,1 / j120ap / iPadOS 17.7.11 / 21H461`。原始 Apple 系统、Samsung 固件、密钥、Data 和设备备份由使用者自行取得，清单见 [proprietary-files.txt](proprietary-files.txt)。

仓库原有许可见 [LICENSE](LICENSE)，各模块保留自己的许可与版权声明。[来源记录](support/SOURCES.md) 列出对应上游和基线。NativeUsbDebug 的原作者保留所有权利，文件头与 NOTICE 记录直接来源及缺少明确上游许可的事实；设备仓库的 BSD 许可不适用于该导入部分。
