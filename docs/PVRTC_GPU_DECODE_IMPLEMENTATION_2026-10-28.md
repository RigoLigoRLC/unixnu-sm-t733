# PVRTC GPU 解码与缓存采样

本轮为 metal2vulkan 固定版本 `720ef1ce9ae2a2068999a8675717418b9416283e` 增加 PVRTC GPU 解码。源码保存于 `AdrenoMetal/bridge/patches/0014-pvrtc-gpu-decoded-textures.patch`；`texture.rs` 与同步工作共用的变更按文件整体归入 `0011-preserve-metal-stages-and-host-visibility.patch`。构建入口已经登记补丁，未执行构建、测试或设备 probe。

## 数据经过哪些对象

`pvrtc.rs::plan_blocks` 继续创建保存原始 8 字节压缩块的 Vulkan carrier image。4bpp 的块覆盖 4x4 texel；2bpp 的块覆盖 8x4 texel，所以后者的 carrier 宽度是 Metal 纹理宽度的一半。CPU getBytes、replaceRegion 和 GPU blit 继续使用原来的块顺序与区域转换。

`Device::with_decoded_twin` 为该 image 创建同 mip 数、同 array/cube layer 数、完整 Metal texel 尺寸的 RGBA8 image。两个 image 的访问由原纹理的资源标识排序，纹理及其 views 共同保留解码 image。新建 RGB 纹理的零压缩块对应黑色、alpha 1；RGBA 对应全零。heap 中已有内容通过 `decode_whole` 初始化解码 image。

`Texture::facts` 向采样绑定传递解码 image 的 RGBA8 格式。`decoded_view` 返回缓存 Vulkan image view，linear 格式使用 RGBA8 UNORM，sRGB 格式使用 RGBA8 SRGB。只改变 sRGB 解释的 Metal texture view 继续共享原压缩块和解码 image；view 的 mip、layer、swizzle 参数照常传入。argument buffer 的 texture handle 也通过这一映射取得可采样 view。

## 解码和更新

新增 compute shader `src/texture/pvrtc_decode.comp` 根据 PowerVR 官方 [PVRTDecompress.cpp](https://github.com/powervr-graphics/Native_SDK/blob/master/framework/PVRCore/texture/PVRTDecompress.cpp) 的端点、调制与插值规则实现，版权与 MIT 许可保存在相邻 `PVRTC-LICENSE.txt`。

| 格式与操作 | 处理方式 |
| --- | --- |
| RGB/RGBA 4bpp | 解出 A/B 端点、双线性插值、四值调制；RGBA 处理 punch-through alpha，RGB 输出 alpha 255 |
| RGB/RGBA 2bpp | 处理直接调制、棋盘格存储值、水平/垂直/四邻域插值；邻块坐标按 mip 层块数循环 |
| sRGB | shader 输出原始 8-bit 颜色码，采样 view 执行 sRGB transfer；alpha 保持线性 |
| mip 与 array/cube | 每个被写入的 mip level 和 layer 独立解码；最小 mip 的邻块按实际块数循环 |
| CPU replaceRegion | 原压缩块上传和 GPU 解码进入同一提交队列，避免 CPU 解码与 GPU 写入的并发覆盖 |
| buffer-to-texture / texture-to-texture blit | 既有 `decode_twin` 写后入口创建 GPU pass，解码当前 carrier 中的块 |
| 区域更新 | 刷新该区域所属的完整 mip level/layer；PVRTC 相邻块会影响区域外 texel，整层刷新包含这些依赖和循环边界 |
| 纹理重复采样 | 直接使用已有解码 image/view，采样本身不触发重新解码 |

`texture/decoded_twin/pvrtc.rs` 将一个 mip 层拆成横跨完整宽度的条带。每条读取包含上下相邻行的压缩块；跨 mip 顶部或底部时分成两次 image-to-buffer copy。compute dispatch 生成条带的 RGBA8 texel，再执行 buffer-to-image copy。复制、compute 和后续采样沿用已有 `DecodePass` 屏障。临时 blocks/texels 总尺寸最多 32 MiB 加 descriptor alignment，不分配整张解码纹理大小的临时 buffer。

更新范围扩展为完整 mip 层是一项具体成本：大型 PVRTC 纹理频繁进行小区域写入时，会解码该层的所有 texel。缓存仍在没有写入时复用。这里优先保证端点插值和当前压缩数据一致。

## 能力与构建输入

`families/formats.rs` 的 PVRTC Filter 答复现在同时检查 carrier 的复制能力、image limits、解码 RGBA8 image 的采样/线性过滤/transfer-destination 能力。纹理实际取得 decoded twin 后，handle 表和 `degrade_unreadable` 不再把它作为只可复制的 carrier。

本实现覆盖 runtime 自有纹理和 heap 自有 image 的现有 decoded-twin 创建路径。外部 Vulkan image 或调用者提供的 host heap pages 沿用其专门的内存映射路径；没有 decoded twin 的对象继续返回原拒绝或明确报告不可读 handle。iPadOS 17 公共 API 对 PVRTC 的 CPU replaceRegion 要求整 mip 上传，现有 runtime 对该入口的限制保持；GPU blit 的合法块区域写入能够更新采样结果。

runtime `build.rs` 在构建主机调用 `M2V_GLSL_COMPILER` 编译 checked-in GLSL，输出到 Cargo `OUT_DIR` 后编入 runtime。核心 `build-adreno-metal-bridge.py` 自动选择已有 `.cache/mesa-tools/bin/glslang`；可显式覆盖该环境变量。GPU runtime 只创建 Vulkan shader/pipeline，不运行外部编译器。shader、许可、Rust glue 和 build.rs 都包含在补丁与现有源输入识别中。

主代理统一完成提交、构建与设备验证后，再记录编译和运行结果。family 的最终结论仍需结合其余 Apple3 表项与 iPadOS 17 范围审计。
