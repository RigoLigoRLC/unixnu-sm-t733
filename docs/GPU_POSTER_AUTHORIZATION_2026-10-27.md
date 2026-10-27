# 56b Poster GPU 授权故障与修复

## 结论

56b 操作窗口内，KaleidoscopePoster 和 AegirPoster 首先在打开 NativeAdreno
用户客户端时被原 Sandbox 拒绝。每个名称的前八次打开均已创建并释放客户端；
之后又触发驱动按开机累计打开次数实施的上限。两者报告 Vulkan 设备初始化失败。
Kaleidoscope 随后向空的 Metal buffer contents 写入常量，产生 SIGSEGV。

本次修复把原始签名中的 `com.apple.security.ts.opengl-or-metal=true` 接入
NativeAdreno 的特定连接授权。进程名称不参与新增授权；原有 backboardd 和
iconservicesagent 服务授权保持原行为。连接数量的并发限制和关闭计数由同轮
Adreno 用户客户端生命周期修改处理。

## 当次日志与时间范围

证据来自工作区 `logs/t733-usb-live/t733-20261027T004806.720902Z.raw`，
时间取相邻的 `boot-events.jsonl`。以下均为 UTC：

| 进程 | 异常退出 | 首次到末次 | 同类退出平均间隔 |
| --- | --- | --- | --- |
| KaleidoscopePoster | 12 次 SIGSEGV | 00:55:06.944299 至 00:55:25.252399 | 1.664 秒 |
| AegirPoster | 15 次 SIGABRT | 00:55:08.035574 至 00:55:31.095837 | 1.647 秒 |

两者合计在约 24.15 秒内退出 27 次。`watch-status.json` 中的 fault_count=41
包含异常与退出的分别记账，不能作为独立崩溃次数。日志多次出现
`Too many corpses being created`。

raw 第 6814、6816 行记录：

```text
client=AegirPoster opens=8 refused=7 closes=8 slot=255 peak_mib=0 last_refusal=e00002d5
client=KaleidoscopePoster opens=8 refused=4 closes=8 slot=255 peak_mib=0 last_refusal=e00002d5
```

第 6821、6822 行均为 `no-device device: Vulkan(VkResultCode(-3))`。
每个进程此前各有八次 `IOUC AGXDeviceUserClient failed MACF`。
`0xe00002d5` 是 `kIOReturnBusy`。当时 mount_reason、mount_status 和
mount_failures 均为零。`opens=closes=8`、slot=255 和 peak_mib=0 对应已经
关闭的失败连接；本窗口没有客户端槽未释放的证据。

raw 第 4808 与 6776 行的汇总保持相同：T pipeline=111/13575ms，
V vkpipe=116/8002ms，V cache hit=0、miss=225、wr=225。因此本次反复崩溃
期间没有持续创建 pipeline。反复创建进程、失败初始化和处理崩溃会增加 CPU
及系统服务工作，能够解释该时间段的一部分卡顿；现有被动日志没有把每次手势
停顿与这些退出逐个配对。

本次运行源码身份记录在工作区 `logs/gpu-memory-56b-source-revisions.json`：
Adreno6xx `5b1510856f4bbc876f5551f2b2eefcab8f94301b`，
core `e0a8997907a5d7c9026a4025ae961683fc96e77a`，
device `bc90d201aae4993f51c80ea92cf4b9cfb77cd9f8`。

## 原始二进制与调用契约

两个原始可执行文件均包含 `com.apple.security.ts.opengl-or-metal=true` 和
`com.apple.posterkit.provider=true`。来源使用 `codesign -d --entitlements :-`
只读提取：

- `out/t733-system-inputs/os-mount/System/Library/PrivateFrameworks/WatchFacesWallpaperSupport.framework/PlugIns/KaleidoscopePoster.appex/KaleidoscopePoster`
- `out/t733-system-inputs/os-mount/System/Library/CoreServices/AegirProxyApp.app/PlugIns/AegirPoster.appex/AegirPoster`

原始 IOGPU 映像位于
`out/t733-lldb-symbols/extracted/System/Library/PrivateFrameworks/IOGPU.framework/IOGPU`。
`_IOGPUDeviceCreateWithAPIProperty` 起点为 `0x1fcb2f8c8`：

```text
0x1fcb2f8f8  mov w8, #1
0x1fcb2f8fc  bfi w8, w2, #16, #16
0x1fcb2f910  mov x2, x8
0x1fcb2f914  bl 0x20035f288
```

该 stub 的 ADRP/ADD/BR 指向 `0x18ffc6630`，在原始 dyld cache 中解析为
IOKit `_IOServiceOpen`。原 IOGPU 打开类型为 `1 | (API << 16)`。
NativeAdreno 的 Mesa 传输使用 `0x4147`；`0x4147xxxx` 且 xxxx 非零是
进度和文本标记。`0x41470000` 不是实际普通设备连接类型。

NativeAdreno 的 `AGXDeviceUserClient` 继承自己的 `IOUserClient`，使用
NativeAdreno ABI。原 IOGPU 的类型和用户客户端协议不等同于这个协议。
单独改 `IOUserClientClass` 字符串或改成原 IOGPU open type 并不能补足其
对象布局、selector 和内存映射契约。本次使用已有 Metal entitlement 作为
本平台连接授权依据；原 Sandbox 对具体 class/filter 的所有内部判断没有
在此宣称被完整恢复。

Kaleidoscope 的所有该类空地址异常均落在主映像 `+0x94d0`。
原始 `-[NTKKaleidoscopeResourceManager _loadMTLBufferData]` 从
`0x100009488` 开始，在 `0x1000094b4` 调用
`newBufferWithLength:options:`，长度为 128；在 `0x1000094c0` 调用
`contents`；`0x1000094d0` 执行 `stp q0, q1, [x0]`。实际 FAR=0，
故障指令为向空 contents 写入。此路径由 `initWithPixelFormat:` 调用。
Aegir 当前只保存到 SIGABRT 退出，未取得其具体 abort 调用点。

## 打开失败与释放路径

core 的实际 Mesa 源码为 `.cache/mesa-work/src/freedreno/vulkan/tu_knl_darwin.cc`：

- `dw_open_connection`，第 432 行起：取得服务后在第 455 行调用
  `IOServiceOpen`，随后释放 service；open 失败释放 mutex。
- 第 464 行 selector 探测失败及第 476 行设备查询失败均执行
  `IOServiceClose`、清空 conn、销毁 mutex。
- `darwin_device_init`，第 1874 行起：每个 Vulkan device 独立打开连接；
  后续 mutex、condition 或 worker 创建失败均关闭连接并释放 retire state。
- `tu_knl_darwin_load`，第 2095 行起：枚举临时连接在分配失败时关闭，
  `tu_physical_device_init` 返回后也关闭，第 2169 行附近。

XNU `is_io_service_open_extended` 在 `newUserClient` 成功后执行 MACF
和 selector filter 授权。后续失败路径调用 `clientClose()`、清除 terminate
defer、释放 client，最终向用户态返回失败。该顺序解释了 MACF 拒绝仍让
驱动累计 opens 和 closes 各增加一次。此次没有修改 Mesa 的正常释放路径。

## 已完成源码修改与边界

正式 core 补丁为 `patches/xnu/zzw-native-adreno-sandbox-grant.patch`，
其原有 ARM64 series 项继续选中：core
`profiles/arch/arm64/patches/xnu/series:50`，设备
`patches/arch/arm64/series:51`。没有新增补丁选择项。

`NativeAdrenoAccelerator.cpp:101` 发布 entitlement 名和连接类型属性。
补丁第 45 行的 `native_sandbox_grant_open` 要求：

1. 服务的实际 metaclass 为 `AGXAccelerator`。
2. 两个 Metal 插件属性分别为 `NativeAdrenoMetal` 和
   `NativeAdrenoMetalDevice`。
3. 授权属性为固定的 `com.apple.security.ts.opengl-or-metal`，连接类型
   属性为 `0x4147`。
4. 本次打开使用 `0x4147` 或现有非零进度标记类型。
5. `IOTaskHasEntitlement(owningTask, ...)` 返回 true。

原 XNU 入口先确认 owningTask 等于 current_task。上述 helper 覆盖服务打开
检查、创建客户端后的 MACF 检查、selector filter resolver 三处，使同一
任务的连接和后续 selector 使用一致的授权结果。

原 `native_sandbox_grant(IOService *)` 保持原签名和行为。
`zzwb-native-sandbox-grant-set-properties.patch` 仍只调用该原 helper；
新增 GPU entitlement 不参与属性写入授权。`0x4148` handoff 类型不进入
新增 entitlement 分支。其他服务和类型继续执行其现有检查。

展开 XNU 来源路径为
`out/t733-kernel/gts7fewifi/ipados-17.7.11-21H461/gts7fewifi/tree/.cache/xnu-work/iokit/Kernel/IOUserClient.cpp`。
本次修改的是被 series 选中的正式补丁，展开树由统一构建准备阶段应用。
Publish 的 grants 诊断同步由本轮客户端生命周期修改负责。

此交付状态为源码就绪。没有在此执行构建、测试、设备附加或部署；统一 57
构建负责应用正式补丁并产出镜像，后续运行需观察两个 Poster 成功建立连接
以及上述重复退出消失。
