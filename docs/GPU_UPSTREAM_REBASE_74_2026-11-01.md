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

源码提交：core `799255e`、Adreno6xx `757704d`、ArmSmmu4K `b49b0ea`、
Metal `ac86d45`、SM7325 `157b453`。core 与 GPU 模块已发布到
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

每轮固定 tag 为 `build/TAG-source-1`，原 tag 保留。工作区
`logs/gpu-upstream-74-source-revisions.json` 与 `logs/gpu-upstream-74a-source-revisions.json`
记录输入；设备 `build/releases/TAG/logs/` 保存阶段日志。本轮尚未生成可部署系统包。
