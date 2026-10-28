# SM-T733 的 21H461 显示格式策略

本机面板与当前 DPU 接入使用 SDR sRGB、未压缩扫描输出。设备 `board.toml` 在签名代码页首次映射执行之前，使用已有 `payload.text_fixups` 修正 21H461 的框架自动格式选择。`board.platform.toml` 同样覆盖 ADT gamut；平台配置保持既有空 `text_fixups`，因为它不启动这套用户态图形系统。

这份实现记录对应源码，尚未构建或进行设备验证。内核 `native-text-fixup.patch` 从 `/chosen/text-fixups` 读取文件名、偏移、原字节与替换字节，先验证原文件签名，再匹配原字节并写入物理页，最后调用 `cache_sync_page`。设备已有 `native-text-repage.patch` 使页面被换出再读入时重新执行同一流程。磁盘上的 Apple 文件保持原内容。

## RenderBox 的资源创建

RenderBox UUID 为 `947FCCE5-1883-3B77-8D1E-04625B7E8EB4`。`RB::ColorMode::pixel_format` 中 `0x1e262844c` 原指令为 `tbz w2,#2,0x1e26283e4`，替换为 `b 0x1e26283ec`。它只改变普通 RGB8 候选 523/542 的自动选择，转入框架已有的 BGRA8 分支。

该分支使用原来的 transfer 位：位清零返回 Metal 80，置位返回 81。颜色空间输出由此前 `0x1e26282cc-0x1e26282d0` 的 `eor` 和 `strb` 产生，alpha/格式标志也已由原函数写入；修正不重新解释它们。默认 colorMode 0 的 transfer 位为零，因此采用 BGRA8Unorm 80，并保留 RenderBox 原本写出的 ColorSpace 字节 1。非默认 transfer 对应 BGRA8Unorm_sRGB 81。这里的 sRGB 显示颜色空间不等同于所有纹理都必须使用 Metal 81。

同一函数的调用方包括 `RBLayer displayWithBounds:callback:`、`make_image_provider`、图像拷贝和 `RBSurface _updateWithDevice:frame:synchronized:`。选择结果继续传给 `SharedSurfaceGroup::add_subsurface` 或 `RB::Device::alloc_shared_texture`，再由 `RB::Texture::alloc` 创建实际资源，颜色空间参数沿原调用链传递。显式 `RBLayer.pixelFormat`、高精度 ColorMode、YUV420/422 和单/双通道专用分支保持原选择；已经分配的双平面 IOSurface 不改 descriptor。

## QuartzCore 的显示能力

QuartzCore UUID 为 `BCA74CF2-236A-30AC-BD05-8872B2C2262D`。以下存储指令改为存储 `wzr`，使本机不发布未实现的显示/扫描能力：

| 首选虚拟地址 | 原缓存写入 | 本机结果 |
| --- | --- | --- |
| `0x18957be88`、`0x18957bec0` | CADeviceUseCIF10 的 MG 与 preference 结果 | false，普通 SDR 显示不请求扩展色 framebuffer |
| `0x1894d9f04` | CADeviceSupportsUniversalCompression | false |
| `0x1894d9ea8` | CADeviceSupportsLossyUniversalCompression | false |
| `0x189682ae8` | CADeviceDisplaySupportsEDRFactor | false |
| `0x189683b80` | CADeviceHasHardwareAcceleratedHDR | false，当前没有 Apple scaler HDR 接口 |
| `0x189683ce4` | CADeviceSupportsHDRTVModes | false，当前没有 HDR 外接输出模式 |

`CADeviceSupportsHDRProcessing` 以及显式高精度离屏处理保持原实现，HDR 内容仍可通过处理后映射到 SDR 输出。Apple family、SoC 身份和 GPU 的原生 10-bit packed 格式能力保持原表达。Metal 插件继续设置已有 framebuffer/IOSurface compression preference 为 false。

`board.toml` 的数组会替换 OS profile 数组，因此保留设备已有的十六项 fixup，再增加八项显示修正，总计二十四项，低于内核三十二项上限。RenderBox 位于 `dyld_shared_cache_arm64.31`，新增 QuartzCore 项位于 `.03`；每项匹配目标指令及前后各一条原指令，总计 12 字节。固定 OS profile 为 `ipados-17.7.11-21H461`，更换 OS 必须重新确定这些位置和原字节。

## 启动、MobileGestalt 和状态报告

两个 board 的 `/product/artwork-display-gamut` 均为 NUL 结尾的 `sRGB`。MG 的 ArtworkTraitDisplayGamut getter读取该字段，hasExtendedColorDisplay 比较其是否为 P3。此前 60a 运行记录已显示扩展色 MG 回答 false，同时 RenderBox 仍创建普通 RGB 双平面，因此 ADT 修改与 RenderBox 修正共同实现本策略。

addon 携带 `NativeAdrenoMetal.display-policy`，版本为 `21H461-srgb-bgra8-1`。`T733DebugNetwork` 在网络初始化前比较 `/private/var/db/t733-display-policy`。版本变化时，只删除 MG 的共享系统组 cache、两种大小写的 mobile preference cache 和旧 mobile Library/Caches 文件，随后以原子 rename 保存版本和本次 `kern.boottime`。其他 Data 内容与钥匙串不参与这项操作。

helper 不发起重启。本次 boot 的状态是 `awaiting-restart`，由部署负责人安排下一次完整启动；后续不同 boot 的状态是 `cache-refreshed`。相同版本的正常后续启动不再次删除缓存。删除或版本保存失败会保留错误状态，使下一次启动重试。状态同时进入 `/private/var/run/t733-debug-network.json` 的 `display_policy` 字段。

`NativeAdrenoSystemFormatPolicy.h` 只读取已映射代码。插件在首次返回 Metal device 之前注册 dyld 回调：已有框架立即被检查，后加载框架在其初始化前检查。回调不调用 loader 或 Objective-C API，不写内存，不改变页面权限。OS build、框架 UUID 与实际指令共同决定报告：`applied`、`not-applied`、`bad-code`、`bad-uuid`；尚未加载为 `unloaded`，OS 不匹配为 `unsupported`。`not-applied` 包括只应用部分指令的情况。

每个框架加载时只输出一条状态，backboardd 仅在状态变化时通过已有 USB 文字通道输出 `FMT` 行。全部指令确实是替换内容时才报告 applied。策略本身由内核在任何进程执行框架之前完成，插件加载先后不会改变格式选择。
