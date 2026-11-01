# GPU 上游迁移 74

## 目标与基线

用户要求将 SM-T733 的现有适配迁移到上游重构后的 GPU 实现及更新后的移植文档。
本轮更新 `unixnu-sm-t733`；Apple3 分支保留原固定版本。

| 仓库 | 迁移前 | 本轮上游基线 |
| --- | --- | --- |
| core | `513f6a208026b0386635d666b4e9f6c61f364f56` | `f0e7d52` |
| Adreno6xx | `86e0f1bd9afb3471b82ceac11fc062426af674a2` | Adreno740 `a9a2cbb43b89d672e2921e19721a6438b738b17f` |
| AdrenoMetal | `c31037dc6e01593ba93a8973a488a88b6188b349` | `831a9dcbd6ac7de89a11bdaf031e07b9aadfa3c8` |
| ArmSmmu4K | `599ba82f96f3e10b0afbd784c040127d38e2683a` | `83969cd874503e65997ff7639f7dcf3d0a92cd17` |

core 的旧提交保存在 `backup/unixnu-sm-t733-before-20261101`；GPU 模块使用
`backup/unixnu-sm-t733-before-upstream-20261101`。新的提交从上游基线承接本机功能，
原始构建 tag 和已交付产物保持各自原有源码身份。

本轮按固定源码中的以下文档实施：

- [GPU 与 Metal](https://github.com/UniXNU/UniXNU/blob/f0e7d52/docs/zh/porting/gpu.md)
- [设备迁移](https://github.com/UniXNU/UniXNU/blob/f0e7d52/docs/zh/porting/migration.md)
- [GPU 运行路径](https://github.com/UniXNU/UniXNU/blob/f0e7d52/docs/zh/drivers/adreno740.md)

## 公共构建与设备输入

公共构建合并上游 OS profile、预链接候选输出、GPU 构建和固件打包更新，并保留
SM7325 资源转换、设备菜单、固定 Data 种子、私有身份及完整补丁清单。
`UNIXNU_RECORD_HASHES=0` 时，Turnip 使用承载 Mesa 固定版本与补丁集的 core 提交号
作为 pipeline cache 标识。同一提交的重复构建复用标识，core 提交变化会更新标识。

Metal 构建按模块 `bridge/patches/series` 顺序应用补丁。公共脚本保留上游
`NMB_SCANOUT_AIR` 与 `ios27` 构建入口，SM-T733 继续选择 iPadOS 17 的 ARM64 ABI。
设备构建向 Metal 准备工具传入选定 OS 镜像；工具收集镜像内全部 `.metallib` 的目标路径，
写入 `build/prime-list.txt`，由插件构建器嵌入 `__DATA,__primelist`。

## Metal 与 Turnip

AdrenoMetal `ac86d45f1d1d1d412f28ba3a68c31d33ba4e4021` 以 `831a9dc` 为父提交。
其运行时补丁顺序为上游 24 层加本机 9 层，保留上游异步录制队列、缓存预热、阶段记录、
scanout、函数链接和 iOS27/PAC 接口。本机资源声明和屏障通过相同录制队列执行，
相关句柄列表复制后留存到录制任务使用完毕；硬件一致性、采集、复制合并和 memoryless
录制功能随运行时补丁迁移。

Turnip 继续使用固定 Mesa `85fda3b36405bcaea8bffc64aa4f2651dc90770d`，补丁清单为
26 层。上游 GMEM 字节序修正、优先级与 Note 接口、pass/submit 记录接入本机 V2
异步提交。内存补丁加入两个地址红黑树、系统内存压力通知、空闲回收和延迟 BO 分配，
同时保留本机尺寸分级复用、缓存预算和回收线程。

## 实施与交付状态

源码提交：core `799255e`、Adreno6xx `65cfc8e`、ArmSmmu4K `b49b0ea`、
Metal `ac86d45`、SM7325 `5d9714c`。core 与 GPU 模块已发布到
`rebase/unixnu-sm-t733-20261101`，SoC 保持 `unixnu-sm-t733`。

Adreno6xx 将每连接 VM、ASID、动态页表、A642L 四参数 CP 页表切换、跨 VM 物理页关联
及连接关闭后的 fence 回收贯通。优先级使用就绪队列选择，250 ms 防饥饿；
A740 双 ring 抢占依赖 A7xx restore/SMMU 记录，SM7325 当前启动未提供这些记录，
因此本轮未启用。新增方法为 SetPriority 43 与 Note 44，保留原有 1 到 42 的请求编号。

## 构建记录

构建命令使用工作区统一入口：

```sh
source tools/activate.sh
bash tools/build-t733-recovery.sh TAG --system --payload-only \
  --local-module soc:sm7325=/Users/rigoligo/expm/unixnu-sm-t733/sources/unixnu-sm7325-common
```

| TAG | 已保存结果 |
| --- | --- |
| `system-gpu-upstream-74` | 配置解析成功；主机沙箱拒绝 `sysctl`，XNU installhdrs 失败。 |
| `system-gpu-upstream-74a` | 使用相同源码并允许主机构建权限；完成 XNU 编译记录和 GPU 之前的依赖归档，GPU 编译因 OS17 缺少 `task_get_gpu_role` 及优先级常量声明而退出。 |
| `system-gpu-upstream-74b` | 改用 OS17 任务策略接口；其内部 C 头文件中的 `template` 参数名被 C++ 编译器拒绝。74c 将读取操作移入独立 C 文件，配方使用 XNU 的 C 编译记录。 |
| `system-gpu-upstream-74c` | 内核、预链接、菜单与 Turnip 编译成功；OS 镜像未挂载，Metal 预热列表生成停止。 |
| `system-gpu-upstream-74d` | 只读恢复已有 OS 镜像后使用相同源码，九阶段全部返回 0；166 个 Metal 库路径嵌入插件，完整 FAT 写入及 sync 成功。 |
| `system-gpu-upstream-74e` | 修正 lazy transient image 资源登记后，九阶段全部返回 0；完整 FAT 写入及 sync 成功。 |

每轮固定 tag 为 `build/TAG-source-1`，原 tag 保留。工作区
`logs/gpu-upstream-74d-source-revisions.json` 与交付目录 `source-revisions.json`
记录成功包的输入；设备 `build/releases/TAG/logs/` 保存各轮阶段日志。

## 74d 交付

- 源码：device `039f160`、core `799255e`、SM7325 `5d9714c`、GPU `65cfc8e`、
  Metal `ac86d45`、SMMU `b49b0ea`。
- 固定 tag：`build/system-gpu-upstream-74d-source-1`，六个仓库均已发布。
- 产物：`build/releases/system-gpu-upstream-74d/payload.img`，536870912 字节。
- 部署：2026-11-01 13:59，重新枚举 MSC 后写入固定启动 FAT，完整写入及 sync 返回成功。
  日志为工作区 `logs/msc-20261101-135901-system-gpu-upstream-74d/`。
- 被动 USB：工作区 `logs/t733-usb-live/`，采集进程 25918 已启动。

首次启动已进入 launchd、backboardd 和 SpringBoard，用户观察到停在苹果标。
GPU CP scratch 成功，已发出的 17 个任务全部完成，faults 和 timeouts 均为 0。
backboardd 的 completion 5 已接受但尚未发出，记录包含一个等待信号和一个 DPU
读取者；后续桌面任务的 GPU 依赖逐渐累积。显示帧的 producer_ready 为 0，
CPU transfer 返回 `e00002d6`。首帧 fence 8 已完成并执行一次 DPU 切换；后续任务因
DeviceAddress 回退请求 SubmitAllMapped，将连接内全部映射标为读写，因而等待
DPU 正在读取的前帧页。DPU 等待新帧完成后才释放前帧，两者形成相互等待。
触发入口是内存懒分配的 transient image：它先保留 IOVA，使用 GMEM 时尚无 BO；
原有资源登记无条件提交完整地址范围，找不到 BO 后进入 DeviceAddress 回退。
修复将资源登记接到 GMEM/SYSMEM 选择及实际 BO 分配之后。帧记录中的
`gpu_status=e00002c7` 是尚未执行元数据查询时的初始值。
本次 USB 原始日志为工作区
`logs/t733-usb-live/t733-20261101T060100.741857Z.raw`，包含 gpu、display 和
tasks backboardd 现场，快照保存在 `logs/gpu-upstream-74d-boot-failure/`。
被动采集继续运行。

## 74e 修复与交付

core `7961ae25c296ec6048d350a9741cf7dc4ce220f5` 修正 Turnip 的三个登记入口。
`tu_cmd_use_image` 和 descriptor resource 收集跳过尚无 BO 的 transient image；
`tu_allocate_transient_attachments` 在 GMEM/SYSMEM 决策和实际 BO 分配后，
登记所需读写范围，包含其他录制路径已分配的 lazy BO。实际未知地址继续使用
原有回退，GPU 与 DPU 的读取保护保持启用。其余组件沿用 74d 的固定提交。

设备输入为 `fd08c9a1b2ae6da2a66439b55700f0a55d2d9b07`，六仓固定 tag 为
`build/system-gpu-upstream-74e-source-1`，均已发布。统一完整构建九阶段返回 0；
2026-11-01 14:12 经 MSC 写入 536870912 字节 FAT 并完成 sync。部署记录位于工作区
`logs/msc-20261101-141207-system-gpu-upstream-74e/`，源码清单为
`logs/gpu-upstream-74e-source-revisions.json` 和交付目录 `source-revisions.json`。
被动 USB 采集继续运行，已通知用户完整重启固件并选择 Boot UniXNU。

首次启动恢复桌面渲染。原来阻塞的 completion 5 已派发到 hardware fence 9，
返回 status 0。GPU 已发出 150 个任务并全部完成，faults/timeouts/poisoned 均为 0；
150 条资源记录的 fallback 全为 0。367 条显示完成记录均为 status 0，
DPU frame 推进到 5314。元数据查询有 4 条 `e00002d6`，对应显示完成仍为 0；
最后一帧元数据查询和显示完成均成功。

本次 raw 为 `logs/t733-usb-live/t733-20261101T061427.910351Z.raw`。
首次启动和 gpu 查询快照保存在 `logs/gpu-upstream-74e-boot/`，
其中 `frame-summary.json` 分别记录显示完成和元数据查询状态。

用户随后报告 74e 卡顿没有明显改善，红蓝通道异常扩大到壁纸和图标。
上述提交与显示完成记录说明任务可以推进；本次颜色回归继续调查新增 GMEM
通道交换补丁在 A642L 上的格式处理。现场保存在工作区 `logs/gpu-color-75/`。
