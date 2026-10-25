# 仓库结构与上游关系

当前采用官方 UniXNU `fb5e8ba` 推荐的 format 2。ARM64、SM7325、iPadOS 21H461 与设备配置由公共构建器依次合并；普通数组由后层整体替换。`system-format2-48f` 已完成构建、FAT 部署及对应 USB 启动记录，细节见 [48f 记录](archive/FORMAT2_2026-10-25.md)。

`board.toml` 和 `board.platform.toml` 是两份完整配置，分别选择系统与平台 defconfig。`feeds.toml` 固定公开的 [unixnu-sm7325-common](https://github.com/RigoLigoRLC/unixnu-sm7325-common)；该仓库提供 `soc.toml`、共享组件表、DTSI、config fragment 和五个 SM7325 XNU 补丁。设备 `dts/board.dts` 提供 DRAM 地址跨度，共用转换器结合 board 的 ADT 属性生成载荷。

公共构建器按所选组件组装工作树。设备与 SoC 组件表保留实际仓库来源，设备同名模块优先，设备 `scripts/` 覆盖同名公共脚本。设备 `package/` 在组装树中成为 `kits/gts7fewifi`，公共菜单的 `package/` 由 core 提供。公共 `patches/xnu` 保留；SoC 的 `patches/xnu` 通过 profile 路径直接解析，避免遮盖公共补丁模块。

## 模块归属

| 内容 | 当前位置 | 独立维护方式 |
| --- | --- | --- |
| SM7325 CPU、GIC、定时器、PSCI、UFS 资源和共用内核设置 | 固定的 `unixnu-sm7325-common` feed | SoC 仓库单独版本化 |
| ArmCpu、NativeUfs、NativeGeniSpi、NativeRamBlock、NativeMobileDisplay | `drivers/` 同名 submodule 覆盖 | 各自 fork 固定适配提交 |
| DarwinBoot | `drivers/DarwinBoot` | 引导器公共项目，保留设备属性接口 |
| FT8203 协议实现 | `drivers/NativeFocaltechTouch` submodule | 独立触屏驱动；待合入 package 中的 SPI/HID 接入，设备提供板级参数和固件输入 |
| TSENS、SPMI、PMIC keys、GENI I2C、DSI | `drivers/NativeQcomTsens`、`NativeQcomSpmi`、`NativePmicKeys`、`NativeGeniI2c`、`NativeQcomDsi` submodule | 控制器操作独立维护，设备组合资源与策略 |
| USB 调试控制器和 CDC | `drivers/NativeUsbDebug` | 来源为 ios-vermeer-lab `d68e05df`；文件头与 NOTICE 记录原作者 All Rights Reserved 和未明示许可，当前保留设备内源码 |
| USB ECM ifnet | `drivers/NativeUsbNetwork` | 由设备 archive recipe 编译并接入已有 USB transport |
| 预链接、早期入口诊断 | `kits/NativePrelink` submodule、`kits/EarlyEntry` | 预链接在独立 fork 维护，早期诊断留在设备仓库 |
| XNU 适配 | 设备 ARM64/OS/功能 series、SoC series 与诊断清单 | board 显式按序选择，共用和设备构建器消费同一最终列表 |
| TLMM、EPSS、电池服务、触屏 HID | `package/` | 待按硬件职责迁移到独立驱动仓库，文件清单见 UPSTREAMING.md |
| 软件显示注册、PMGR 启动服务与启动策略 | `package/` | 设备负责组件注册、板级数据与启动组合 |
| 菜单滚动与按键、payload 附加文件和菜单项、构建恢复及禁用文件确认哈希 | `unixnu-core`；差异记录为 `support/UniXNU.patch` | 适配提交由 `support/UniXNU.rev` 固定，官方基线由 `support/UniXNU.upstream.rev` 固定 |
| Samsung UEFI、BootShim、MSC 固件修正 | 配套 UEFI 仓库 | 与 XNU 设备仓库分别发布 |

按照[官方仓库规范](https://unixnu.com/zh/contributing/repository)，每个硬件驱动必须在独立公开仓库维护，带 `unixnu` topic，并由设备 `drivers/<Name>` 子模块固定提交。上表记录当前源码位置；其中仍在 package 或设备普通目录的硬件驱动，按 [UPSTREAMING.md](UPSTREAMING.md) 的文件清单和依赖次序迁移。拆分时一起更新模块接口、调用方和构建输入，构建命令仍由一个设备入口顺序执行。

设备有 12 个 driver 子模块和 1 个 kit 子模块，共 13 个，适配分支统一为 `unixnu-sm-t733`。克隆使用 `--recurse-submodules`，更新后执行 `git submodule update --init --recursive`。来源见 [GITHUB_REPOSITORIES.md](GITHUB_REPOSITORIES.md)、[SOURCES.md](../support/SOURCES.md) 与 USB [NOTICE](../drivers/NativeUsbDebug/NOTICE)。NativeUfs、NativeMobileDisplay、NativeRamBlock 和 NativePrelink 的社区身份保留在组件表中。

## 构建文件的职责

`board.toml` 选择 SoC、OS profile、完整组件与 archive 顺序、预链接根、启动参数和菜单；`configs/` 提供 XNU 功能开关。`components.toml` 记录组件来源、依赖和 recipe，`porting.toml` 按子系统记录实现与已有证据。CI 从设备固定的 core 准备验证工具，再读取设备和 SoC 的社区组件表。

`scripts/build.py` 执行 configure、resolve、kernel、menu、wallpaper、debug-tools 和 payload；内核命令拥有 archive 编译、链接和预链接，payload 命令生成 ADT、安装确认菜单和 FAT。包装器从 `status --json` 的 `output` 取得工作目录。默认系统目录为 `build/work/gts7fewifi/ipados-17.7.11-21H461/gts7fewifi/`，平台最后一级为 `gts7fewifi_platform/`，交付目录为 `build/releases/TAG/`。

平台板在忽略的 `build/platform-device/` 中以 `board.toml` 文件名传给官方 `-D`，同时链接 DTS、组件表、feed 和私有输入。缺少 IOStorage 绑定时，入口先在独立的 `build/bindings/` 输出根构建平台内核，将内核与布局存入 `.cache/inputs/bindings/`。

私有输入经 `.cache/firmware/t733-inputs` 接入官方共享缓存。公开 board 中的十六进制 `chip_id = "733"` 与私有 `local.toml` 的 `hardware.unique_chip_id` 共同生成 ADT 身份；包装器将私有值设置到官方 `UNIXNU_UNIQUE_CHIP_ID`。安装、重启、关机和 FAT 格式由公共实现处理。

公共构建工具的本机改动已提交到 `unixnu-core`，实际构建提交由 `support/UniXNU.rev` 固定。`support/UniXNU.patch` 记录相对 `support/UniXNU.upstream.rev` 的差异。设备驱动 submodule、package 和补丁共同构成当前源码；后续公共修改同步维护适配提交、版本 pin 和差异记录。

## Git 内容

提交配置、源码、构建脚本、来源和说明文档。`.gitignore` 排除 `.cache/`、`build/`、本机 `local.toml`、Apple/Samsung 二进制输入、密钥及生成镜像。固定输入对应的名字记录在 `proprietary-files.txt`，其内容不进入 Git。

当前设备仓库为 `RigoLigoRLC/unixnu-sm-t733`；所有适配分支统一为 `unixnu-sm-t733`，本地目录名可不同。所有公共构建路径由脚本所在目录、`UNIXNU_BASE`、`--out`、`--uefi` 和显式输入决定。
