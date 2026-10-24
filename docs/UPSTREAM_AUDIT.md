# 本机修改与官方流程审计

2026-10-24。本记录先审计差异，随后按用户要求完成源码清理与上游同步。运行基线保持 `system-graphics-memory-38a`；本次未构建、测试或写入设备。

## 对照版本

- 整理前公共源码基线：`5fdbd5d08518bc2d5c196264c56a377dd80cf5ca`。
- 当前已同步的 [UniXNU main](https://github.com/UniXNU/UniXNU/tree/1a01c2afa197d975d04da892325c56b6b14aaecb)：`1a01c2afa197d975d04da892325c56b6b14aaecb`，提交时间 2026-10-23 23:24 +08:00；`support/UniXNU.upstream.rev` 固定该官方基线。`support/UniXNU.rev` 固定已发布核心 fork 的适配提交 `63f6920d552daacd050b4af65e45d6df16cfc332`。
- 本次获取的 [UniXNU-sheng main](https://github.com/UniXNU/UniXNU-sheng/tree/251b291adf941b9953fe3ae66a808b9b626e164d)：`251b291adf941b9953fe3ae66a808b9b626e164d`。
- 公共子模块已递归同步至该主仓固定的 gitlink。设备覆盖中的 NativeMobileDisplay 合并至 `8666a995`，其余六个已有独立来源的模块基线未变；具体来源见 [SOURCES.md](../support/SOURCES.md)。

## 数量和来源

整理前设备 `patches/xnu/` 共 74 个文件：相对记录的公共基线，21 个为本机新增、5 个有本机修改、48 个没有本机修改。此次已移除 48 份基线副本，将本机内容整理为 `patches/t733-xnu/`。拆分后共 28 个补丁：5 个公共文件替换、13 个功能与 USB 新增项、3 个 USB 诊断项、7 个 early 诊断项。补丁数量不能直接当作 hack 数量；例如 kalloc 的新增项只是保留原断言并补全失败参数。

`board.toml` 有 7 组用户空间二进制修补，共 17 个写点。AW 和 PosterBoard 的 2 组、3 个写点直接来自 sheng；其余 5 组、14 个写点为本机后续加入。另有 3 项停服务和 1 项 allocator 配置，共列出 11 项运行期兼容措施。它们仍在每次启动或执行相应程序时生效。

| 措施 | 数量和来源 | 当前作用与撤除条件 |
| --- | --- | --- |
| AttentionAwareness 初始化提前返回 | 1 写点，sheng 已有 | 绕过 SpringBoard 初始化的循环等待；需要修好等待依赖或采用已解决该问题的系统组件。 |
| PosterBoard 文件保护属性调整 | 2 写点，sheng 已有 | 适配没有 Apple Data Protection 的 Data；需要支持所请求的属性，或由上层正确处理缺失能力。 |
| DC ZVA 清零改为原库 STP 路径 | 1 写点，本机新增 | 绕开图形缓冲区清零异常。38a 已修 managed RT RAM 属性，但尚未撤除此写点运行原清零路径；这是优先收敛项。 |
| IconServices mode 0 | 1 写点，本机新增 | 选择 CoreGraphics 绘制、避开 CoreImage 效果；软件渲染仍需要完整可用的效果路径。 |
| CoreImage 声明 GL 后端不可用 | 1 写点，本机新增 | 避免空 GL dispatch；需要可用的 CPU、GL 或 Metal 后端及对应失败处理。 |
| CoreUI 图像和文字效果调整 | 3 写点，本机新增 | CI context 缺失时保留图像和 CoreGraphics 文字；需要原效果路径能正确产生图像。 |
| FairPlay MIG 1200..1203 超时 | 8 写点，本机新增 | 给 lockdownd 的发送和接收分别设置 5 秒超时；需要请求得到正确回复，或组件已有有限等待和错误处理。 |
| 停用 watchdogd | 1 参数，sheng 有同类措施 | 需要 SM7325 watchdog 与 IOWatchdog 用户态接口。 |
| 停用 locationd | 1 参数，其他设备有先例 | 需要解决本机位置服务等待链，并恢复服务启动。 |
| 停用 identityservicesd | 1 参数，其他设备有先例 | 需要解决 IDS 依赖等待；当前用本机独立开关控制。 |
| `kt_fixed_zones=120` | 1 参数，sheng 已有 | 让当前混合内核的类型 zone 数量落在上限内；需要合适的 allocator 默认预算。 |

本次没有找到官方最新代码能够直接替代本机新增的上述 5 组二进制修补。上游自己仍采用 AW、PosterBoard、watchdogd 和 kalloc 的措施；继承官方措施并不表示对应能力已经完整实现。

## 硬件功能与未完成的实现

以下功能必须由设备代码或接收了该实现的上游提供，系统启动成功不会消除这些职责：

- CPU/GIC/timer/SMP：SM7325 次核启动、redistributor IPI、缓存及虚拟定时器访问。
- UFS/SMMU：本机时钟和寄存器、SID/CB 接管、块读写、flush 和根卷发布。
- 持久 Data：明确重置才初始化，后端失败时阻止退化成易失 Data。这是数据保护。
- 显示：framebuffer 内存属性、IOSurface 读取、CPU present 和完成通知；大核绑定是可调整的性能策略。
- FT8203：GENI SPI、固件传输、触点与 HID。当前触控轮询应迁移到公共 TLMM 中断。
- SM5714：电量与插拔事件；GPIO54 当前中断接法应迁移公共 TLMM 分发，硬件充电策略继续补齐。
- DWC3 CDC：SM7325 时钟、SMMU、DMA、重连和日志传输；进程采样属于可选调试功能。
- DarwinBoot/ADT/平台服务：实际内存窗口、BootVideo 和设备描述需要保留。现有 PMGR 服务用于满足启动匹配，硬件电源管理仍待实现。

`cpu_machine_init` 的 `started` 修正已拆为 `native-smp-cpu-started.patch`，SMP 统计归入诊断组。managed RT 内存属性留在 `native-t733-managed-rt-memory.patch`，页表读取输出拆为诊断组的 `native-t733-page-diagnostics.patch`。

7 个早期入口、MMU、C、VM、异常、framebuffer 与 panic 诊断补丁已归入 `early-entry/`，只有 `CONFIG_XNU_EARLY_ENTRY_MARKER=y` 才应用。两个 defconfig 都保持 `n`。普通 USB 日志、进程异常、用户栈与页表诊断随 `CONFIG_XNU_NATIVE_USB_DEBUG=y` 保留，选择方式见 [XNU_PATCH_LAYOUT.md](XNU_PATCH_LAYOUT.md)。

## 官方更新已经覆盖什么

| 内容 | 官方实际状态 | 本机处理 |
| --- | --- | --- |
| Install 确认菜单 | [df43ee1](https://github.com/UniXNU/UniXNU/commit/df43ee1) 已从 `[payload.install]` 生成确认和参数选择 | 已删除自写 stage，使用官方生成。 |
| Restart / Power off | [bad2c6b](https://github.com/UniXNU/UniXNU/commit/bad2c6b) 已生成 | 直接使用官方输出。 |
| 启动参数中的路径 | [52d18ce](https://github.com/UniXNU/UniXNU/commit/52d18ce) 放行 `/` | 采用官方参数检查。 |
| 菜单可见区域随选择滚动 | 本次获取的 main 尚未实现；Menu.c 只重画旧、新选中项，Ui.c 仍按 `MenuTop + Index * RowHeight` 绘制 | 本机滚动修复仍需要，保留为独立通用补丁；之前口头判断“官方已修”已纠正。 |
| USB 主机终端 | [b9ecfdb](https://github.com/UniXNU/UniXNU/commit/b9ecfdb) 添加收集与屏幕工具 | 基础 status/replay 可复用；官方白名单没有本机 `tasks [NAME]`、`cpus`，也未提供本机常驻重连、IPC 和日志栈符号化。不能整体替换当前工具或内核 CDC。 |
| XNU 通用更新 | 签名缓存读取、union 下层目录查找、lookup 诊断、sandbox set-properties、GPU addon mount 共 5 个 patch 文件变化 | 前四项直接使用官方公共目录；GPU mount 合并到本机同名替换。设置延迟的实际改善需下轮运行记录。 |
| 7 个已有来源的驱动/kit | 主仓固定的 6 个版本未变；NativeMobileDisplay 更新至 `8666a99`，处理 GPU 复制后的 swap retire | 已合并更新，同时保留 T733 的 CPU 显示路径。 |

构建树现在保留官方 `patches/xnu/`，设备仅按文件名替换五项，并按清单添加本机补丁。`prepare-xnu-source.py` 已合入官方 GitHub token 请求支持，保留 API 下载回退。补丁集合变化时保存旧工作源码再重新生成；实际工作目录要到下一次构建才会更新。

## 构建流程的审计与处理

[官方移植指南](https://unixnu.com/porting/)明确让设备提供 `board.toml`、defconfig、驱动、ADT 和板级脚本，公共仓库提供构建及模块解析。因此设备参数留在 board 是官方流程；为了本机包装器改写公共流程的部分需要缩减。

| 本地修改 | 审计结论 | 本次处理 |
| --- | --- | --- |
| `--skip-prelink` | 官方 `build kernel` 已顺序执行编译和 prelink，各自有日志；本机拆成两步没有功能必要 | 已删除新增参数，直接调用官方 `build kernel`。 |
| `--board-overlay` 与递归 merge | 官方没有；用于本机早期平台诊断，完整系统不依赖它 | 已删除。两个完整 board 通过官方 `-D` 选择，平台目录由设备包装器生成。 |
| `{device}` 和 archive/prelink 参数展开 | 大部分可由官方构建树和相对路径完成；defconfig 的 `{device}` 没有调用者 | 已删除额外展开；archive 使用脚本名和相对 `build/...`，私有输入接入官方 firmware 缓存目录。 |
| 整份 `stage-payload.py` | 重复官方 ADT、trust/Data、persistent Data、text_fixups 和 FAT 生成 | 已删除，改用官方 `unixnu build payload`；增加通用 `files` 与 `extra_entries`，声明菜单 EFI、CPU 备用项和返回固件项。 |
| `scripts/build.py` | 顺序调用工具和归档产物有用，重新承接公共业务逻辑过多 | 已收敛为命令调用和产物归档；公共编译、预链接和 payload 由官方命令负责。 |
| Samsung recovery/BOOT/Odin sparse/tar | 本机格式和部署需求 | 保留板级装配，输入来自官方构建产物。 |
| bootstrap 缺少对象时恢复编译 | 官方仍只查 startup 记录；本地补看 link.filelist 与对象 | 单独维护为通用构建修复，不与板级路径修改混合。 |
| storage headers 条件准备 | 通用构建改进 | 保留在公共差异中，按所选存储驱动准备头文件。 |
| `UNIXNU_RECORD_HASHES=0` | 用户明确要求，官方尚未支持 | 保留，并接入官方 payload 的报告和 skip-setup 种子缓存；缓存采用路径、大小、修改时间和参数。 |

转换 payload 时已处理两个具体问题：

1. 官方只共享设备 `.cache/firmware`，本机 `.cache/inputs` 通过该目录下的 `t733-inputs` 链接接入；板级源码从 `kits/gts7fewifi` 读取。
2. 完整的平台 board 没有 `[payload.install]`，无需继承或清除系统安装项。

owned-memory、root UUID、ADT 与 text fixups 由官方 payload 从 board 读取，外层不再重复转成参数。固定 Data、容量、UUID、尾部布局和运行期兼容措施均保留。

公共仓库当前只保留七个文件的差异：菜单库三项，以及 `ipsw.py`、`kernel.py`、`payload.py`、`userdata.py`。`support/UniXNU.patch` 保存这些差异，`support/UniXNU.upstream.rev` 保存官方基线，`support/UniXNU.rev` 保存含适配的发布提交。原工作区与有本机修改的子模块在切换前分别保存到 Git stash，说明为 `T733 source before official sync 2026-10-24` 和 `T733 module before official sync 2026-10-24`；当前分支按发布命名统一为 `unixnu-sm-t733`。设备目录中的改动是后续维护入口，不应将旧 stash 整体重新应用到官方树。

## 发布前工作

- [x] ~~对照官方主仓和 sheng 当前引用，按运行措施、硬件实现、诊断与构建重复分类。~~ 2026-10-24；本记录。
- [x] ~~收敛公共 CLI 和 payload 修改，更新构建说明和 `support/UniXNU.patch`。~~ 2026-10-24；官方构建命令接入设备 `scripts/build.py`，完整配置和扩展字段见 [BUILDING.md](BUILDING.md)。
- [x] ~~同步官方 XNU 补丁与选择脚本，保留本机实际需要的修改。~~ 2026-10-24；`patches/t733-xnu/`、`scripts/build-xnu-4k-baseline.sh`、`scripts/prepare-xnu-source.py`。
- [x] ~~拆分纯诊断和功能补丁；将调试输出留为可选配置。~~ 2026-10-24；[XNU_PATCH_LAYOUT.md](XNU_PATCH_LAYOUT.md) 记录功能、USB 诊断和 early 诊断的选择。
- [x] ~~同步官方主仓固定子模块，并合并设备覆盖驱动的新增上游修改。~~ 2026-10-24；NativeMobileDisplay `8666a995`，来源见 [SOURCES.md](../support/SOURCES.md)。
- [ ] 下一次交付时完成新流程完整构建和实机启动记录。
- [ ] 先审查 DC ZVA 兼容修补的撤除条件，随后按驱动完成进度撤销停服务措施。
- [x] ~~依据最终剩余修改确定并创建所需 fork 和新仓。~~ 2026-10-24；九个 fork、两个新仓，适配分支统一为 `unixnu-sm-t733`；清单见 [GITHUB_REPOSITORIES.md](GITHUB_REPOSITORIES.md)。
- [x] ~~发布核心、UEFI、设备与八个独立模块的源码，固定 submodule 和构建提交。~~ 2026-10-24；`support/repositories.json`、三个版本 pin 与 `.gitmodules`。

源码收敛已完成。重新构建、设备部署和剩余运行期兼容措施的撤除留在上述未完成项中。
