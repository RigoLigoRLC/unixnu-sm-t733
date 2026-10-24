# UniXNU-SM-T733

Samsung Galaxy Tab S7 FE Wi-Fi (`SM-T733`, `gts7fewifi`, Qualcomm SM7325) 的 UniXNU 设备仓库。

GitHub 入口为 [RigoLigoRLC/unixnu-sm-t733](https://github.com/RigoLigoRLC/unixnu-sm-t733)。设备、公共核心、UEFI 与八个独立模块的适配源码已发布；模块仓库与固定提交见 [GitHub 仓库清单](docs/GITHUB_REPOSITORIES.md)。

结构参照 [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) 和 [官方移植指南](https://unixnu.com/porting/)。公共构建入口使用 [UniXNU](https://github.com/UniXNU/UniXNU)，Samsung UEFI 保持为独立配套仓库。当前目录名 `UniXNU-gts7fewifi` 是本地工作目录，发布名称采用 `UniXNU-SM-T733`。

## 实机状态

实机基线为 2026-10-24 的 `system-graphics-memory-38a`。源码已同步官方 UniXNU `1a01c2a` 及其固定子模块，并完成构建流程收敛；本次整理尚未执行完整重建或设备部署。

| 功能 | 状态 |
| --- | --- |
| UEFI、UniXNU 菜单、XNU 启动 | 可用，菜单可滚动 |
| 八核、GIC、架构定时器 | 已实机运行 |
| UFS 系统卷与持久 Data | 可启动桌面，96 GiB Data 跨重启保留 |
| 显示 | CPU 软件渲染与帧复制可用，GPU 加速未接入 |
| FT8203 触屏 | 基础点击、滑动可用，中断与休眠恢复待完善 |
| SM5714 电量计 | 电量、电压、充电状态可读，插拔中断即时更新 |
| USB CDC 调试 | 日志、状态和调用栈采样可用 |
| MSC | 可更新 FAT，固件端仍有 endpoint 恢复问题 |
| 桌面与设置 | 已进入桌面；设置有较长加载延迟；图库返回桌面正常 |
| 物理按键、S Pen、无线、音频、系统休眠 | 按后续驱动计划实现 |

详细状态和可勾选的后续工作见 [驱动计划](docs/DRIVERS.md)。当前硬件记录对应 `T733ZCS9DYE1/CHN` 与 FT8203 面板配置。

## 构建

在设备仓库目录执行：

```sh
git clone --recurse-submodules --branch unixnu-sm-t733 https://github.com/RigoLigoRLC/unixnu-sm-t733.git
cd unixnu-sm-t733
export UNIXNU_RECORD_HASHES=0
bash scripts/bootstrap-base.sh ../UniXNU
export UNIXNU_BASE="$(cd ../UniXNU && pwd)"
cp local.toml.example local.toml
# 按 docs/BUILDING.md 准备本机固件、签名策略、trust cache 和固定 Data 种子。
bash scripts/build.sh system-local-1
```

完整步骤、首次安装与日常更新的区别见 [构建说明](docs/BUILDING.md)。每次使用新 tag，产物位于 `build/releases/TAG/`，包括内核、菜单、含 ADT 的 FAT 和各阶段日志。该命令不写设备。

默认 `board.toml` 和 `gts7fewifi_defconfig` 构建完整系统。早期平台诊断采用 `board.platform.toml` 和 `gts7fewifi_platform_defconfig`，使用 `--platform` 选择。菜单每次从源码构建。

## 源码位置

| 目录 | 内容 |
| --- | --- |
| `drivers/` | 在公共驱动基础上的设备覆盖；保留独立模块边界 |
| `kits/` | 本机需要的预链接与早期诊断工具覆盖 |
| `patches/t733-xnu/` | 本机 XNU 新增、逐文件替换及可选诊断；公共补丁由官方树提供 |
| `package/` | T733 板级服务、ADT 输入与存储布局 |
| `scripts/` | 官方构建命令包装、输入准备、ADT 和 Samsung 封装 |
| `support/` | 公共 UniXNU 基线、尚未上游的补丁与来源记录 |
| `tools/` | 主机 USB 日志采集和调用栈符号化 |
| `.cache/`、`build/` | 私有输入、编译缓存与生成物，Git 忽略 |

模块覆盖、后续独立驱动仓库的划分见 [仓库结构](docs/REPOSITORY_LAYOUT.md)。本轮清理结果和仍需保留的运行期修补见 [上游审计](docs/UPSTREAM_AUDIT.md)，补丁选择见 [XNU 补丁说明](docs/XNU_PATCH_LAYOUT.md)。

## 输入与许可

Apple 输入固定为 `iPad7,1 / j120ap / iPadOS 17.7.11 / 21H461`。原始 Apple 系统、Samsung 固件、密钥、Data 和设备备份由使用者自行取得，清单见 [proprietary-files.txt](proprietary-files.txt)。

仓库原有许可见 [LICENSE](LICENSE)，各模块保留自己的许可与版权声明。[来源记录](support/SOURCES.md) 列出对应上游和基线。NativeUsbDebug 的原作者保留所有权利，文件头与 NOTICE 记录直接来源及缺少明确上游许可的事实；设备仓库的 BSD 许可不适用于该导入部分。
