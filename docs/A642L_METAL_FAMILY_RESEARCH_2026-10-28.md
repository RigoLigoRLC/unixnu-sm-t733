# A642L 的 Metal GPU family 能力边界

日期：2026-10-28。对象：SM-T733 / SM7325 / Adreno 642L，当前 iPadOS 17.7.11。

本研究读取 Apple 指定的官方 PDF、当前 Mesa/Turnip 源码、metal2vulkan runtime 和 AdrenoMetal Objective-C 前端。没有改驱动、执行测试、构建、设备 probe 或计算文件确认散列。文中的 Vulkan 值是本机 chip ID 走当前源码分支得到的值；本轮没有运行新的 Vulkan capability dump。

## 结论

**如果需要一个用于移植决策的代数，A642L 最接近的完整实现目标是 Apple3。Apple4 是第一个需要补建整套 tile/imageblock/像素访问顺序处理的分界。** 这是依据功能能否映射到现有 A6xx 图形和计算管线作出的工程判断。A642L 的多视口、64 倍细分、图形阶段 subgroup、ASTC HDR、BC 和 FP32 纹理过滤等单项已经分别覆盖 Apple5、6、7、9 的要求，不能把这些单项合并成“完整 Apple7/9”。

当前三个问题的答案如下：

| 问题 | 答案 | 决定结论的具体事实 |
| --- | --- | --- |
| 硬件可直接覆盖到哪一层主干？ | Apple3 的传统顶点/片元/计算/细分路径最接近；较新的若干单项也有硬件支撑 | 16K 纹理、8 个颜色附件、4x MSAA、细分和间接 draw/dispatch 均有 A6xx/Turnip 实现。Apple4 的完整显式 imageblock、tile dispatch 和 raster order groups 没有现成对应实现 |
| 全部表项都直接由硬件提供时，能否宣称某一整代？ | 不能据此宣称完整 Apple2 或更高 | PVRTC 没有原生格式路径；64 位整数由编译器拆成较窄操作；Apple3 的 texture-buffer 长度还超过当前 Turnip 暴露值。这个结论描述完整集合，不代表 GPU 的常规运算能力低于 Apple2 |
| 当前 Turnip + runtime + Objective-C 前端已完整暴露到哪代？ | **没有一个 Apple family 已满足完整集合** | Apple2 已有 PVRTC 采样等缺口；Apple3 再有 depth resolve、细分边界、间接命令和 256M texel buffer 缺口。前端强制返回 Apple1/2/3 |
| 利用现有和待实现的软件路径，最先可争取完成哪代？ | **Apple3 是具体可实施的首个完整目标** | 本文列出格式解码、buffer 访问扩展、resolve 和 Metal API 连接工作。完成后仍须按当前 OS 可调用的完整表项判断 |
| 能否继续到 Apple4/5 或更高？ | 可以继续设计模拟，但现有代码没有完成任何一个更高 family | Apple4/5 要先完成 tile/imageblock/MSAA/ROG；Apple6 再增加 sparse 和软件 ray tracing；Apple7 增加完整 mesh/matrix 路径；Apple9 增加 64 位原子。任意增加软件执行器没有一个可由芯片型号单独推出的最高代数 |

这里的 Apple3 目标不是把 `supportsFamily:` 改为 true。该值当前已经是 true。目标是让目前被强制放行的表项有完整可调用的实现。

## 官方表的版本和 iPadOS 17 的范围

用户指定的 [Metal Feature Set Tables PDF](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf) 本次取得的版本日期是 **2026-05-21，共 18 页**。本地留存于 [研究采集目录](../../../logs/gpu-stack-policy-research-20261028/Metal-Feature-Set-Tables-2026-05-21.pdf)，第 3、4 页已渲染阅读。PDF 的功能表在第 3-6 页，limits 在第 7-10 页，格式能力在第 11-18 页。

这版表从 Apple2 列到 Apple10，没有 Apple1 列。其第 6 页说明，`Metal 3 programming model` 合并 Metal 1、2、3 的 API，所有 Apple families 都能使用其中适合自身的 API；`Metal 4 programming model` 从 Apple7 起。这个表头不等于 `MTLGPUFamilyMetal3`。Apple 的 [Metal3 family 文档](https://developer.apple.com/documentation/metal/mtlgpufamily/metal3) 明确列出 iPadOS 上 A14/M1 起的硬件，枚举从 iPadOS 16 可用。

因此，iPadOS 17.7.11 提供 Metal 3 时代的系统框架，并不使本机自动满足 Apple7 或 `MTLGPUFamilyMetal3`。同样，2026 表中新出现的 MTL4 command allocator、argument table、tensor 等 API 不属于这次 iPadOS 17 前端应实现的范围。2026 表中同时标注 Metal 3 的较新 API，也必须按 SDK availability 区分；“Metal 3 model”不是“在 iPadOS 17 全部存在”。

当前 runtime 的 `families/mod.rs:1-78` 合并 2026-05-21 与 2023-10-31 两版表：Apple families 用新版列，Common/Mac 用旧版列，Apple1 用 Apple2 的超集代答。它没有按 iPadOS 17 availability 剪裁所有新版行。因此原始 `family_blockers` 是源码诊断清单，不能把其中较新系统的 API 缺口直接当作 iPadOS 17 硬件缺陷。本文用于否定当前 Apple2/3 完整实现的 PVRTC、resolve、texture-buffer 和未连接的绘图接口都属于已有 Metal 功能。

## 本机对应的硬件和软件来源

本机硬件编码为 `0x06030500`。设备 `drivers/Adreno6xx/NativeAdrenoService.cpp:305` 检查该值，`:801` 将其交给用户态。Mesa `freedreno_devices.py:553-590` 的 `0xffff06030500` 通配项命中本机，采用 `a6xx_base + a6xx_gen4`，包含两个 CCU、32 KiB compute shared memory 和 32x16 tile 对齐。产品名 A642L 与 Mesa 名称 Adreno 7c+ Gen 3 属于同一实际编码的不同命名，不能用名为 A642 的另一颗芯片代替。

本次读取的提交身份如下；Mesa 工作目录还包含本项目已应用的 Darwin 补丁，下面的基础提交不代替补丁内容。

| 来源 | 当前读取的提交 |
| --- | --- |
| Mesa 基础源码 `sources/UniXNU/.cache/mesa-work` | `85fda3b36405bcaea8bffc64aa4f2651dc90770d` |
| metal2vulkan 参考源码 `sources/UniXNU/.cache/gpu-reference/metal2vulkan` | `720ef1ce9ae2a2068999a8675717418b9416283e` |
| AdrenoMetal `sources/UniXNU/drivers/AdrenoMetal` | `0d69cf81e3d1e9e0cc897bf3049e480c94bc4556` |

下文源码简写：`Mesa/` 是上述 mesa-work；`M2V/` 是上述 metal2vulkan；`Frontend/` 是上述 AdrenoMetal。实际部署身份和当前工作树的关系由构建交付记录管理。本研究没有给旧二进制重新指定版本。

## 每一代增加的实际门槛

这张表按官方列中首次出现的关键能力和数字组织，每代同时继承前面的要求。它用于指出具体缺口，完整格式差异另见后文。

| Family | 本代关键门槛 | A642L / 当前实现的判断 |
| --- | --- | --- |
| Apple1 | 最新 PDF 不再给列；runtime 以 Apple2 代答 | 前端强制 true。不能用这个代答证明 A7 的历史完整表已满足 |
| Apple2 | 可编程混合、PVRTC/ETC/ASTC、argument buffer tier 1、memoryless、可编程采样位置；8K 纹理、512 线程、64M texel buffer | 大多数图形基础有对应路径。PVRTC 只能复制，不能采样；独立 depth/stencil 的完整 resolve 和部分纹理操作仍有 runtime 缺口 |
| Apple3 | 细分、间接 draw/dispatch 和 ICB、64 位整数、XR 格式、深度 resolve；16K 纹理、256M texel buffer | 传统图形硬件适合实现。当前 128M texel buffer、Min/Max depth resolve、细分编译边界和前端 ICB 连接阻止完整支持 |
| Apple4 | tile shaders、显式 imageblocks、ROG、post-depth coverage、非整组 dispatch；1024 线程、32 KiB shared、124 片元分量 | 算量和资源上限足够，tile/imageblock/ROG 的完整语义与前端缺失是首个重大结构性门槛 |
| Apple5 | 16 视口、64 倍细分、分层渲染、stencil feedback/resolve、非正方形 tile dispatch、间接细分 | 视口、细分和 stencil export 有硬件路径；继承 Apple4 缺口，另缺完整 stencil resolve 和 tile 形状处理 |
| Apple6 | argument buffer tier 2、ASTC HDR、稀疏颜色纹理、可变光栅速率、顶点放大、函数指针、ray tracing、纹理原子 | BDA/descriptor indexing、ASTC HDR、32 位纹理原子可用。Darwin sparse 关闭；无硬件 ray intersection；完整 sparse/RT/放大组合未实现 |
| Apple7 | mesh、SIMD reduction/matrix、重心坐标、MSAA 分层渲染；顶点放大从 2 增至 8 | A6xx gen4 图形 subgroup 和 MSAA array 可用；没有 mesh/task、fragment barycentric 扩展，matrix 仅部分计算模拟；硬件 multiview 数量为 6 |
| Apple8 | 稀疏 depth/stencil、lossy compression、SIMD shift/fill、texture LOD query | shift/fill 可用 shuffle 合成，LOD query 已有；sparse 和有损压缩仍缺。64 位 atomic min/max 的 Apple8 例外只适用于 macOS，不能套到本机 |
| Apple9 | 所有平台完整 64 位原子、BC 普遍支持、间接 mesh 和更多 RT 能力；FP32 格式过滤/resolve 增强 | BC 和 FP32 过滤单项已有；A6xx 没有该路径要求的 64 位 buffer/image 原子，mesh/RT 和 tile 继承缺口仍在 |
| Apple10 | 32K 纹理、8x MSAA、更多间接状态和纹理原子/读取操作 | Turnip 当前是 16K 与最高 4x；高级 API 也未连接。不能靠 family 返回值越过这些数字 |

Apple7/8 在 iPadOS 的 BC 支持是逐设备查询项，Apple9 才普遍要求。Apple8 的部分 64 位原子是 macOS 例外。两处均按 PDF 第 6 页脚注解释，不能仅凭格式表某个单元格把它们扩大到所有平台。

## 硬件、Turnip 和 Metal 前端逐项矩阵

| 功能 | A642L 硬件对应能力 | 当前 Turnip 实际暴露条件 | metal2vulkan runtime / Objective-C 前端 |
| --- | --- | --- | --- |
| Framebuffer fetch / programmable blending | GMEM 与光栅重叠顺序控制；可读取同像素前序颜色 | `EXT_rasterization_order_attachment_access=true`，颜色/depth/stencil access 均 true | runtime 用有序 input attachment 实现，属于可直接映射的有效路径 |
| Raster order groups | 寄存器能保证 attachment 访问顺序；任意 buffer/image 的 ROG 还需要完整同步语义 | 没有声明 `EXT_fragment_shader_interlock`，pixel interlock 特性保持 false | runtime 完整 ROG 判定要求 interlock；只有颜色输出等受限情况可运行，不能覆盖 Apple4 全部 ROG |
| Tile shader / imageblock | 有 tile-based rendering、512 KiB GMEM 和 compute shared memory；没有现成 Metal tile 阶段接口 | 没有将 Apple imageblock 的布局、持久 tile memory、样本覆盖直接暴露成 Vulkan 功能 | runtime 有单采样单层的部分 tile 模拟；多采样、层、若干线程组形状及 imageblock 组合仍拒绝。Objective-C `dispatchThreadsPerTile:` 直接使命令失败 |
| Tier 1/2 argument buffers | GPU 地址、bindless 纹理和动态描述符访问有对应执行能力 | BDA、runtimeDescriptorArray、partiallyBound、updateAfterBind、nonuniform indexing 均开启 | runtime 满足条件时报告 tier 2，可用地址和资源 ID；当前桥接没有完整 `MTLArgumentEncoder` 对象接口，不能把 runtime 属性当全部 ObjC API 已接通 |
| 普通间接 draw/dispatch | CP 能执行间接命令，支持 base vertex/instance | multiDrawIndirect、drawIndirectFirstInstance、drawIndirectCount=true | runtime 有实现；索引偏移到 buffer 尾部等仍被标作 `IndirectDrawArguments` 缺口，桥接主要连接直接 draw/dispatch |
| ICB | 可用 GPU 写入命令数据、间接执行实现，不要求 Apple 特有命令二进制 | BDA 和上述间接功能提供基础；不能用 Vulkan indirect draw 等同 Metal ICB 全部状态 | runtime compute ICB 较完整，render ICB 对 visibility/range/特定 draw 组合保留缺口；前端未提供完整 newIndirectCommandBuffer/executeCommands 接口 |
| 细分 | 原生 HS/DS 管线 | tessellationShader=true，最大 factor=64，patch size=32 | runtime 有直接/间接 patch draw；缺省控制点数、片元 primitive ID 等编译组合仍拒绝；ObjC 未接完整 patch draw API |
| SIMD/quad | gen4 `has_getfiberid`，跨 lane 指令 | BASIC/VOTE/BALLOT/SHUFFLE/RELATIVE/ROTATE/CLUSTERED/ARITHMETIC；gen4 加全部 graphics stages 与 QUAD | runtime 逻辑线程宽度为 32，通过其 subgroup 翻译实现。单项可覆盖 Apple4/6/7/8 的 shuffle/reduction；matrix 完整阶段覆盖仍缺 |
| FP16 / 64 位整数 / FP64 | 原生 16 位 ALU；64 位整数需较窄指令组成；无 native double | shaderFloat16=true、shaderInt16=true、shaderInt64=true；shaderFloat64=false | runtime 可拆分 Int64；Vulkan true 不代表单条原生 64 位算术。Metal family 表没有 FP64 必选项，FP64 缺失不用于否定 family |
| 32 位和浮点原子 | 32 位整数原子提供 CAS 基础 | buffer/shared/image Float32 atomics=true，Float32 atomic add=false；R32 integer image atomic 有格式位 | runtime 将浮点加等操作降为 CAS 循环，已有实现可覆盖相应功能；需保留正确浮点和内存顺序语义 |
| 64 位原子 | A6xx gen4 未设置 64 位 SSBO/image atomic 能力 | `has_64b_ssbo_atomics=false`、`has_64b_image_atomics=false`、shared=false | runtime 完整 Apple9 原子要求 buffer 64 位原子及 R64_UINT image view，两项都不满足；普通两个 32 位读写不能替代原子操作 |
| Sparse | GPU MMU 是实现稀疏映射的基础，尚无本机页重绑/驻留语义的完整路径 | Darwin `has_sparse=false`、`has_sparse_prr=false` | runtime 有普通整张图保存未映射零值的替代路径，仍缺部分 view/cube/MSAA tile map 及访问计数；不能宣称 Apple6 sparse 完整 |
| Mesh / object shader | 传统 graphics + compute 可用于展开几何；没有已暴露 mesh/task 管线 | 扩展表没有 EXT_mesh_shader，meshShader/taskShader 未开启 | runtime 部分模拟不能覆盖所有 object threadgroup/间接执行组合，完整 family 判定仍要求 mesh/task。前端未接 mesh pipeline/draw |
| Ray tracing | A6xx gen4 没有 `has_ray_intersection` | RT 扩展由该硬件位控制，因此不暴露 acceleration_structure/ray_query；源码里孤立的 `rayQuery=true` 不能越过扩展条件 | runtime 当前要求 Vulkan ray query + acceleration structure，再有自身 acceleration-structure 缺口；软件 BVH 遍历需新增，ObjC RT API 未接 |
| 可变光栅速率 / 顶点放大 | gen4 有 multiview、per-view viewport 和部分 shading-rate 能力 | multiview 最大 6；fragment density map=true，attachment shading rate 依硬件位 | runtime 可分区域重放及逐放大绘制；不同层 rate map/scissor、放大与细分组合仍有缺口；8 views 可用重放实现而非强改硬件上限 |
| 动态库 / 函数指针 / binary archive | 软件链接、switch 分派、GPU buffer 足以承载大部分语义 | 不需要同名硬件单元 | runtime 已有动态库、可见函数表、switch/调用栈路径和 archive；前端没有与 runtime 全量对象一一连接 |

关键源码是 `Mesa/src/freedreno/vulkan/tu_device.cc:193-418,425-550,814-844,925-965`，`M2V/crates/metal2vulkan-runtime/src/families/features.rs:1000-1445`，以及 `Frontend/NativeBridgeMetalFrontend.mm:700-707,1259,1351`。

Framebuffer fetch 的硬件证据还包括 `Mesa/src/freedreno/vulkan/tu_pipeline.cc:4400-4439`：有序 attachment access 时，将 `GRAS_SC_CNTL.SINGLE_PRIM_MODE` 设为 `FLUSH_PER_OVERLAP`。`registers/adreno/a6xx.xml:1705-1729` 描述它在可能重叠的 primitive 之间等待并使 UCHE 无效；sysmem 的另一模式还协调 UCHE/CCU 的数据与标记。这是真实寄存器路径。它说明 programmable blending 能实现；它尚未提供 Metal 允许的任意 ROG 内存访问和八个逻辑组的完整规则。

## 格式、过滤、render target 和 MSAA

格式必须按操作判断。采样成功只证明 sampling；Color、Blend、Write、MSAA、Resolve、Atomic、Sparse 各有独立要求。

| 格式组或操作 | A642L / Turnip | 当前 Metal 实现和 family 含义 |
| --- | --- | --- |
| R/RG/RGBA8、BGRA8、16 位归一化和 FP16 | A6xx 格式表有 texture/color 映射，多数格式支持 shader write；整数格式不线性过滤、不混合 | 覆盖常规系统渲染基础。不能从“支持某个 PixelFormat”推出所有操作可用 |
| ETC2/EAC、ASTC LDR | 原生 texture 格式，Vulkan 相应 compression feature=true | 可支持 Apple2 起的这些压缩采样格式 |
| ASTC HDR | gen4 继承 `has_astc_hdr=true`，格式表有 FLOAT ASTC | 属于 Apple6 格式单项优势；不使 sparse、RT 等 Apple6 要求自动满足 |
| BC1-BC7 / BC6H | 本机不是裁剪 BPTC 的 A702，Turnip BC feature=true | 覆盖 Apple9 的 BC 采样单项，iPadOS Apple7/8 的可选性无需借用 |
| PVRTC | 无原生 fd6 PVRTC 格式和 Vulkan IMG PVRTC 扩展 | `pvrtc.rs` 只用 BC1/ETC2/EAC/BC4 carrier 保存原始块并复制，明确没有解码器。采样和 shader 绑定拒绝，是 Apple2 的直接阻断项 |
| RGB10A2、RG11B10、RGB9E5 | fd6 有 color/texture 映射；后两者有浮点格式，RGB9E5 无普通 vertex 格式 | 可按操作接入，packed vertex float 可由 shader 取数转换。需分别核对 storage/resolve，不靠格式名统一宣称 |
| BGRA10_XR/BGR10_XR 及 sRGB | 没有 Apple XR 的一一对应原生表示 | runtime 以原始 texel、shader 转换和有序 attachment fetch 实现过滤/混合，某些 blend/resolve 组合仍拒绝；这属于 Apple3 的明确补齐工作 |
| R32/RG32/RGBA32 float | 本机 fd6 texture/color 支持，`tu_format_linear_filtering_supported` 对 A642L 的非整数格式允许线性过滤；可混合 | 单项超过较早 Apple family 的 FP32 过滤范围。runtime 仍要按自身 view/resolve 规划决定完整 API |
| Depth16、Depth32、Depth32+Stencil8 | 有格式和 shadow comparison。Turnip depth/stencil resolve 只列 SAMPLE_ZERO 与 CUSTOM_EXT | Metal depth Min/Max 不能从该掩码直接取得；runtime 对 Min/Max 仍标 `DepthStencilResolve`。Stencil 的 DepthResolvedSample 另需选择对应深度样本 |
| 4x MSAA | color/depth/stencil/sample image 都列 1/2/4；storage image 仅 1 | 满足 Apple2-9 的最大 render-pass sample count；tile/imageblock 的 MSAA 语义仍有独立缺口 |
| 8x MSAA | 当前 Turnip 的 `sample_counts` 没有 8 | Apple10 的明确数字缺口。不能从寄存器有 sample 枚举推成当前可创建 8x Metal texture |
| 64 位 texture atomic | R64_UINT/SINT 的特殊 storage 格式路径只在 `has_64b_image_atomics` 时出现，本机为 false | `RG32Uint` 作为 Metal ulong 原子需要这个路径；常规 RG32 读写不提供 64 位原子 |

依据：`Mesa/src/freedreno/fdl/fd6_format_table.c:133-143,221-230,265,310-374`、`vulkan/tu_formats.cc:105-270`、`tu_device.cc:969-972,1061-1068,1260-1272`；`M2V/.../src/pvrtc.rs:1-56`、`families/formats.rs:1-65`、`features.rs:1015-1027`。当前私有双平面 XR/YCbCr 的系统选择另见 [Metal 提交研究](GPU_METAL_SUBMISSION_RESEARCH_2026-10-28.md)，不把私有格式单独等同某个公开 family。

## 数字上限和 texture buffer 的具体性质

| 数字 | A642L 对应当前源码 | 对 family 的影响 |
| --- | --- | --- |
| 1D/2D/cube 边长 | 16,384 | 达到 Apple3-9；Apple10 的 32,768 未达到 |
| 3D 边长 / array layers | 2,048 / 2,048 | 达到表中这些维度；framebuffer layers 另为 1,024，不能把 texture array 长度当可一次渲染层数 |
| 颜色附件 / 片元输入分量 | 8 / 124 | 覆盖 Apple4 起这些数字 |
| Compute shared memory | 32 KiB | 覆盖 Apple4 起的 threadgroup memory；不能用它代替 128/256 KiB 的隐式 imageblock |
| Threadgroup 线程 | A6xx base threadsize=64、max_waves=16；double-threadsize 设置可增加 Vulkan 值；runtime 最多暴露 1,024 | 满足 Apple4 起的理论线程数；具体 pipeline 仍受寄存器和 shared memory 约束 |
| 细分 / 视口 | factor=64 / 16 | 达到 Apple5 的两个数字；前端和编译边界仍需补齐 |
| Descriptor 数量 | `MAX_SET_SIZE=(1<<28)*4`，每个最大 texture descriptor 为 64 bytes，单类计数上限约 16M；部分组合结构取更小数 | 足以承载 Apple6 的 1M texture descriptors。实际 sampler 对象分配还受 `maxSamplerAllocationCount=65536` 及内存限制，不能只看 descriptor 数量 |
| Metal buffer 最大长度 | BDA 模式取 Vulkan maxBufferSize 和 maxMemoryAllocationSize 较小者；当前为 `0xffffffff` bytes | 这是单次分配/API 上限，实际可分配还受本机内存与进程预算限制；不是 128M texel 的单位 |
| Texture buffer | 当前 `maxTexelBufferElements=1<<27` | Apple2 要 64M，能覆盖；Apple3 起要 256M，即 `1<<28`，当前不覆盖 |
| Storage-buffer descriptor range | `maxStorageBufferRange=1<<27` bytes | 单位是 bytes；BDA 可以绕过这类描述符 range，不等于普通 Metal buffer 最大长度 |
| Multiview | 6 views | Apple6 的 2 可由硬件覆盖，Apple7 的 8 要重放或扩大实现；已有 runtime 还有其他放大组合缺口 |

Texture-buffer 上限最容易被误读，源码链为：

1. `freedreno_devices.py:151` 给 A6xx 设置 `max_texel_buffer_range_elements = 1 << 27`。
2. `tu_device.cc:1158-1160` 将它交给 Vulkan `maxTexelBufferElements`。
3. `fdl/fd6_view.cc:575-608` 用 `elements = size / elem_size` 从字节数得到 texel 数，再写 descriptor；16 位 R texel 与 32 位 R texel 对应的字节数不同，元素上限相同。
4. `registers/adreno/a6xx_descriptors.xml:74-78` 的 buffer `NUM_ELEMENTS` 字段覆盖 bit 0-29，共 30 位。因此**源码没有证明本机只具有 27 位元素寻址**。128M 是当前驱动承诺的上限，不是由这一个寄存器字段直接推得的物理极限。
5. 当前 Mesa 的扩容开关只对 A7xx+ 启用，`tu_device.cc:1740-1751` 明确说明现成 texel-buffer 模拟依赖 A7xx 才出现的 `resbase`。
6. runtime `families/limits.rs:875-879` 直接比较 `maxTexelBufferElements`，所以不论 Metal 普通 buffer 的最大字节数多大，本机现在都过不了 Apple3 的该行。

可实施的替代是由 Metal 编译/绑定层持有原始 BDA 与格式，按地址加载并解包 texel，或把一个逻辑 buffer texture 分成多个有效 Vulkan view 并按索引选择；读取、写入、atomics、view、长度查询和边界处理须一起改。现有描述符字段足够宽，并不能据此直接把驱动常数翻倍。本研究未执行硬件边界实验，也没有把该常数翻倍当成已证明的修复。

## 当前为什么连 Apple2/3 都不能称完整

`Frontend/bridge/NativeMetalObjects.rs:387-416` 固定 `TARGET_FAMILIES = Apple1/Apple2/Apple3/Common1/Common2`，`:475` 将 `runtime.supports_family` 与该数组匹配结果做 OR。`Frontend/NativeBridgeMetalFrontend.mm:1586-1588` 把放行的缺口写入 `pipelines.log`，每行以 `F Apple...` 开始。本轮在已有 `logs/` 文本、raw/tail 中未找到可引用的完整 family waiver 输出，因而下面采用源码中可确定的具体拒绝项，不编造实机缺口数量。

| Family | 足以阻止完整声明的当前条目 | 直接代码证据 |
| --- | --- | --- |
| Apple2 | PVRTC 过滤；独立 depth/stencil 的完整 resolve；linear texture 和 stencil-view 操作边界；MTLIOCommandQueue 尚缺 | `pvrtc.rs:1-56`；`features.rs:918-935,1010-1027,1420-1429` |
| Apple3 | 继承上述；depth Min/Max resolve；细分的部分合法输入；完整 indirect draw/ICB render；256M texture buffer；XR 的部分操作组合 | `features.rs:1015-1022,1180-1190,1250-1286`；`limits.rs:875-879`；`families/formats.rs:24-40` |
| Apple4 | 继承上述；pixel interlock；imageblock 布局/MSAA/测试后丢弃语义；tile 多采样/多层/线程组形状；post-depth coverage | `features.rs:1189-1193,1287-1320`；ObjC `NativeBridgeMetalFrontend.mm:700-707` |
| Apple5 | 继承上述；stencil resolve 的深度对应样本；非正方形 tile 和间接细分组合 | `features.rs:1022-1027,1280-1320` |
| Apple6 | 继承上述；sparse 页映射和访问计数；ray tracing；完整 vertex amplification、rate map；coherent shader texture；部分后续系统 API | `features.rs:1137-1165,1221-1234,1344-1354,1397-1434` |
| Apple7 | 继承上述；完整 mesh/task；图形阶段 matrix；barycentric；相关新读纹理操作 | `features.rs:1120-1125,993-999,1250,1315-1343` |
| Apple8 | 继承上述；sparse depth/stencil；lossy compression | `features.rs:1405-1425` |
| Apple9 | 继承上述；64 位 buffer/image 原子；间接 mesh；扩展 RT 功能 | `features.rs:1210-1220,1326-1354` |
| Apple10 | 继承上述；32K、8x；sampler/indirect state/额外纹理操作与 MTL4 对象体系 | `tu_device.cc:969,1153-1157`；`features.rs:968-999,1435-1450` |

此外，runtime 有某个 Rust 对象并不表示本机 ObjC 前端已经提供相应 Metal 对象。当前桥接未提供完整的 argument encoder、ICB、tessellation draw、mesh 和 acceleration structure 接口。系统当前使用的直接绘图/计算路径可以正常渲染，和覆盖完整 family 是两个分别需要列出调用入口的事实。

## 从现状到完整 Apple3 的具体补齐范围

Apple3 作为第一目标的理由是：它所需的传统图形阶段、算术、线程组规模、普通纹理维度和 attachment 操作都有 A6xx 硬件或已存在的 runtime 工作作为基础；第一套必须大规模重做渲染过程的 tile/imageblock 功能从 Apple4 才开始。以下是使 Apple3 成为实际完整目标的必要工作集合，尚未实施：

1. **压缩和特殊格式**：加入 PVRTC 的 RGB/RGBA、2/4 bpp、sRGB、mip 和区域更新解码，保证 shader sampling/read；补齐 XR blend/resolve、linear texture 和 stencil view 的合法操作组合。carrier 复制继续保存原始块，解码后的采样内容随写入更新。
2. **256M texture buffer**：在编译器与绑定层增加 BDA 格式访问或分段 view；同时覆盖读写、长度、view 和越界语义。普通 buffer 的 BDA 路径已经存在，不能只修改 family limits。
3. **Depth/stencil**：把 Min/Max depth resolve、独立附件组合和 stencil 的指定样本选择做成完整 GPU 路径；确保多层、可采样限制、store/resolve 次序都覆盖。
4. **细分和间接执行**：补齐缺省 control-point count、primitive ID 等 compiler 分支；实现合法 indirect index 偏移；补齐 render ICB 的 visibility、GPU 写 execution range 和 draw 状态组合，并连接 Objective-C 对象及 selector。
5. **完整前端连接**：连接 argument encoder、采样位置、相关 blit、IO command queue、查询/计数对象等 iPadOS 17 可调用 API，使 Rust runtime 的功能能够从系统 Metal 调到。按 family rows 的具体输入组合处理纹理 swizzle 等剩余项。
6. **按系统版本整理判定**：把 iPadOS 17 API 可用集合与 2026 新版 API 分开，保留每个缺口的实际处理结论；为本机提供内容完整的 family 明细。最终声明依据已完成实现，现有强制 family 返回值不参与证明。

这些路径能够利用现有硬件完成所需操作，性能代价集中在格式解码、特殊 resolve 和间接执行编排。这里的“可达”是有具体实现步骤的工程推断，当前文档没有把它记成已完成或实机已覆盖。

## Apple4 以上的软件实现需要增加什么

| 目标 | 需要新增的实际工作 | 当前可以确认的边界 |
| --- | --- | --- |
| Apple4/5 | 在 fragment 与 compute/tile 执行之间保存 attachment 和每样本 imageblock；保持深度、stencil、coverage、ROG 和重叠 primitive 的执行顺序；支持多层、MSAA、规定线程组和多个 imageblock 布局 | 部分单层单采样模拟已经存在；完整过程不是增加一个 Vulkan feature flag。若没有完整 interlock，必须改变绘图执行顺序或实现其他等价的顺序保证 |
| Apple6 | 稀疏映射/驻留查询/访问计数；或者为每个逻辑 tile 保存映射状态并在 shader 中正确处理未映射访问；软件 BVH 构建和 ray traversal；函数调用与 RT 查询状态；完整 rate map/amplification 组合 | Darwin 的 sparse 与 A6xx RT 扩展当前均不提供，runtime 的普通整图替代和函数表只解决其中部分 |
| Apple7 | 用 compute 展开 object/mesh 工作并输出传统绘图数据，处理 payload、dispatch、屏障和间接上限；对所有所需阶段实现 SIMD matrix 与 barycentric | 普通 compute 能承载计算，但当前 runtime 仍有明确拒绝项，前端尚未连接这些 API |
| Apple8/9 | 完整 sparse depth/stencil、有损纹理存储策略；对每个共享位置实现正确的 64 位原子串行化；扩大 RT 和 mesh 间接命令实现 | 两次 32 位写不等于 64 位原子；基于锁的 shader 实现还需解决执行进度、跨队列和所有内存别名，当前不存在可宣布完整的实现 |
| Apple10 | 对 32K 纹理增加分块寻址/过滤，对 8x MSAA 增加样本存储和光栅化实现，并连接新 API | 当前 16K/4x 原生路径不覆盖这些要求；这已经超出日常 API 适配的工作规模 |

因此，当前有证据的排序是：**先完成 Apple3；Apple4/5 作为需要完整渲染过程改造的下一阶段；Apple6 及以上需要独立的大功能实现。** 本机已经具备的较高代单项可以逐项开放，让调用者按具体能力使用，无须把整个 family 提前宣称为完整。

## 阅读入口

- [Apple 官方 PDF](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)：本次版本 2026-05-21。
- [Apple Metal3 family 定义](https://developer.apple.com/documentation/metal/mtlgpufamily/metal3)：区分系统 API 和硬件 family。
- [A642L 硬件编码及接入调查](../../../notes/ADRENO642L_IMPLEMENTATION_RESEARCH.md)：芯片身份、原厂 DTS 和 Mesa 匹配依据。
- [系统 Metal 格式与提交研究](GPU_METAL_SUBMISSION_RESEARCH_2026-10-28.md)：当前系统的格式选择与 CPU/GPU 等待。
- `Mesa/src/freedreno/common/freedreno_devices.py`、`vulkan/tu_device.cc`、`vulkan/tu_formats.cc`、`fdl/fd6_view.cc`、`registers/adreno/a6xx_descriptors.xml`：本机实际能力与数字来源。
- `M2V/crates/metal2vulkan-runtime/src/families/`：每个 feature、limit、format 操作的源码判断；`pvrtc.rs` 和 `capabilities.rs` 给出关键例外。
- `Frontend/bridge/NativeMetalObjects.rs:387`、`Frontend/NativeBridgeMetalFrontend.mm:700`：当前 family 强制值与 tile API 拒绝行为。
