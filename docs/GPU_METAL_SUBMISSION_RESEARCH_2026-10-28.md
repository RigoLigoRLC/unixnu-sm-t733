# 从系统 Metal 调用到 A642L 执行的格式、同步与等待研究

日期：2026-10-28。范围：SM-T733、iPadOS 17.7.11 21H461、AdrenoMetal、metal2vulkan、Turnip Darwin、Adreno6xx 与 DPU 显示交接。本轮整理用户决定并继续源码研究，仅修改文档。

## 用户决定和研究目标

1. DVFS 延后。当前阶段优先消除桌面合成中的多余工作、等待和转换，不以提高频率作为本轮优化方案。
2. 查清 A6xx 对 Apple 使用的双平面 10 位格式的采样、写入和渲染支持。优先研究让系统在创建 SDR 合成表面时直接选择每像素 32 位的 BGRA8/sRGB，贯通颜色空间、纹理格式、pipeline attachment 与显示扫描格式。
3. 提交与同步策略优先采用成熟驱动已经实现的办法。先把 Metal API 的顺序和资源访问要求映射到这些办法，再沿系统调用、Metal 前端、metal2vulkan、Turnip 和内核逐层落实。
4. 重新设计等待粒度，明确每个 command buffer、queue、GPU job、IOSurface 写入者和 DPU 扫描任务实际等待哪个完成点。
5. 将普通 draw 顺序和固定功能渲染顺序能够保证的部分交给硬件。研究以编码期增量记录和具体命令边界生成依赖，消除提交时扫描全部资源列表来推导 barrier 的工作。

第 5 项是待落实的设计方向。研究必须列出可直接使用顺序的调用，以及 shader 内存访问、跨 encoder/queue、显式 Metal fence/event 等需要处理的具体情况，避免凭命令数量或资源列表猜测依赖。

## 已有时序证据

图中 3000-3130 区间来自 `system-gpu-coherent-60a`，原始文件为工作区 `logs/t733-usb-live/t733-20261027T181353.446480Z.raw`。按提交编号去重后有 131 笔，内核记录的提交 CPU 工作平均 18.57 us，软件缓存维护为零；提交到 CPU 观察完成平均 28.893 ms。下面确认的各类格式转换，需要与这些具体任务的表面和 pass 关联。

| 提交 | IB 数 | 提交到观察完成 | 内核提交 CPU 工作 |
| --- | ---: | ---: | ---: |
| 3025 | 3 | 17.722 ms | 13 us |
| 3050 | 30 | 47.911 ms | 26 us |
| 3075 | 3 | 14.195 ms | 10 us |
| 3100 | 28 | 48.883 ms | 29 us |

126/131 笔在前一笔已被观察到完成后进入内核，两个时点的间隔中位数为 10.912 ms。该间隔需要继续对应用户态录制、依赖等待和线程调度。Metal 记录在第二段高台时段出现 13 个 render pass、140-160 次 draw；后续降至 1-3 个 pass。Metal command buffer 编号与内核提交编号分别计数，按时间及后续关联标识对应。

## A6xx 格式支持与现有转换

这里的 10 位指每个颜色分量的位数。Apple 的 RGB 加 alpha 双平面、RGB raw-code 双平面和视频 P010 分别具有不同布局。

| 格式 | 原始内存布局 | 当前采样图像 | 当前写入或渲染方式 |
| --- | --- | --- | --- |
| `RGB10A8_2P_XR10` 550/551，IOSurface `b3a8` | RGB 三个 10 位分量放在一个 32 位字中，另一个平面保存 alpha8，共 5 B/px | `R32G32_UINT`，8 B/px | staging copy、compute 重排和 buffer/image copy；XR10 混合还可按需增加 RGBA16F 图像 |
| `RGB8_444_2P` 542，包含 `_RB4` | plane 0 为绿色8，plane 1 为蓝色8、红色8，共 3 B/px | `R8G8B8A8_UNORM`，4 B/px | storage 写入通过 RGBA32Uint 保存三个 float 位模式及是否写过的标记，再编码回原平面 |
| `RGB10_444_2P` 545 | plane 0 一个16位字的高10位，plane 1 两个16位字的高10位，共 6 B/px | `A2B10G10R10_UNORM_PACK32`，4 B/px | storage 写回采用上项的标记和重编码程序 |
| `YCBCR10_420_2P` 505，P010 型布局 | 每像素 Y16，每 2 x 2 像素一个 Cb16/Cr16 对，平均 3 B/px | converted 视图 RGBA16F，sRGB 变体 RGBA32F | 支持的 converted YCbCr color attachment 使用 RGBA32F render shadow，再编码回 plane |
| `BGRA8Unorm_sRGB` 81 | 单平面 4 B/px | 原生 BGRA8 sRGB | A6xx/Turnip 支持直接采样、颜色输出和混合 |

`RGB*_2P` raw-code 读取将 Cr、Y、Cb 分别作为 R、G、B，alpha 为 1。`ycbcr_renders()` 当前接受的 converted YCbCr attachment 与这些 raw-code 格式分别处理。RGBA32F render shadow 的成本只归入实际使用它的 converted YCbCr 路径。

A6xx 的 `a6xx_enums.xml:123` 定义了 `FMT6_P010`、`P010_Y/UV` 与 `TP10/Y/UV`。本机 Mesa `fd6_format_table.c:376` 和 `fd6_view.cc:281` 接通的是 NV12/YV12 类 8 位 4:2:0 多平面采样，颜色输出字段为 `FMT6_NONE`；P010 和 Apple RGB 加 alpha 格式尚无对应接入。`tu_shader.cc:880` 对需要矩阵转换的 YCbCr 采样插入 `nir_convert_ycbcr_to_rgb`。

因此，对当前 `b3a8` 可执行的优化方向是让创建者选择 BGRA8，或另做直接读取两个平面的完整实现。普通 RGB10A2 的 alpha 只有 2 位，XR10 的颜色解码为 `(code - 384) / 510`；把已有 `b3a8` 当成 RGB10A2 会改变颜色和 alpha。

源码入口，均位于公共 core 的忽略目录，Mesa revision 为 `85fda3b36405bcaea8bffc64aa4f2651dc90770d`：

- `.cache/mesa-work/src/freedreno/fdl/fd6_format_table.c:181`、`:209`、`:376`：BGRA8、RGB10A2、多平面能力。
- `.cache/mesa-work/src/freedreno/vulkan/tu_formats.cc:193`、`:233`：sampled 与 color attachment/storage 能力的生成。
- `.cache/gpu-reference/metal2vulkan/crates/metal2vulkan/src/metal/format/mod.rs:957`、`:1730`：采样格式和 converted YCbCr attachment。
- 同项目 `crates/metal2vulkan-runtime/src/texture/surface_two_planes.rs:37`、`render/extended_shadow.rs:1`、`texture/ycbcr_image.rs:375`、`ycbcr.rs:89`：三类转换及 XR10 混合。
- 同项目 `crates/metal2vulkan/src/bind/ycbcr_convert.rs:5`、`:50`、`ycbcr_write.rs:35`：布局、精度和写回舍入。

小中间格式仍有独立研究空间：`RGB*_2P` 采样现已使用 4 B/px；`b3a8` 的 30 位 RGB 加 8 位 alpha 可以研究 `R32_UINT + R8_UINT` 直接读取，需同时处理过滤、混合和写回；converted YCbCr render 的 half 舍入规则可能允许 8 B/px 中间目标，需要把舍入与 encode 一起调整。优先级排在系统直接选择 SDR 格式之后。

公开来源：[Apple BGRA8 sRGB](https://developer.apple.com/documentation/metal/mtlpixelformat/bgra8unorm_srgb)、[Apple BGRA10 XR](https://developer.apple.com/documentation/metal/mtlpixelformat/bgra10_xr)、[Mesa A6xx 格式表](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/freedreno/fdl/fd6_format_table.c)、[A6xx 格式编码](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/freedreno/registers/adreno/a6xx_enums.xml)。

## 从系统源头选择 SDR 格式

当前 `AdrenoMetal/NativeBridgeMetalFrontend.mm:1568` 已把 `supportsExtendedXR10Formats` 返回为 false。`NativeAdrenoMetalPlugin.mm:234` 读取 `CAEnableDeepFramebuffer` 及 MobileGestalt 能力用于诊断；该位置没有设置 deep framebuffer 开关。该文件关闭 framebuffer/IOSurface compression，控制的是 Apple 压缩布局。

用户要求优先通过设备描述使系统自动选择 SDR 格式。实施入口先采用 ADT、MobileGestalt 和显示能力的消费关系；公共 `pixelFormat` API 只用于说明格式如何传递。目标流程是在创建时共同确定颜色空间、纹理格式、shader 输出和 pipeline attachment 格式，最后由 DPU 读取同一单平面 BGRA backing。

本轮进一步静态读取了本机 iPadOS 21H461 的 QuartzCore、RenderBox。输入位于工作区 `out/t733-lldb-symbols/extracted/`，以下地址均为文件中的未加运行时 slide 的地址。

### 设备描述怎样改变系统自动选择

已经闭合以下调用链：

1. MobileGestalt 的 `ArtworkTraitDisplayGamut` getter `0x1a02115f0` 读取 `IODeviceTree:/product` 的 `artwork-display-gamut`。
2. `HasExtendedColorDisplay` getter `0x1a0207ab0` 调用上述 getter，与 `P3` 做字符串比较。字段为 `sRGB` 时，这个能力答复为 false。对应的查询键为 `Aixt/MEN2O2B7f+8m4TxUA`。
3. QuartzCore 的 `___CADeviceUseCIF10_block_invoke` `0x18957be58` 读取该 bool，保存到 `0x1f491ae10`。仅当 MG 答复为 true 时再读取 `CAEnableDeepFramebuffer`；有效的偏好值覆盖该 bool。
4. `IOMFBDisplay` 构造函数在 `0x1896e719c-0x1896e71b0` 将该值写入对象 `+0x238` 的 bit 3。最终 `framebuffer_pixel_format()` 在 `0x18953053c` 读取此位；清零时进入 `0x189530544` 的 `BGRA` 候选路径。该函数在此前还有 EDR、调试配置和显示压缩相关分支，完整条件见下节。

因此，设备描述可以控制系统的 CIF10 选择。设备配置的具体入口是 `board.toml` 已有的 `[adt.product.string]` 表，设置 `artwork-display-gamut = "sRGB"`。当前公共 OS profile `profiles/os/ipados-17.7.11-21H461/profile.toml:79` 为 `P3`，设备表未覆盖该键。`scripts/unixnu/profiles.py:10` 的递归合并使设备字段覆盖 OS 默认；`devicetree.py:295` 将字符串编码为带 NUL 的 ADT 数据。这个修改保持现有 iPad 型号和 GPU family 答复。

MobileGestalt 缓存也必须随描述更新：21H461 的缓存检查使用 MG build UUID 和系统 build，未把 `artwork-display-gamut` 作为失效条件。应移除保留 Data 中 `containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist` 及已有的 mobile 同名偏好缓存，再让消费进程在新启动中重新初始化。设备的 `seed-data-skip-setup.py:82-84` 已列出三个缓存路径；保留现有 Data 的普通部署需单独处理这些缓存文件。

**当前采样已经提供了额外限制：60a 的 backboardd 插件记录为 `6a38`。** 证据是工作区 `logs/t733-usb-live/t733-20261027T181353.446480Z.raw:3674-3725`：该段 client 为 backboardd，marker 从第 3684 行开始多次出现。插件 `NativeAdrenoMetalPlugin.mm:234-248` 中，bit 3 表示 MG 答复存在，bit 2 表示答复是 true；`0x38` 的 bit 2 为零，且 bit 0 表示的 deep 偏好未设置。因此，现有记录没有显示 backboardd 仍通过该 MG 答复开启 CIF10，不能给仅修改 gamut 预先分配本轮卡顿的性能收益。

另一个已有配置入口是公共 `scripts/add-gpu-plugin-data.py:94-106`：发布 GPU 时向 Data 种子的 mobile/root、backboardd 和全局偏好写入 `CAEnableDeepFramebuffer=false`。源码里存在这段逻辑并不代表保留 Data 已应用；上述运行记录应优先用于解释 60a 的实际行为。

方案由此分成两个具体动作：将 SM-T733 的显示描述固定为 sRGB，并同步更新 MG 缓存；继续针对 RenderBox 的普通 SDR 双平面自动选择落实格式策略。显示描述确定色域和 CIF10 能力，RenderBox 的 SDR 布局选择还由下节的 family 与调用 flags 决定。

### RenderBox 离屏表面

`RB::ColorMode::pixel_format` 位于 `0x1e26282b0`，同时包含返回 80/81，即 BGRA8 UNORM/sRGB，及返回 500/520、502/522、523/542 等双平面格式的分支。

`RB::Device` 在 `0x1e262ae1c` 开始依次调用 `supportsFamily:`，参数从 1001 开始，把最高连续支持的 Apple family 保存到对象 `+0x119`。上述格式选择函数在 `0x1e2628388` 读取这个值并检查是否至少为 3。因此，RenderBox 的双平面选择还由 GPU family 答复驱动。

当前 `NativeMetalObjects.rs:387-394` 的 `TARGET_FAMILIES` 包含 Apple1/2/3 与 Common1/2；`:475` 将该列表与 runtime 逐项能力判断合并。该处记录的系统调用关系包括 Core Image 选择 Metal 路径，以及 QuartzCore 在 Apple3 条件下选择提交方式。修改 family 答复会同时改变这些路径，格式策略应与完整的 family 能力评估一起设计。用户另行要求按官方 Metal Feature Set Tables 评估 A642L，结论保存到 [A642L Metal family 研究](A642L_METAL_FAMILY_RESEARCH_2026-10-28.md)。

已追到的普通颜色模式分支为：颜色模式字节为 0、调用 flags 的 `(flags & 0x1a) == 2`、bit 0 为 0、设备 Apple family 至少为 3 时，进入私有双平面候选。该分支内 flags 的 bit 2 会跳过若干双平面候选并进入已有普通格式路径。继续实施前，应沿调用者确定这个 bit 的实际含义与传递条件，以该函数的现有 BGRA8 路径作为源头优化入口。

进一步追到 `-[RBLayer displayWithBounds:callback:]`：`0x1e2627270` 读取 `allowsDisplayCompositing`，在 `0x1e2627578-0x1e2627580` 据此设置上述 flags 的 bit 2；`promotesFramebuffer` 则影响 bit 5。这些 flags 同时携带调用用途，实施时保留用途语义。

`-[RBLayer rb_init]` 在 `0x1e263d690` 将 `_colorMode` 初始化为 0；`ColorMode(0)` 保留 depth 0 和属性 flags 0。这种普通颜色模式在 opaque、调用 flags 为 2、Apple family 至少为 3、`allowsDisplayCompositing=false` 时，仍能自动选择 `RGB8_444_2P` 542。该具体分支说明，将设备色域改成 sRGB 后仍有 SDR 双平面表面的来源。

该函数还存在显式格式入口：`0x1e26275f8` 调用 `-[RBLayer pixelFormat]`，返回非零时在 `0x1e2627614` 跳过自动 `ColorMode::pixel_format` 选择。二进制中存在 getter `0x1e26289a8` 和 setter `-[RBLayer setPixelFormat:]` `0x1e26a1424`。这可作为确定需要覆盖 SDR 布局时的实现入口；仅有 setter 证据尚未闭合系统创建者如何采用该策略。实施优先级仍是设备能力及自动选择关系。

另三个 `ColorMode::pixel_format` 调用者是 `make_image_provider`、`RBLayer copyImageInRect:options:completionQueue:handler:` 的异步 block、`RBSurface _updateWithDevice:frame:synchronized:`。它们分别传入自己的 flags，图层入口的设置覆盖范围需要按这些调用者分别确认。

格式值向下传递的已确认入口如下，实施时让同一个选择贯穿这些对象：

| 层次 | 接收和传递格式的操作 |
| --- | --- |
| 系统图层 | `RBLayer displayWithBounds:callback:` 得到 `pixelFormat`，在 `0x1e262783c` 将该值交给 `SharedSurfaceGroup::add_subsurface`；另一路随 drawable 渲染回调保存 |
| 系统纹理分配 | `RB::Device::alloc_shared_texture` 按格式、尺寸、颜色空间、usage 查已有纹理；需要分配时在 `0x1e262d358` 交给 `RB::Texture::alloc` |
| Metal 纹理前端 | `NativeBridgeMetalFrontend.mm:1406` 的 `newTextureWithDescriptor:iosurface:plane:` 接收 descriptor 和原 Surface 布局；两个及以上 plane 调 `textureOnSurface`，普通 BGRA 调 `textureOnBuffer` |
| Rust bridge | 普通 BGRA 经 `nmb_texture_new_on_buffer` 和 `IoSurfacePlane::new(..., false)`；双平面经 `nmb_texture_new_on_surface` 增加第二平面布局。两条路径都把原 descriptor 格式传给 `new_texture_with_iosurface_plane` |
| Metal pipeline | frontend `:1173` 把每个 color attachment 的 pixelFormat、混合和写掩码交给 pipeline 描述；与实际纹理格式共同确定输出 |
| Vulkan/Turnip | image plan 和 pipeline 使用上述格式；BGRA8/sRGB 走 A6xx 原生颜色格式，双平面格式按前表进入各自转换 |

这个流程使原 Surface 的内存布局、shader 颜色解释、pipeline attachment 和实际图像一致。CAMetalLayer 和系统私有图层的选择都在创建资源前落实。

`RB::ColorMode` 构造还读取 `RB_COLOR_MODE`。其字符串解析器位于 `0x1e262a558`，接受 `deep`、`wide`、`extended`、`fixed`、`linear`、`nonlinear`、`5bpc`、`422`、`420`，分隔符为 `-,; `。`fixed` 把模式字节设为 0；上段的双平面选择仍会检查其他 flags，因此它只是一个已找到的控制入口。设置是在首次初始化时缓存的，实际采用方案需覆盖框架第一次使用前的初始化顺序。

### QuartzCore 最终 framebuffer

`CA::WindowServer::IOMFBDisplay::framebuffer_pixel_format` 位于 `0x18953043c`。它检查 EDR headroom、显示对象中的状态以及缓存配置，实际包含选择 `BGRA`、`b3a8`、`RGhA` 和 `w40a` 的分支；BGRA 选择在 `0x189530544`，b3a8 选择在 `0x189530530` 及 `0x18953057c`。最终返回前部分路径还调用显示对象的格式处理方法。

显示采用 SDR 的目标已确定。下一步沿这个函数的状态来源统一显示能力和 framebuffer 选择，同时处理上面的 RenderBox 离屏入口。精度更高的滤镜中间目标仍按具体运算需要选择，输入视频的 plane 格式则继续由对应视频路径解码。

进一步解码了前置条件与最终格式处理：

- `0x1f491b3e8` 是 `_ca_debug_options[0x78]`，名称 `CA_FORCE_FP16_FRAMEBUFFER`；启用时直接选 `RGhA`。它由 QuartzCore 调试配置初始化。
- 在读取 CIF10 bit 3 之前，函数先检查 `supports_icc_color_management()` 和 `CADeviceSupportsUniversalCompression`；两者为 true 时可直接选择 `b3a8`。因此 `HasExtendedColorDisplay=false` 的效果需要与这个独立分支一起解释。
- `CADeviceSupportsUniversalCompression` 从 MG 的 `IOSurfaceFormatDictionary` 取 `universal-buffer-compression`。MG getter `0x1a020a790` 根据 `/arm-io/soc-generation` 的代际字符串生成整个字典；缺少 generation 时返回空字典。`universal-buffer-compression` 在这里是输出键，直接向 ADT 添加同名 false 属性不会改变该 getter 的答案。`6a38` 只记录字典存在，未记录这个键的值。
- `CAEnableFramebufferCompression=false` 控制后续压缩选择，不能据此认定前面的 universal-compression 分支已经关闭。最终 `compressed_pixel_format()` 还要按实际显示对象的方法处理候选 fourcc。

普通 SDR 显示采用 BGRA 的自动路径需要以下事实同时成立：CIF10 关闭；EDR/强制 FP16 分支未选中；ICC 加 universal-compression 分支未选中；最终格式处理保留普通 BGRA。现有静态证据已定位每个条件，其中 MG/gamut 是可直接由设备描述修正的一项。单平面 BGRA 的 fourcc 与 Metal 的 80/81 视图分别表达内存布局和采样/写入颜色解释，sRGB 描述需继续传递到所选颜色空间与 texture view。

HDR 描述也有独立来源：`DeviceSupportsHDRPhotoDisplay` 检查 `/product/supports-meteor` 是否存在，存在即启用；否则查询 OLEDDisplay。`RenderWideGamutImagesAtDisplayTime` 根据设备类别与 `soc-generation` 生成。后续设备能力表应按每个已解析 getter 的实际来源填写；不能用修改 GPU family 或伪造 SoC 代际来代替这一过程。

### 保存的静态证据

- 工作区 `logs/gpu-stack-policy-research-20261028/renderbox-color-mode.txt`：格式选择和构造函数。
- 同目录 `renderbox-color-mode-options.txt`：`RB_COLOR_MODE` 解析器。
- 同目录 `renderbox-device-family.txt`：Apple family 查询和字段写入。
- 同目录 `renderbox-layer-format-inputs.txt`：图层用途 flags、显式 pixelFormat 和自动选择的分支。
- 同目录 `renderbox-layer-initialization.txt`：RBLayer 默认颜色模式初始化。
- 同目录 `mobilegestalt-display-getters.txt`：MG getter、实际 ADT 字段及 `P3` 比较。
- 同目录 `mobilegestalt-display-policy.md`：MG 显示、HDR、压缩字典和缓存的完整研究。
- 同目录 `quartzcore-framebuffer-format.txt`：最终 framebuffer 分支。
- 同目录 `quartzcore-framebuffer-policy-chain.txt`、`quartzcore-policy-data.txt`：MG/pref 到 CIF10 状态位、独立格式分支、调试配置和显示虚函数。

复读命令使用 `xcrun llvm-objdump --disassemble --demangle --no-show-raw-insn --start-address=... --stop-address=... <binary>`。本轮仅在主机读取已保存的文件；格式选择方案尚未在设备应用。

来源：[UniXNU MobileGestalt 设备描述](https://unixnu.com/guide/mobilegestalt)、[CAMetalLayer.pixelFormat](https://developer.apple.com/documentation/quartzcore/cametallayer/pixelformat)、[CALayer.contentsFormat](https://developer.apple.com/documentation/quartzcore/calayer/contentsformat)、[CAMetalLayer.wantsExtendedDynamicRangeContent](https://developer.apple.com/documentation/quartzcore/cametallayer/wantsextendeddynamicrangecontent)。缓存校验细节采用本机 21H461 二进制证据。

## Metal API 顺序如何映射到成熟驱动

设备使用的 AdrenoMetal revision 为 `0d69cf81e3d1e9e0cc897bf3049e480c94bc4556`。设备 `support/UniXNU.rev` 指向的 core gitlink、实际模块 HEAD 和组件表均对应这一版本；设备启用 AdrenoMetal，沿用公共 core 的 frontend 和 bridge。

**普通 render draw 的附件操作已经使用 draw 顺序和固定功能顺序。** 当前 `render.rs:51` 将 render encoder 的一般 shader-memory 访问设为 unordered；`tracker.rs:992` 在这种 encoder 中直接返回无自动 barrier，`:1002` 继续累计实际资源使用摘要。因此当前代码没有为每个普通 draw 自动增加一般资源 barrier。

| Metal 调用或场景 | 顺序与依赖 | 采用的驱动策略 | 当前接入情况 |
| --- | --- | --- | --- |
| 同 render encoder，附件写入、固定 blending、depth/stencil | 同像素附件操作按 primitive order 处理 | 保持 draw 顺序和 render pass；连续普通 draw 无须通用 memory barrier | 已实现 |
| 同 render encoder，shader 写 buffer/texture 后由另一个 draw 访问 | 应用通过 render memory barrier 指明 scope 和 producer/consumer stages | 直接生成 stage/access 的 `VkMemoryBarrier2`；附件局部依赖优先留在 pass 内 | runtime 已实现，Objective-C frontend 和 C ABI 缺 render barrier 入口 |
| Serial compute 连续 dispatch | 后续 dispatch 使用前面 dispatch 的结果 | 对有冲突访问产生执行和内存依赖 | 当前支持 serial；scope barrier 允许忽略 |
| Concurrent compute | dispatch 允许重叠，应用表达必要 barrier | 按 compute scope 生成依赖 | frontend 明确拒绝创建，尚未支持 |
| 同 queue，跨 encoder，tracked 资源 | Metal 自动维护访问冲突 | 编码时合并资源使用摘要，在确定执行顺序后处理摘要中的冲突 | runtime 已有增量摘要和 tracker |
| 同 queue，跨 command buffer | enqueue 建立 queue 次序，tracked 访问继续建立依赖 | queue 内 Vulkan barrier 可覆盖先前提交，依赖在 GPU 上执行 | tracker 保存跨批次状态；Darwin CPU wait 另按下一节改造 |
| Untracked 资源，跨 encoder | 应用使用 fence 等命令表达依赖 | 把 update/wait 的阶段传到对应 GPU 依赖 | frontend 传了 stages，runtime 将其扩大为整个 encoder |
| 跨 queue 生产者和消费者 | 显式 event value 等建立有向依赖 | timeline wait/signal 随任务提交 | runtime 能生成跨 queue wait；Darwin 最终仍在 CPU 等待 |
| Argument buffer 等间接引用配合 `useResource` | 调用提供驻留、usage 和 render stages 信息 | 调用时登记引用与使用摘要 | compute/render frontend 的 `useResource` 均为空；直接绑定路径另有资源记录 |

这些规则对应当前 iPadOS 使用的 Metal 编程模型。设备直接分配默认 tracked，heap 分配默认 untracked；实际模式继续沿创建参数传递。

### 可以直接采用的成熟策略

1. **Metal 转换层决定依赖，Turnip 选择硬件操作。** `tu_barrier` 接受 Vulkan 已提供的 stage/access；`tu_flush_for_access` 用 `pending_flush_bits` 增量维护访问类别，遇到消费访问时选择所需操作，发出后清除相应状态。该阶段不需要重新遍历资源两两比较。
2. **显式 scope barrier 使用固定大小的访问描述。** `VkMemoryBarrier2` 可以描述全部 buffers/textures 的阶段和访问类型，生成它无需扫描一个资源列表。
3. **tracked 自动同步在调用和编码时累计。** 资源身份、别名和绑定关系在创建、绑定、解除时维护；每个 encoder 累计实际使用的 key/range/read/write。提交顺序确定后只处理这些摘要。当前 tracker 已是这个方向，后续优化具体针对摘要、range 和 alias 的重复遍历及分配。
4. **保留 fence 指定的阶段。** PanVK 把 stage mask 对应到受影响的 vertex/tiler、fragment、compute 子队列；在 A6xx/Turnip 中采用其保留生产与消费阶段的策略，由 Turnip 选择本硬件的实现。
5. **命令顺序、自动 hazard 与资源寿命分别落到对应对象。** BO 引用列表供提交保留和回收内存；barrier 从 Metal 顺序及访问要求生成。显示生产者关系来自该 Surface 的写入完成对象。

本机 tracker `plan()` 当前遍历 summary 中实际使用的资源，并非扫描整个设备的全部资源。单 summary 直接查既有状态，多 summary 只复制所涉及资源的状态；完成回收只处理该批次触及的资源。若以整个 encoder 的固定 barrier 代替全部自动跟踪，会扩大无关工作的依赖范围。因此本轮目标同时包括减少 CPU 遍历和保留 GPU 的可重叠执行。

### 已确认的落实点

- `NativeBridgeMetalFrontend.mm:727` -> `NativeMetalObjects.rs:2039` -> render encoder：draw 入口。
- runtime `encoder.rs:523`、`tracker.rs:992`、`:1002`：共享存储别名和编码期访问摘要。
- `NativeMetalObjects.rs:1499` -> runtime commit；`tracker.rs:1753` 与 `:1708`：提交计划与完成回收。
- frontend `:740-741` -> bridge `:1646` -> runtime `fence.rs:190`、`:204`：render fence 的具体 stages 在 runtime 被丢弃，等待扩大到整个 encoder。
- runtime `fence.rs:212`：render barrier 已支持 scope、资源列表和阶段。render-target 且 fragment 到 fragment 的路径使用 `barrier_between_draws`；其他当前实现可能切分 pass。frontend 的 `NBRenderEncoder` 至 `:742` 没有对应 selector，C ABI 也未接入。
- frontend `:640`、`:739`：compute/render `useResource` 空入口，间接资源声明需接入引用和访问摘要。
- frontend `:863-864`：非 serial compute 返回拒绝；`:641` 的 compute scope barrier 空实现符合当前 serial 模式规则。
- runtime `command_buffer/submit.rs:313`、`:467` 和 `tracker.rs:305`、`:317`：每批固定的 `after_completed` 与 `to_host`。应按实际 batch 边界、host 访问需求和共享资源出口研究合并范围。
- Mesa `tu_cmd_buffer.cc:10125`、`:5854`、`:483`：`tu_barrier`、`tu_flush_for_access` 和 pending 操作的发出与清除。

render barrier 和间接资源入口的覆盖缺口已经由源码确认；现有记录尚未提供 SpringBoard/backboardd 对这些 selector 的调用频次。render fence 阶段扩大和固定首尾 barrier 也已确认存在，其操作期时间需要按具体提交记录。

来源：[Apple Resource synchronization](https://developer.apple.com/documentation/metal/resource-synchronization)、[MTLCommandBuffer](https://developer.apple.com/documentation/metal/mtlcommandbuffer)、[render memoryBarrier](https://developer.apple.com/documentation/metal/mtlrendercommandencoder/memorybarrier(scope:after:before:))、[Apple bindless Metal](https://developer.apple.com/videos/play/wwdc2022/10101/)、[Vulkan 同步规范](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)、[附件操作顺序](https://docs.vulkan.org/samples/latest/samples/extensions/rasterization_order_attachment_access/README.html)、[Turnip 命令实现](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/freedreno/vulkan/tu_cmd_buffer.cc)、[PanVK 队列实现](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c)。

## 等待对象与内核调度设计

目标是提交线程交付任务及依赖后返回，内核根据依赖和硬件空间发出任务，GPU 完成唤醒具体等待者，显示只等待当前 Surface 的生产者。以下是基于 MSM、KGSL、DRM scheduler 和 SDE 的待实施设计。

| 对象 | 保存的具体关系 | 完成条件 |
| --- | --- | --- |
| Metal command buffer | 所属 queue、产生的 Vulkan 提交、最终完成对象 | 全部相关 GPU 工作完成后发 completed 回调 |
| Vulkan timeline point | semaphore、64 位 value、关联的完成对象或汇合结果 | GPU 完成或合法 host signal 满足该 value |
| 内核 job | 软件 queue、输入依赖、输出完成对象、IB、资源引用 | 接受时保留引用，等待依赖期间也保留 |
| 已发出的 job | ring 身份、复位代数、软件序号、硬件 32 位 fence | PM4 和 WPTR 已发布 |
| 共享 Surface backing | 稳定分配身份、最后写者、尚未完成的读者 | 按实际 backing 和访问范围更新 |
| DPU present request | 当前 Surface、生产者、DPU 读占用、呈现通知 | 生产者完成后发出，硬件接收切换后通知呈现 |

内核接受、写入 ring、GPU 写 fence、CPU 收到通知、DPU 接收切换、旧 Surface 不再被扫描，各有独立时点。当前 Darwin sync 保存 `state + uint32_t fence`，timeline 由 Mesa 包装；多个 chunk 的最终 signal 关联最后 fence。实现异步提交时保留多笔 GPU 工作的汇合语义，timeline value 与硬件 fence 分别编号。

### 依赖如何调度

1. 已完成依赖直接满足。
2. 生产者已写入同一有序 ring 时，消费 job 可排在其后，省略 CPU 等待完成；由翻译层生成的必要 GPU 依赖仍在命令流中执行。
3. 生产者仍在同一调度器的软件队列时，先确认它已发到 ring。DRM scheduler 对这种关系使用 scheduled 完成点。
4. 其他引擎、外部设备、host signal 和尚未出现的 timeline point 由完成回调重新唤醒调度。各软件 queue 内保持原顺序，等待中的 queue 让出调度机会给其他就绪 queue。
5. 显式 CPU 等待使用带超时的睡眠等待。空提交的输出 signal 关联输入依赖和此前 queue 工作。

当前单 ring、单 priority、无抢占，调度选择发生在任务写入 ring 前。依赖将来才会被软件提交的工作，应在软件队列中等待，避免占住唯一 ring 后阻止 signal 的生产者运行。

本机 `tu_knl_darwin.cc:2558` 的 `darwin_queue_submit()` 将输入依赖逐个转成 CPU 等待，是改造入口。参考同代 `msm_gem_submit.c:450`、`msm_gem.c:702` 的同 ring 判断；异步依赖采用 `kgsl_drawobj.c:210`、`adreno_dispatch.c:392` 及 `sched_entity.c:402`。本机旧 MSM 对 foreign fence 仍有同步等待，这一段采用 KGSL 和通用调度器的回调设计。

### Surface 的生产者与扫描占用

导入时建立持续存在的 backing 对象，将不同进程 handle 和 descriptor 对同一分配的别名归到该对象。身份包含分配代数和完整范围。读依赖此前写者；写依赖此前写者和未完成读者。跨进程与 DPU 交接消费当前任务的共享资源访问摘要，内部 draw 的 barrier 仍在 Metal 到 Vulkan 的转换层决定。

显示接收 Surface 时，在一次受保护操作中取得当前写者并登记 DPU 读占用。生产者完成后直接交给显示 workloop。DPU 切到新画面时解除旧画面的读占用；新画面一直占用到下一次切换。多平面或独立写者尚未汇合时保留完成对象集合。

61a 的 `SurfaceWrite` 是有界 profiling 历史，正式依赖保存在 backing 对象中。对应成熟实现为 `dma_resv` 写者/读者和 `msm_atomic_prepare_fb:512` 从当前 framebuffer BO 取得生产者 fence。当前 `NativeAdrenoPublish.cpp:1991` 的 `lastSubmitted()` 应由这条具体 Surface 关系替换。

### IRQ、容量与资源寿命

`NativeAdrenoIrq.h:22` 明确当前对象只解码寄存器，尚未接入 XNU interrupt source。设备树已有 GPU `<0 300 4>`、HFI `<0 304 4>` 和 GMU `<0 305 4>`；`package/T733GpuResources.cpp:190` 当前只提取 SMMU 中断。应沿现有 DT 中断转换和 ArmCpu 注册过程接入 GPU IRQ。

硬 IRQ 确认状态并记录事件，工作线程按完成 fence 退任务、唤醒依赖与 CPU 等待者、归还硬件容量、释放引用。已完成检查和回调登记遵循同一锁规则。DPU 在真实 vblank IRQ 中检查该次 CTL flush 已被接收，再通知呈现和解除旧 buffer 占用。正常完成通知退出 1 ms 定时轮询，计时器承担故障超时。

当前全局最多 8 个活动 job，Darwin Busy 重试却只等本连接最老 fence。改为内核维护有界软件队列和全局硬件容量，依赖全局完成/空间事件继续调度。临时无空间与命令本身超过 ring 容量分别返回；重试沿用原请求的 job 和输出完成对象。

BO 回收同时考虑已接受的 job、运行中的 GPU job、DPU 扫描及 CPU/用户引用。GPU 完成释放 GPU 引用，DPU 切换释放旧画面的扫描引用。复位和错误使相关完成对象带错误终结；DMA 停止后再回收其内存。硬件 fence 与 ring 身份、复位代数和软件序号共同解释。

成熟源码位于工作区 `references/android_kernel_samsung_sm7325/`，固定 revision `36b842cf1fc87a989dcc8158a142d7575cee41c1`：

- `drivers/gpu/drm/msm/adreno/a6xx_gpu.c:137`、`:723`，`drivers/gpu/drm/msm/msm_gpu.c:706`：fence、IRQ 和 retire worker。
- `drivers/gpu/drm/scheduler/sched_entity.c:402`，`sched_main.c:596`、`:648`：scheduled 依赖、容量及完成唤醒。
- `drivers/gpu/msm/kgsl_drawobj.c:210`、`:292`，`adreno_dispatch.c:392`、`:779`：异步依赖到期和重新调度。
- `drivers/gpu/drm/drm_syncobj.c:316`、`:937`：等待时间点出现与等待完成。
- `techpack/display/msm/msm_atomic.c:512`，`sde/sde_encoder_phys_vid.c:541`：具体 framebuffer 生产者和 vblank/flush 完成。

## 继续落实时需要取得的操作期证据

| 项目 | 要取得的具体结论 |
| --- | --- |
| 每个操作阶段的格式 | Surface/texture 格式、尺寸、读写用途、转换次数和字节数，将 b3a8、RGB raw-code、converted YCbCr 分开 |
| 系统格式选择 | 已定位函数的调用 flags、颜色模式、family 答复；源头选 BGRA8 后实际创建的 Surface、pipeline 格式及色彩结果 |
| Metal 同步调用 | fence、event、render barrier、useResource 的真实调用、scope、stages、queue 和 command buffer 关联 |
| barrier 生成成本 | 每批 encoder/summary/range/alias 数量，固定首尾 barrier 和 fence 扩大范围各自造成的时间 |
| 等待与寿命 | 分别记录接受、发出、GPU 完成、通知、DPU 切换和旧 Surface 释放；测量 CPU 空档与就绪任务数量 |

后续操作期采样应采用有界记录和汇总，关联系统 Metal command buffer 与内核 job。GPU 性能采集同时启用相关硬件计数器和 shader profiling，并从保存内容确认有效测量。本轮完成了静态研究与文档，尚未实施上述代码改造或新增设备采样。

相关前序记录：[等待审计](GPU_FENCE_WAIT_AUDIT_2026-10-28.md)、[61a 帧记录](GPU_FRAME_PROFILE_61.md)、[CPU 缓存维护移除](GPU_CPU_CACHE_REMOVAL_60.md)。
