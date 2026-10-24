# GitHub 仓库与适配分支

2026-10-24 已在 `RigoLigoRLC` 账号创建 11 个公开仓库：9 个 fork、2 个新仓。
仓库名全部使用 `unixnu-` 前缀；适配分支和默认分支统一为
`unixnu-sm-t733`。每个 description 都包含 `SM-T733` 和模块用途。
完整 description 与 fork 分支的基础提交保存在
[repositories.json](../support/repositories.json)。

| 仓库 | 来源 | 用途 |
| --- | --- | --- |
| [unixnu-sm-t733](https://github.com/RigoLigoRLC/unixnu-sm-t733) | 新建 | 设备配置、ADT、板级服务、补丁清单、构建入口及模块提交 |
| [unixnu-core](https://github.com/RigoLigoRLC/unixnu-core) | [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU) | 公共菜单、payload 扩展与构建修正 |
| [unixnu-mu-silicium](https://github.com/RigoLigoRLC/unixnu-mu-silicium) | [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) | Samsung UEFI、BootShim、MSC 与 BOOT/recovery 封装 |
| [unixnu-armcpu](https://github.com/RigoLigoRLC/unixnu-armcpu) | [UniXNU/ArmCpu](https://github.com/UniXNU/ArmCpu) | SM7325 SMP、GIC、timer 与 IPI |
| [unixnu-darwinboot](https://github.com/RigoLigoRLC/unixnu-darwinboot) | [UniXNU/DarwinBoot](https://github.com/UniXNU/DarwinBoot) | 内核装载窗口、BootVideo 与内核交接 |
| [unixnu-nativegenispi](https://github.com/RigoLigoRLC/unixnu-nativegenispi) | [UniXNU/NativeGeniSpi](https://github.com/UniXNU/NativeGeniSpi) | GENI SPI FIFO、传输与取消恢复 |
| [unixnu-nativemobiledisplay](https://github.com/RigoLigoRLC/unixnu-nativemobiledisplay) | [UniXNU/NativeMobileDisplay](https://github.com/UniXNU/NativeMobileDisplay) | CPU 帧复制、映射缓存、大核执行和完成通知 |
| [unixnu-nativeramblock](https://github.com/RigoLigoRLC/unixnu-nativeramblock) | [UniXNU/NativeRamBlock](https://github.com/UniXNU/NativeRamBlock) | 持久 Data 与显式重置处理 |
| [unixnu-nativeufs](https://github.com/RigoLigoRLC/unixnu-nativeufs) | [UniXNU/NativeUfs](https://github.com/UniXNU/NativeUfs) | SM7325 UFS、SMMU 接管与根介质 |
| [unixnu-nativeprelink](https://github.com/RigoLigoRLC/unixnu-nativeprelink) | [UniXNU/NativePrelink](https://github.com/UniXNU/NativePrelink) | XNU 预链接和存储绑定布局 |
| [unixnu-nativefocaltechtouch](https://github.com/RigoLigoRLC/unixnu-nativefocaltechtouch) | 新建 | FT8203 控制器协议；板级总线和 HID 组合留在设备仓库 |

七个已有独立来源的模块都有实际功能修改，因此均保留 fork。
EarlyEntry 属于本机可选诊断，保留在设备仓库。
NativeUsbDebug 的来源和 ARR 声明保留在设备目录，见
[SOURCES.md](../support/SOURCES.md) 和 [NOTICE](../drivers/NativeUsbDebug/NOTICE)。

## 当前远程内容

九个 fork 和两个新仓的 `unixnu-sm-t733` 已发布本机适配源码。
核心固定提交为 `63f6920d552daacd050b4af65e45d6df16cfc332`，UEFI 为
`799544f93bfffd0e7eae38efb53a08fb996d4af5`。八个模块的完整适配提交
保存在 `support/repositories.json` 的 `adaptation_commit`，设备仓库的
Git submodule 记录固定对应提交。设备仓库自身由使用者检出的 Git 提交标识。

公共 UniXNU 与配套 UEFI 本地目录的 `origin` 已指向上述个人 fork，
`upstream` 保留官方来源；设备本地目录已添加自己的 `origin`。
三个本地目录的当前分支也已统一为 `unixnu-sm-t733`。这些设置保留工作区修改。

## 源码导入顺序

- [x] ~~按实际模块差异确定仓库并完成创建、description 与分支设置。~~ 2026-10-24；上表及 `support/repositories.json`。
- [x] ~~配置现有公共源码、UEFI 和设备目录的个人远程及分支。~~ 2026-10-24；三个本地 Git 仓库的配置。
- [x] ~~将七个模块的本机修改提交到对应 fork，FT8203 提交到独立新仓。~~ 2026-10-24；`support/repositories.json` 的八项模块提交。
- [x] ~~提交公共改动并固定适配版本与官方基线。~~ 2026-10-24；`support/UniXNU.rev`、`support/UniXNU.upstream.rev` 和差异文件。
- [x] ~~将八个设备模块改为固定已发布提交的 submodule。~~ 2026-10-24；`.gitmodules` 与设备 Git tree。
- [x] ~~发布 UEFI 源码与输入获取说明。~~ 2026-10-24；`support/Mu-Silicium.rev` 固定上述固件提交。
- [x] ~~完成设备源码首次发布并记录模块提交。~~ 2026-10-24；本仓库及 `support/repositories.json`。
- [ ] 使用已发布源码完成新流程构建和实机启动记录；本次发布未构建或部署。

设备克隆和更新命令见 [BUILDING.md](BUILDING.md)。初始化设备 submodule 后，
bootstrap 从核心 fork 检出固定适配版本，固件按独立 pin 获取。
