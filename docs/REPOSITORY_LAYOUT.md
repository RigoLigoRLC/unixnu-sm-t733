# 仓库结构与上游关系

2026-10-24 已根据[上游审计](UPSTREAM_AUDIT.md)收敛公共 CLI 扩展与 FAT 装配，并同步官方 UniXNU `1a01c2a` 及其固定子模块。独立 fork 按剩余模块改动确定。

本仓库采用 [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) 的设备目录约定。UniXNU 根据设备 `board.toml`、内核 defconfig 与模块目录组装工作树。设备中的同名 `drivers/`、`kits/`、`patches/` 优先于公共仓库；设备 `scripts/` 覆盖同名公共脚本。设备 `package/` 在组装树中成为 `kits/gts7fewifi`，公共 UniXNU 菜单的 `package/` 仍由公共仓库提供。

## 模块归属

| 内容 | 当前位置 | 独立维护方式 |
| --- | --- | --- |
| ArmCpu、NativeUfs、NativeGeniSpi、NativeRamBlock、NativeMobileDisplay | `drivers/` 同名 submodule 覆盖 | 各自 fork 固定适配提交 |
| DarwinBoot | `drivers/DarwinBoot` | 引导器公共项目，保留设备属性接口 |
| FT8203 协议实现 | `drivers/NativeFocaltechTouch` submodule | 独立协议驱动；板级总线、固件和 HID 连接由设备配置选择 |
| USB 调试控制器和 CDC | `drivers/NativeUsbDebug` | 来源为 ios-vermeer-lab `d68e05df`；文件头与 NOTICE 记录原作者 All Rights Reserved 和未明示许可，当前保留设备内源码 |
| 预链接、早期入口诊断 | `kits/NativePrelink` submodule、`kits/EarlyEntry` | 预链接在独立 fork 维护，早期诊断留在设备仓库 |
| XNU 适配 | `patches/t733-xnu/`、构建和准备源码脚本 | 保留官方 `patches/xnu/`，只增加本机补丁及逐文件替换；选择规则见 [XNU_PATCH_LAYOUT.md](XNU_PATCH_LAYOUT.md) |
| T733 电池服务、触屏 HID、软件显示连接、PMGR 与启动策略 | `package/` | 留在设备仓库，依赖本机板级数据 |
| 菜单滚动与按键、payload 附加文件和菜单项、构建恢复及禁用文件确认哈希 | `unixnu-core`；差异记录为 `support/UniXNU.patch` | 适配提交由 `support/UniXNU.rev` 固定，官方基线由 `support/UniXNU.upstream.rev` 固定 |
| Samsung UEFI、BootShim、MSC 固件修正 | 配套 UEFI 仓库 | 与 XNU 设备仓库分别发布 |

独立驱动仓库有利于多个设备复用和独立提交。设备仓库以 Git submodule 固定其提交，或使用 UniXNU 的 `feeds.toml` 固定来源。拆分时保持模块名、路径和接口，构建命令仍由一个设备入口顺序执行。

官方 sheng 的 [`.gitmodules`](https://github.com/UniXNU/UniXNU-sheng/blob/main/.gitmodules) 直接引用 `NativeNovatekTouch` 和 `ShengPreflight`，其他公共驱动从 UniXNU 基线取得。T733 采用同样的归属原则：已经存在独立上游的驱动通过 fork 承载本机改动；新协议驱动在接口独立后单独维护；板级配置与组合代码保留在设备仓库。

2026-10-24 已发布九个 fork 和两个新仓的适配源码，清单见 [GITHUB_REPOSITORIES.md](GITHUB_REPOSITORIES.md)。七个官方来源模块与 FT8203 协议驱动通过八个 submodule 固定实际适配提交，分支统一为 `unixnu-sm-t733`。没有修改的公共驱动继续使用固定 UniXNU 核心提供的模块。克隆设备仓库时使用 `--recurse-submodules`，更新后执行 `git submodule update --init --recursive`。

2026-10-24 已按官方主仓 `1a01c2a` 固定版本合并 NativeMobileDisplay `c031d882` 至 `8666a995` 的修改：GPU 接收帧后可提前完成 swap，刷新属性支持固定间隔。T733 的 CPU 拷贝、源映射缓存、大核绑定和完成通知保留。其余六个已有来源的模块版本与该官方主仓一致，具体提交和许可证见 [SOURCES.md](../support/SOURCES.md)。USB 的直接来源及原作者权利另见 [NOTICE](../drivers/NativeUsbDebug/NOTICE)。

## 构建文件的职责

`board.toml` 选择 Apple 组件、驱动归档顺序、预链接根、启动参数和菜单；`configs/` 提供 XNU 功能开关。默认配置为完整系统，`board.platform.toml` 是完整的平台配置。平台入口将其放入忽略的设备目录，通过官方 `-D` 选择。

`scripts/build.py` 顺序调用官方 `defconfig`、`build kernel`、`build package` 和 `build payload`。内核命令拥有编译与预链接；payload 命令生成 ADT、安装确认菜单和 FAT。板级包装器归档产物，并按选项调用 Samsung 固件封装。缺少 IOStorage 绑定输入时，先以平台配置生成绑定内核和布局；这两个文件保存在 `.cache/inputs/bindings/`，后续完整系统构建复用。编译中间文件放在 `build/work/`，按 tag 的交付放在 `build/releases/`。

公共 CLI 保持官方语义，不再提供 `--skip-prelink`、`--board-overlay`、递归 board 合并或额外的 `{device}` 参数展开。本机输入经 `.cache/firmware/t733-inputs` 接入官方共享缓存；新增 payload 配置只声明需要复制的文件和备用菜单项，安装、重启、关机与 FAT 格式由官方实现。

公共构建工具的本机改动已提交到 `unixnu-core`，实际构建提交由 `support/UniXNU.rev` 固定。`support/UniXNU.patch` 记录相对 `support/UniXNU.upstream.rev` 的差异。设备驱动 submodule、package 和补丁共同构成当前源码；后续公共修改同步维护适配提交、版本 pin 和差异记录。

## Git 内容

提交配置、源码、构建脚本、来源和说明文档。`.gitignore` 排除 `.cache/`、`build/`、本机 `local.toml`、Apple/Samsung 二进制输入、密钥及生成镜像。固定输入对应的名字记录在 `proprietary-files.txt`，其内容不进入 Git。

当前设备仓库为 `RigoLigoRLC/unixnu-sm-t733`；所有适配分支统一为 `unixnu-sm-t733`，本地目录名可不同。所有公共构建路径由脚本所在目录、`UNIXNU_BASE`、`--out`、`--uefi` 和显式输入决定。
