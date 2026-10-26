# Core Animation compositor HUD 计时与显示完成回调

本文针对 iPadOS 17.7.11，build 21H461 的 Core Animation compositor HUD。
Tools 通过 `CARenderServerGetDebugOption` / `CARenderServerSetDebugOption`
的 option 36 控制此 HUD。它与 Metal HUD 是不同的实现。

本次证据来自主机已保存的原始二进制和源码。没有附加设备进程，没有运行
profiling、测试、构建或文件散列。下列地址是原始映像虚拟地址，运行时需要
各映像的实际 slide。

## 结论

- `render_for_time` 累计 `CA::WindowServer::Server::render_for_time` 主体的
  墙钟时间。主锁等待、该调用实际走到的同步等待、渲染编码、同步 pipeline
  创建、对象释放和资源回收都可能计入。它不是线程纯 CPU 用时，也不是 GPU
  命令的硬件执行时间。
- HUD 每次 `PerformanceHUD::update()` 消费并清零累计数组。时间文本没有按
  调用次数平均；默认取较大的累计值并保持最多 300 ms。折线记录本次消费值。
  同一次 HUD 更新之间发生多次 render 调用时，这些调用的主体用时会相加。
- `Frame Duration` 的输入来自 IOMobileFramebuffer frame-info 回调中的
  `Presentation_time`。它累计相邻不同 frame ID 的呈现时间差。NativeMobileDisplay
  的 frame-info 键枚举和完成报告接口目前返回 `Unsupported`，没有把 DPU 的
  呈现完成送进这条原生回调链，这是 0 ms 的直接实现缺口。
- Metal `GPUStartTime` / `GPUEndTime` 已经沿 runtime 接到 Vulkan timestamp
  query 和 Adreno CP always-on counter。CA 在另一条 completion block 中使用
  它们；不能用 command completion 的主机墙钟间隔替换 `Frame Duration`。
- 当前采集窗口 pipeline 总量只从 T `115 / 5176 ms` 变为 `116 / 5180 ms`，
  V 从 `120 / 687 ms` 变为 `121 / 688 ms`。这段窗口的反复卡顿没有对应持续
  pipeline 创建，不能继续把每次卡顿都归因于首次 pipeline 编译。

## 原始映像

工作区根目录下的来源：

| 内容 | 路径 |
| --- | --- |
| QuartzCore 原始 arm64 映像 | `out/t733-lldb-symbols/extracted/System/Library/Frameworks/QuartzCore.framework/QuartzCore` |
| IOMobileFramebuffer 原始 arm64 映像 | `out/t733-lldb-symbols/extracted/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer` |
| 原始 dyld cache，恢复链接指针和字符串 | `out/t733-system-inputs/systemos-mount/System/Library/Caches/com.apple.dyld/` |
| 原始 kernelcache | `out/t733-kernel/ipsw/21H461/kernelcache.decompressed` |
| cache 只读映射和符号工具 | `tools/t733-symbolicate-stacks.py` |
| 原显示 ABI 生成器 | `scripts/build-software-display.py` |

`llvm-objdump -d --start-address=... --stop-address=...` 可读取 dylib 指令；
kernelcache 的 prelinked executable section 使用 `-D`。ABI 生成器中的
reference 地址比这里列出的原始 kernelcache 地址小 `0x1000000`，由其
`REFERENCE_SLIDE = -0x1000000` 和本次 prelink slide 统一换算。

## render_for_time 的开始和结束

QuartzCore 的完整符号是：

```text
CA::WindowServer::Server::render_for_time(
    double, CVTimeStamp const*, unsigned long long,
    CATimingReference const*, unsigned int)
```

| 原始地址 | 实际操作 |
| --- | --- |
| `0x18951dde0` | 函数入口 |
| `0x18951dea8` | `mach_absolute_time()`，保存开始时间 |
| `0x18951df08` | 主 `pthread_mutex_lock`，位于计时开始之后 |
| `0x18951ea10` | `CA::Render::BatchWaitLock` 构造 |
| `0x189520404` | `Renderer::render` |
| `0x189520498`、`0x189520e28` | `Update::release_objects` |
| `0x1895204b8` | 内层 autorelease pool 清理 |
| `0x189520fb4` | Update 析构 |
| `0x189521128` | 主锁解锁 |
| `0x1895211bc` | `CA::Render::collect` |
| `0x189521610` | 再次 `mach_absolute_time()` |
| `0x189521614` | 结束时间减开始时间 |
| `0x18952161c` 到 `0x189521624` | 加到 `_global_values[3]` |
| `0x18952164c` | 最外层 autorelease pool 清理，位于本项计时之后 |

导入岛 `0x18da81418` 解析到 `mach_absolute_time`，因此这里测的是经过时间。
已有保存调用栈包含 `swap_wait_timeout -> can_update -> render_for_time`，
以及 Metal `commitAndWaitUntilSubmitted -> waitUntilScheduled`。这些栈证明
相应等待路径存在；一次 300 ms 事件具体在哪个子阶段仍需以同一事件的阶段记录分解。

## HUD 如何更新、保持和归零

`GetPerfHud` 位于 `0x1895e9cb4`。`0x1895ea038` 到 `0x1895ea050`
登记 index 2，名字为 `Frame Duration`，counter type 2；
`0x1895ea054` 到 `0x1895ea06c` 登记 index 3，名字为 `render_for_time`，
counter type 同为 2。两项都使用全局数组。

```text
_global_values      = 0x1ed7ac948
_prev_global_values = 0x1ed7ac6e0
ticks_to_ms         = 0x1ed7acfe0
```

`PerformanceHUD::update` 位于 `0x18967b4c0`：

1. `0x18967b608` 读取本轮累计值。
2. 时间项默认关闭平滑。可选平滑分支在 `0x18967bb7c` 到 `0x18967bbbc`，
   实际计算 `0.995 * previous + 0.005 * current`；系数存于 `0x18970ebf8`。
3. `0x18967b630` 到 `0x18967b690` 按比较模式更新显示文本值。模式 0
   遇到较大值立即更新；较小值等保持期过去后更新。两项保留默认模式 0。
   `init_debug` 的 `0x1894f6bbc` 到 `0x1894f6bc0` 明确将保持期写成 300 ms。
4. 图形分支 `0x18967b69c` 到 `0x18967b768` 使用本轮累计值；其中除以
   counter 的显示量程是绘图归一化，不是除以 render 调用数。
5. `CounterInfo::to_string` 的 `0x18967bda4` 到 `0x18967bdc8` 将时间项
   乘 `ticks_to_ms`，交给 `%.2fms`，没有平均除数。
6. `0x18967bbfc` 将全局累计数组复制到 previous 数组。
   `0x18967bc34` 开始的向量清零指令清空全局累计数组，下一轮重新累计。

因此显示数字是 HUD 消费间隔内的累计用时，文本还带 300 ms 峰值保持。
300 ms 的保持期和数值本身恰好同量级，并不意味着显示的 300 ms 是保持期。
HUD 在渲染过程中更新，当前 render 调用结束时才累加它自己的主体用时，
所以通常由后续 HUD 更新显示这一次调用的耗时。

## Frame Duration 的真实数据源

`CA::WindowServer::IOMFBServer::frame_info_callback` 入口为 `0x1894f34f8`。
该函数先按回调 frame ID 找到 CA 保存的 FrameInfo，然后从回调字典中取出：

| 原始地址 | 字段 |
| --- | --- |
| `0x1894f358c` 到 `0x1894f359c` | `Presentation_time`，CFString 位于 `0x1e694b940` |
| `0x1894f35a0` 到 `0x1894f35b0` | `Requested_presentation`，CFString 位于 `0x1e694b960` |
| `0x1894f35c0` 到 `0x1894f35e8` | 显示使用 continuous 时钟时转换回 absolute 时钟 |
| `0x1894f39dc` 到 `0x1894f39f8` | 之前 frame ID 非 0 且不同于当前 ID 时，计算此次 Presentation 减上次 Presentation |
| `0x1894f39fc`、`0x1894f3a08` | 保存当前 ID 和 Presentation |
| `0x1894f3a0c` 到 `0x1894f3a14` | 将差值加到 `0x1ed7ac958`，即 `_global_values[2]` |
| `0x1894f3a18` 到 `0x1894f3a2c` | 另一项统计累计 Presentation 与 Requested 的绝对差 |

CA 的 `AccelServer` 构造函数在 `0x1895c5888` 取得支持的键列表，列表为空
就跳过 frame-info 注册。`0x1895c58ec` 注册回调，`0x1895c5900` 按注册
结果启用或关闭 CA FrameInfo 保存。后续仅对驱动支持的键调用
`enable_frame_info_tag_list`，因此平台可以只支持有真实数据的字段。

当前缺口位于 `drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp`：

- `collectCompletionReports` 原起始行 1454，输出 count 0 并返回 Unsupported。
- `supportedFrameInfo` 原起始行 1547，输出 count 0 并返回 Unsupported。
- slot 211、306、307 是停止执行的 guard；完整修复还必须实现修改订阅键、
  订阅启停和每个订阅者的消息发送，不能只让枚举返回成功。

### 原始通知 ABI

| 地址 / slot | 已恢复的调用契约 |
| --- | --- |
| IOMFB `0x1b63d53f8` | `InfoKeyInitialize` 调 selector 74，枚举每个 64 字节键名及键总数 |
| IOMFB `0x1b63d57ac` | `IOMobileFramebufferFrameInfo` 注册 notification type 3 |
| IOMFB `0x1b63d6544` | `kern_ChangeFrameInfo` 调 selector 73，传 type 3 与键 mask |
| kernel `0xfffffff0067785f8` | user client 将 type、原订阅记录地址和 mask 传给 framebuffer slot 210 |
| kernel `0xfffffff006794058` | slot 210 进入原 work-loop gate，再调用 slot 211，参数为 type、订阅记录、mask |
| kernel `0xfffffff00679607c` | 注册 type 3 后调用 slot 306，参数为订阅记录、true |
| kernel `0xfffffff0067963cc` | 解除 type 3 时调用 slot 306，参数为订阅记录、false |
| kernel `0xfffffff006796934` | 原 frame-info sender 遍历原类持有的订阅列表，逐条调用 slot 307 |
| kernel `0xfffffff0067a00f4` | sender 最后调用的 slot 308 不读取报告内容，无操作 |
| kernel `0xfffffff00678aa70` 到 `0xfffffff00678aa90` | `completeCurrentSwap` 在释放请求前调用 slot 337，参数为 request 的 frame ID、null、null、0 |
| IOMFB `0x1b63da1dc`、`0x1b63d3188` | 接收异步消息，按 mask 位顺序将 64 位字段变为 CFNumber，组成回调字典 |

正常 64 位消息格式为 `[mask, frameID, valueForLowestSetBit, ...]`。
bit 31 表示用两段 32 位数据打包一个字段。当前 arm64 客户端使用完整 64 位值。
原订阅记录的异步引用位于 `+0x18`；该记录和引用由原显示类及 user client
拥有，平台回调不能释放它们。

### 通知传输必须同时匹配客户端和内核

selector 74 的最后一个输出是通知传输选择，不是支持字段的 mask。
`InfoKeyInitialize` 在 `0x1b63d54cc` 将首次枚举返回的该值保存到客户端
`+0xde8`。`kern_GetRunLoopSource` 的 `0x1b63d6c54` 到 `0x1b63d6c64`
和 dispatch queue 注册路径 `0x1b63d5210` 到 `0x1b63d5220` 均检查：
notification type 为 3 且这个值为 1 时，选择 IOShq；否则使用 Mach 异步通知。
Apple 原派生驱动的枚举实现 `0xfffffff006bb66b4` 返回 14 个键，并返回 1。

内核侧也有配套开关。原基类 slot 336，`0xfffffff0067a023c`，返回 true。
注册函数在 `0xfffffff006795f58` 调用它，true 会跳过订阅记录 `+0x18` 的
异步引用初始化；false 才执行 `0xfffffff006795f68` 到 `0xfffffff006795f94`，
将用户端口、callback、context 和 task 交给原初始化帮助函数。

选定的平台实现沿用已有 Mach 通知，因此须一次完成两项修改：

1. `supportedFrameInfo` 的最后一个输出返回 0。
2. slot 336 用正确的 `bool` 签名覆盖并返回 false。

这两项分别决定客户端接收通道和内核异步引用初始化。单独修改任何一项都会
产生两端不匹配。原注册、注销、订阅记录和 work-loop gate 继续由基类管理。

原 Apple 订阅记录的字段和操作已恢复：

| 字段 / 地址 | 含义 |
| --- | --- |
| record `+0x10` | 原 user client 指针 |
| record `+0x18` | `OSAsyncReference64` |
| record `+0x74` | 原注册路径设置的活动状态 |
| record `+0x78` | 订阅字段 mask，32 位 |
| record `+0x80` | 原发送器保存的上一次 Presentation，用于 Presentation_delta |
| `0xfffffff006bb672c` 到 `0xfffffff006bb6770` | 原 mask 更新实现只处理 type 3，更新 `+0x78` |
| `0xfffffff006bb6708` | 取消订阅时经同一路径清除 mask |

本平台只枚举 `Presentation_time` 和 `Requested_presentation`，对应本平台
键列表的 bit 0 和 bit 1。CA 按键名求交集，故无需保留 Apple 14 键列表中
原来的 bit 6 和 bit 8。原 mask 字段足够，不另建订阅表或持有第二份引用。

### 完成报告和 Requested_presentation 的原始布局

Apple 的 slot 337 实现从 `0xfffffff006bfa478` 开始。输出缓冲非空时，
count 指针必须非空且 capacity 非零，否则返回 BadArgument。它先清零 count，
逐份报告增加 count；`0xfffffff006bfa5f4` 和 `0xfffffff006bfa880` 明确使用
`0xd8` 字节记录步长。输出缓冲为空时，使用栈上报告并发送通知。

原驱动在 `+0x18f0` 保存最多 16 份 `0x4c` 字节的待退役记录，slot 337
按目标 frame ID 消费至该帧，并扩展为 `0xd8` 字节报告。这是其多请求硬件
队列的所有权安排。本平台 DPU 仅允许一份进行中的请求，退休前暂存一份报告
即可；报告先从平台暂存状态移除，再交给基类发送，避免重入重复通知。

| 报告偏移 | 宽度 | 已恢复的来源和用途 |
| --- | --- | --- |
| `+0x00` | 64 位 | request `+0x2d8`，原发送器作为 Issued_time 读取 |
| `+0x08` | 64 位 | request `+0x2d0`，Requested_presentation |
| `+0x10` | 64 位 | 硬件完成的 Presentation_time |
| `+0x30` | 32 位 | request `+0x314`，CA 的 swap/frame ID |
| `+0x34` | 32 位 | 原控制器生成的状态字段，不作为本平台时间字段 |
| `+0x40` | 32 位 | request `+0x32c` 的原完成状态 |
| `+0x49` | 8 位 | 原初始化函数清零的标记 |
| `+0xcc`、`+0xd0` | 各 32 位 | 原控制器反馈值 |
| `+0xd4` | 8 位 | 上述控制器反馈值有效标记 |
| 全记录 | `0xd8` 字节 | 未实现字段和保留区清零，不设置未知有效标记 |

Requested 的直接证据是 `0xfffffff006bd5954` 的
`ldr q0, [request, #0x2d0]`，随后 `ext ... #8` 交换两个 64 位元素，
`0xfffffff006bd595c` 写入报告开头。因此 report `+0x08` 来自 request
`+0x2d0`。原发送器 `0xfffffff006bb6a50` 正是为 Requested 键读取该位置。
`0xfffffff006bb6a38` 为 Presentation 键读取 `+0x10`，
`0xfffffff006bb6a58` 为 Issued 键读取 `+0x00`。

输入链同样匹配：`iomfb_SwapSetTimestamps` 的类型 1 写客户端 `+0x18`，
进入 wire `+0x00`，内核在 `0xfffffff00678f744` 起的复制路径写 request
`+0x2d0`。CA `finish_update` 的 `0x189502560` 到 `0x189502588` 把其
计划呈现时间转换为 host ticks，交给类型 1。类型 2 则来自另一个 CA 时间，
最终位于 request `+0x2e0`，不能当作 Requested 使用。

### 显示时间使用 continuous 时钟

内核原基类 slot 241 位于 `0xfffffff00679fc28`，调用
`0xfffffff0067920b8`、`0xfffffff0067920cc`，再经导入岛
`0xfffffff0067a2ecc` 和 GOT `0xfffffff006f33ec0` 到导出符号
`_mach_continuous_time`。这也是原请求 Issued 时间的时钟。

用户端兼容层负责时钟转换：

| 地址 | 实际操作 |
| --- | --- |
| CA `0x1896e7204` 到 `0x1896e7214` | 调 `IOMobileFramebufferSetParameter(8, 1, &one)`；成功后置 display `+0x6c08` |
| IOMFB `0x1b63d2468` 到 `0x1b63d2488` | 参数 8 只设置客户端 `+0x78c`，值只能为 0 或 1，不向内核发送 selector |
| IOMFB `0x1b63d3148` | `kern_ingest_timestamp`：客户端未选择 continuous 时，对非零输入加 continuous 减 absolute |
| IOMFB `0x1b63d3368` | `adjust_ni_timestamp`：按键名识别需转换的时间；选择 continuous 时直接保留 |
| IOMFB `0x1b63d8348` | `kern_egest_timestamp`：默认客户端对非零输出减 continuous 加 absolute |
| CA `0x1894f35c0` 到 `0x1894f35e8` | continuous 模式下由 CA 自己将两项回调时间转回 absolute |

导入岛 `0x1ba64c884` 和 `0x1ba64c890` 分别解析到
`mach_absolute_time` 和 `mach_continuous_time`。转换键名列表包含
Issued_time、Presentation_time 和 Requested_presentation。

因此新 DPU 呈现估计和它的时间区间必须直接采集 `mach_continuous_time()`；
request 的 Requested 和 Issued 原值保持不变。现有 DPU 性能统计使用
`mach_absolute_time()` 计算阶段耗时，可以继续保留，不能直接作为原报告的
Presentation 绝对端点。此区分也覆盖系统休眠后两种时钟的偏移变化。

## DPU 的呈现时间与精度

`NativeDpuSurface.cpp` 的 `t733_dpu_surface_poll` 原起始行 720，只有在
CTL0 flush 已清零且 INTF1 frame counter 已变化时才完成 IOSurface 换帧。
该条件与原厂 `sde_encoder_phys_vid_vblank_irq` 检查 flush 后才退役请求的
做法一致。当前实现通过 display work-loop timer 观察完成，尚未保存这一
观察的时间及误差区间。

成熟 DRM 栈允许没有精确硬件 timestamp 的驱动报告主机单调时钟估计：
本地参考 `references/android_kernel_samsung_sm7325/drivers/gpu/drm/drm_vblank.c`
的 `drm_get_last_vbltimestamp`，735 到 754 行，在精确查询不存在或失败时返回
`ktime_get()`，同时返回精度标识 false。该文件 566 到 708 行的
`drm_calc_vbltimestamp_from_scanoutpos` 则在已知实际 mode clock 和 scanout
位置时回推 vblank，记录查询前后的时间差作为误差，并限制重试次数。

本平台应以首次完成观察的 continuous 时间作为呈现估计，在旧 buffer 释放
之前取样，并保留时间来源及区间宽度。这与 DRM 的低精度回退一致。最初的
区间下界在写 CTL flush 前取样；只有读到 flush 仍未完成时，才可将下界推进
到该次寄存器读取之前的时间。当前代码在写 flush 后读取 pendingFrame，
单凭 frame counter 仍相等，不能推出新 buffer 尚未开始扫描，因此不能用
该条件缩小呈现时间区间。完成条件仍采用 flush 清零且 frame counter 前进。

区间上界在首次观察到完成后立即取样。保留宽区间能够覆盖计数器读取跨帧和
work-loop 调度延迟；不能宣称固定 1 ms 精度。该值是软件观察产生的呈现估计，
不是硬件锁存时间。当前没有已证实的 mode pixel clock，不能拿固定 60 Hz
减去 line counter 伪造高精度 vblank 时间。后续接入真实 DPU IRQ 或恢复完整
mode clock 后，沿同一报告契约替换时间来源。

显示报告只为硬件已确认的成功 IOSurface 换帧生成。失败、恢复 firmware
buffer、CPU copy 和提前退役的 GPU copy 都不能虚构一个新 IOSurface 的
Presentation_time。报告必须携带原请求 frame ID，不能用 DPU frame counter
冒充 CA 的 swap ID。

## 下一次显示源码交付的完整范围

实施顺序按所有权组织，以下各项组成同一次显示交付：

1. `NativeDpuSurface.h/.cpp` 保存固定大小的呈现结果：continuous 观察时间、
   区间下界、上界、实际 frame counter 和来源。开始新操作、恢复 firmware、
   fault、timeout 或 unmap 失败时清除有效结果；成功 SurfaceOperation 后
   提供一次消费接口。DPU 模块继续拥有 buffer 映射和 fence，不持有 CA 请求。
2. `NativeLegacyDisplay.cpp` 在当前请求仍被保留且 DPU 呈现成功时，读取原
   request 的 frame ID、Requested、Issued 和状态，构造清零的 `0xd8`
   字节报告；随后按现有顺序退役并释放请求。仅与 pendingSwapRequest 相符
   的成功 SurfaceCompletion 生成报告，避免把已替换或提前退役的请求送回 CA。
3. slot 337 使用正确类型接收报告数组、count 和 capacity。非空数组要求
   count 非空且 capacity 非零；无匹配完成报告时 count 为零并成功返回。
   匹配报告最多一份，取走后清除平台状态；有数组则复制，无数组则调用原
   `native_legacy_frame_info_notify`。原 sender 在 gate 内遍历已有订阅。
4. slot 211 接收 type、订阅记录和 mask，只允许 type 3 及两个已公布位；
   slot 306 在注册和注销时清除该记录的 mask；slot 307 从记录读 mask，
   在栈上构造最多 4 个 64 位值的消息。所有这些操作沿用原 work-loop gate，
   不保存越过回调生命周期的订阅记录指针。
5. slot 307 使用 `IOUserClient::sendAsyncResult64WithOptions` 和
   `kIOUserNotifyOptionCanDrop`。现有 XNU `iokit/IOKit/IOUserClient.h`
   第 103、334 到 343 行明确规定有限排队；`iokit/Kernel/IOUserClient.cpp`
   第 2338 到 2340 行采用零超时发送。队列满或端口关闭只丢弃本次可选报告，
   在固定计数器保存错误及丢弃数，不积压请求，也不阻塞 DPU 退役。平台发送
   回调向基类返回成功以避免原遍历器逐帧打印相同传输错误；真实发送状态另存。
6. 枚举两项键，返回 transport 0，并覆盖 slot 336 返回 false。
   `scripts/build-software-display.py` 同时更新 slot 211、306、307、336、337
   的实际声明和绑定；新增原 sender 绑定的 reference 地址为
   `0xfffffff005796934`，按生成器现有 slide 规则得到本次原始地址
   `0xfffffff006796934`。其余原基类方法继续按原 vtable 绑定。

本方案复用原 IOMobileFramebuffer 的注册、取消、订阅表、客户端字典解码、
时钟转换以及 XNU 有限消息队列。DPU 首次完成观察、误差记录、单个请求到
完成报告的转交是 SM7325 适配。不会在显示热路径逐帧分配报告或追加无界队列。

后续实机验收保留固定计数器：成功 SurfaceCompletion、生成报告、发送成功、
队列满丢弃、其他发送错误、无匹配 frame ID；保留最近 64 份呈现观察区间。
正常接收时成功换帧与生成报告一一对应，失败和 firmware 恢复不增加报告数。
CA 首次样本只建立前值，后续不同 frame ID 才增加 Frame Duration；其变化
应与呈现间隔及记录的误差区间一致。丢报告会使下一份收到的样本跨越更长间隔，
必须与丢弃计数一同解读。此处列的是下一次交付的实机验收要求，本次未执行。

## Metal 时间戳链

QuartzCore 的 `MetalContext::flush` 在 `0x1894a684c` 到 `0x1894a6884`
有条件登记 completion block。block `0x18965a8ac` 读取 GPUEndTime 和
GPUStartTime，差值保存在 `MetalContext + 0xf90`，由
`last_command_buffer_duration()`，`0x18965964c` 读取。

实际构建源位于公共 core 的 `.cache/gpu-reference/metal2vulkan`：

1. `NativeBridgeMetalFrontend.mm:833`、`:834` 的 GPU getters 调
   `nmb_command_buffer_gpu_time`。
2. `NativeMetalObjects.rs:1492` 将 getter 转到 runtime。
3. `runtime/src/command_buffer/mod.rs:1878` 的 `gpu_times` 在未完成、查询
   未就绪或无实际 GPU 工作时可能返回零跨度；缺少时钟等错误经 bridge guard
   返回 0，并保留 lastError。
4. 同文件 `:2383` 取得 query slot，`:2415` 在完成后读取两个 GPU timestamp
   及 availability。
5. `runtime/src/command_buffer/submit.rs:769` 到 `:797` 在首个命令缓冲
   写 TOP_OF_PIPE 时间，在最后一个命令缓冲写 BOTTOM_OF_PIPE 时间。
6. Mesa `src/freedreno/vulkan/tu_query_pool.cc:2078` 的
   `tu_CmdWriteTimestamp2` 用 `CP_REG_TO_MEM` 读取 `CP_ALWAYS_ON_COUNTER`，
   非 TOP 阶段先等待相应 GPU 工作，再发布 availability。
7. `gpu_time.rs:295` 的校准路径关联 host/GPU 时钟；未校准路径用 completion
   时间放置结束点，同时保留 GPU counter 测出的跨度。后者的绝对端点有主机
   通知延迟，跨度仍来自 GPU counter。

CPU 读取校准 counter 的链为 `tu_knl_darwin.cc:1646` 的 GetTimestamp，
`NativeAdrenoUserClient.cpp:413`，`NativeAdrenoService.cpp:796`，最后到
`NativeAdrenoTimer.cpp:38` 的 high-low-high 稳定读取，CP 寄存器为
`0x980 / 0x981`。

`kernelStartTime` / `kernelEndTime` 在 frontend 原行 830、831 返回 0，
这是另一个未实现的 Metal CPU 提交计时契约。它不能解释 display Frame Duration
的 0，也不能作为 GPU elapsed 的替代来源。

Apple 对 getter 的公开定义见
[GPUStartTime](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime)
和 [GPUEndTime](https://developer.apple.com/documentation/Metal/MTLCommandBuffer/gpuEndTime)。
它们完成前为 0，完成后差值描述 GPU 命令时间。本段只使用 Metal API 契约，
没有套用 Metal HUD 的字段定义。

## 对长事件的有界记录

目前 55b 保存窗口中，kernel 最近 256 个 submit 的 CPU 准备通常为每 chunk
6 到 8 ms，提交到 fence 完成观察的 latency 多为 25 到 50 ms。Metal completion
callback 墙钟跨度出现 62829、114789、779563、862683 us。它们覆盖不同阶段和
不同批次，不能相互替换，更不能把 fence 观察 latency 命名为 GPU execution。

后续记录采用固定容量结构，按 command buffer ID、kernel submit sequence 和
display swap ID 保留已有对应关系。每个已测量阶段记录开始、结束和来源：
pipeline 创建、render 编码结束、提交排队、CPU 发布、queue submit、fence 首次
观察、completion handler 调度、DPU 提交、DPU 首次完成观察、退役回调。
真实 Vulkan query duration 单列，并携带 availability 与校准状态。

各层保留最近 64 条超 50 ms 事件，外加累计次数、总时长、最大值与被覆盖计数；
日志只在请求摘要或低频汇总时输出。DPU 记录额外保存呈现估计的下界、上界、
frame counter 和时间来源。资源释放必须在记录中与完成观察分开，避免把页回收
尾延迟算成显示引擎仍在扫描。未来验收先看 frame-info 成功回调计数和原 swap ID
的对应，再看 Frame Duration 是否更新，以及它的波动是否超过已记录的时间误差。

## 实施状态

上述 HUD、回调 ABI、Requested 字段、通知传输选择和 continuous 时钟链已经
完成静态定位。本次交付仅补充研究文档，没有修改实际显示源码、ABI 模板或
构建脚本。`system-gpu-memory-56b` 沿用原显示实现；FrameInfo 的完整实现按
上节范围另一次交付，当前 Frame Duration 为 0 的实现缺口尚未修复。
