# 驱动仓库归属与贡献范围

本项目按 [UniXNU 仓库布局](https://unixnu.com/zh/contributing/repository)维护源码。2026-10-24 已读取该指南并清点本机源码。每个硬件驱动由独立公开仓库维护，设备以 `drivers/<Name>` 子模块固定提交；内核库放在独立 `kits/<Name>` 子模块。仓库引用前添加 `unixnu` topic。

SM-T733 驱动由 `RigoLigoRLC` 维护。涉及官方机型或通用栈的独立改动可以整理后贡献到 UniXNU 组织；设备参数、实验性实现与本机兼容处理在维护者仓库发布。设备适配完成后整理设备页面和移植记录。每个提交只处理一个主题，使用 `feat(<area>)`、`fix(<area>)`、`build(<area>)`、`docs` 或 `chore`。

## 已有仓库

`ArmCpu`、`DarwinBoot`、`NativeGeniSpi`、`NativeMobileDisplay`、`NativeRamBlock`、`NativeUfs`、`NativeFocaltechTouch`、`NativeQcomTsens`、`NativeQcomSpmi`、`NativePmicKeys`、`NativeGeniI2c`、`NativeQcomDsi` 已有独立公开仓库和设备子模块，十二个驱动仓库均带 `unixnu` topic。加上 `kits/NativePrelink`，设备共注册十三个 driver/kit 子模块。实际仓库与已发布提交见 [GITHUB_REPOSITORIES.md](GITHUB_REPOSITORIES.md) 和 [repositories.json](../support/repositories.json)。工作区中的后续功能修改仍随各模块交付单独发布。

2026-10-24 新驱动发布及职责如下。适配和默认分支均为 `unixnu-sm-t733`，description 包含 SM-T733，设备固定表中实际提交。

| 驱动 | 已发布提交 | 模块与设备的职责 |
| --- | --- | --- |
| `NativeQcomSpmi` | `ce3ccf0` | 模块处理 v5/v7 arbiter、事务串行化、APID 所有权及 PDC 分发；设备传入 MMIO、EE、PDC pin 和 INTID。 |
| `NativePmicKeys` | `7469357` | 模块读取 PMIC 实时状态、去抖并提交完整 pressed mask；设备配置三个物理键及原始 IOHID Consumer 服务。 |
| `NativeGeniI2c` | `c9185a7` | 模块处理 FIFO 命令、错误恢复和可选开漏事务；设备准备 SE10 时钟、引脚并写入 MAX77816 电压。 |
| `NativeQcomDsi` | `d486690` | 模块处理 DSI 命令 FIFO、完成等待和视频模式调度；设备提供访问函数、面板 DCS、亮度和电源顺序。 |

## 待迁移源码

45c 已按新驱动规则直接实现并发布 `NativeQcomTsens`，仓库为 `RigoLigoRLC/unixnu-nativeqcomtsens`，提交 `8b2fb5e`，适配/默认分支 `unixnu-sm-t733`，topic 为 `unixnu`。设备工作区通过 `drivers/NativeQcomTsens` 子模块接入；通用驱动只读取参数指定的 TSENS 寄存器，板级传感器分组和 CPU 限频留在 `package/T733Thermal.cpp`。下表为更早硬件实现的迁移任务。

下表的新仓名称为迁移方案，尚未创建。所有新仓使用 `unixnu-sm-t733` 适配及默认分支，description 写明 SM-T733 和实际硬件职责。

| 次序 | 当前源码 | 独立仓库与设备路径 | 迁移时一起处理的调用关系 |
| --- | --- | --- | --- |
| 1 | `package/T733Tlmm.cpp`、`T733Tlmm.h` | 新建 `unixnu-nativetlmm`，`drivers/NativeTlmm` | TLMM MMIO、INTID240 分发、GPIO 屏蔽和重启中断归驱动；电池和触屏通过同一头文件调用。`scripts/build-battery.py` 当前顺带编译 TLMM，迁移时改为单独归档并在 board 链接一次。 |
| 2 | `package/T733CpuPower.cpp`、`T733CpuPower.h`、`T733CpuPowerStartup.c` | 新建 `unixnu-nativeqcomepss`，`drivers/NativeQcomEpss` | EPSS 三域寄存器、LUT 与频率切换归驱动；设备传入域地址和 CPU 对应关系。同步 `scripts/build-cpu-power.py`、USB `dvfs` 调用和 XNU 负载读取接口。 |
| 3 | `package/T733FuelGauge.cpp`、`T733FuelGauge.h`、`T733Battery.cpp`、`T733BatteryStartup.c` | 新建 `unixnu-nativesm5714`，`drivers/NativeSm5714` | SM5714 电量计、充电/MUIC 状态与 IOPMPowerSource 发布作为同一硬件驱动；GPIO12/13 软件 SMBus 经 NativeTlmm 接口访问，GPIO54 IRQ 依赖先迁出的 TLMM。同步 `scripts/build-battery.py`，设备提供地址和引脚。 |
| 4 | `package/T733Touch.cpp`、`T733TouchHid.cpp`、`T733TouchHidABI.h`、`T733TouchStartup.c` | 扩展现有 `unixnu-nativefocaltechtouch` 与 `drivers/NativeFocaltechTouch` | FT8203 协议、GENI SPI 适配、IRQ 工作线程和 HID 事件属于同一触屏驱动；依赖 NativeGeniSpi 与 NativeTlmm。同步 `scripts/build-touch-input.py`。设备保留 GPIO/SE14 参数、固件输入声明和引用忽略固件的 `T733TouchFirmware.S`。 |
| 5 | `drivers/NativeUsbDebug/` | 新建 `unixnu-nativeusbdebug`，保持当前设备路径 | DWC3/CDC 及其配套日志接口一起交付；`scripts/build-xnu-4k-baseline.sh` 的源码复制和 XNU 调用点保持一致。保留 NOTICE、源文件作者声明、直接来源和来源提交。 |
| 6 | 正在实施的 `drivers/NativeUsbNetwork/` | 新建 `unixnu-nativeusbnetwork`，保持当前设备路径 | NativeUsbNetwork 使用 C KPI 实现 XNU ifnet 适配，通过 C 接口收发帧；NativeUsbDebug 独占 DWC3 控制器、端点、ECM 枚举和传输。先完成本轮网络包构建和部署，再固定两个 USB 模块提交。 |

`NativeUsbDebug` 直接来源为 [ios-vermeer-lab](https://github.com/xyyyds0106/ios-vermeer-lab)，固定来源 `d68e05df683f863f054b0f9c063c54d2b1342b2a`。迁移原样保留 ARR 说明，并更新 NOTICE 中到设备来源文档的链接；具体声明见现有 [NOTICE](../drivers/NativeUsbDebug/NOTICE) 和 [SOURCES.md](../support/SOURCES.md)。

`package/T733Display` 的 Apple 组件注册、面板电源组合、`ShengPlatformPower.cpp` 的启动服务、`BootWatchdogPolicy.c` 的用户空间启动策略及板级数据继续由设备组合。`BootLog.c` 和 `kits/EarlyEntry` 是诊断代码；独立复用时按 kit 整理。SPMI、按键、GENI I2C 和 DSI 已按上述边界建立独立仓库。硬件 watchdog 等后续新实现直接建立对应驱动仓库，参考 plist 和硬件寄存器操作放入该驱动。

## 迁移交付步骤

先由协调者与当前源码负责人确认具体文件和归档接口，再执行一个驱动的完整迁移。共享总线和芯片模块按实际硬件责任命名；一个芯片的协议、事件转换和 IOKit 接入可以共同维护在该驱动仓库。新仓提交源码、来源和参考 plist，添加 topic 并推送适配分支后，设备把原路径换成固定该实际提交的子模块。同一次设备修改更新 `.gitmodules`、`support/repositories.json`、构建输入、board 归档顺序和来源文档；硬件参数通过设备配置或明确的初始化参数交给驱动。保留已有模块接口名称直到调用方和声明能一起修改。

本轮 USB ECM 和 debugserver 的构建部署优先。当前源码没有为仓库整理而移动；本文件记录的迁移在交付后按上述顺序实施。模块源码先发布，设备再更新提交引用，公共构建变更继续同步 `support/UniXNU.rev` 与 `support/UniXNU.patch`。

- [x] ~~读取官方仓库规范、清点现有驱动并给七个已发布驱动仓库添加 topic。~~ 2026-10-24；本文件和远程仓库设置。
- [x] ~~明确 package 中硬件驱动的独立仓库和迁移依赖。~~ 2026-10-24；上表。
- [x] ~~发布 NativeQcomSpmi、NativePmicKeys、NativeGeniI2c 和 NativeQcomDsi，并接入固定提交的设备子模块。~~ 2026-10-24；`ce3ccf0`、`7469357`、`c9185a7`、`d486690`，各模块 NOTICE/LICENSE、`.gitmodules` 和 `support/repositories.json`。
- [ ] 完成表中驱动迁移、模块发布和设备子模块引用。
- [ ] 将适合通用栈的修改逐项整理，确定贡献目标并提交对应改动。
