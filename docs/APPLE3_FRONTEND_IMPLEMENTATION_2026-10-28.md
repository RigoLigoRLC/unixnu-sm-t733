# iPadOS 17 Metal 前端接口接入

本轮把 iPadOS 17 可调用的参数编码、间接命令和细分曲面接口接到固定版本 metal2vulkan 的真实对象及编码方法。新增代码在 `AdrenoMetal/NativeBridgeMetalExtended.inc`、`bridge/NativeMetalExtended.h`、`bridge/NativeMetalExtended.rs`，基础前端与 Rust bridge 以 include 接入。公共 `components.toml` 的 `source_files` 包含三个新文件和基础 `NativeMetalObjects` 两个文件，组件源码输入变更能够触发重建。本文记录实现，未执行构建、测试或设备 probe。

## 已接入的调用

| Metal 对象和入口 | 进入 runtime 的操作 | 前端负责的对象和参数 |
| --- | --- | --- |
| `MTLDevice newArgumentEncoderWithArguments:` | `new_argument_encoder_with_arguments` | dataType、index、arrayLength、access、textureType、constantBlockAlignment 全部传递 |
| `MTLFunction newArgumentEncoderWithBufferIndex:` | 函数反射创建 argument encoder | 使用函数的真实参数布局 |
| `MTLArgumentEncoder` | 目标 buffer、数组元素偏移、嵌套 encoder、constantData、资源数组写入 | buffer/texture/sampler/compute pipeline/render pipeline/ICB；批量写入统一交给 runtime 做范围验证 |
| `MTLFunction patchType/patchControlPointCount` | runtime 函数反射 | 普通 vertex 与 post-tessellation vertex 采用不同 pipeline 描述 |
| render pipeline 描述 | `RenderPipelineDescriptor.tessellation` 和 `support_indirect_command_buffers` | 最大因子、partition、factor format/step、winding、control-point index type、factor scale |
| 普通 render/compute encoder | `draw_primitives_indirect`、`draw_indexed_primitives_indirect`、`dispatch_threadgroups_indirect` | 保留参数 buffer 和 index buffer，GPU 读取其内容 |
| 普通 render encoder 的 patch draw | direct/indirect 的 `draw_patches`、`draw_indexed_patches` | patch/control-point 索引 buffer、范围、实例、base instance、factor buffer/offset/stride/scale |
| ICB 创建 | `new_indirect_command_buffer` | commandTypes、inherit flags、各阶段 buffer 上限、kernel threadgroup memory 数量、dynamic stride |
| ICB 的 CPU compute command | pipeline、kernel buffer/stride、dispatch threads/threadgroups、threadgroup memory、stage-in region、barrier/reset | 命令对象持有 ICB，ICB 按 command index 和具体绑定保留资源 |
| ICB 的 CPU render command | pipeline、vertex/fragment buffer、vertex stride、primitive/indexed/patch/indexed-patch draw、barrier/reset | patch draw 包括独立 factor buffer 参数 |
| render/compute ICB execution | `execute_commands_in_buffer` 和 GPU range buffer 版本 | encoder 保留 ICB/range buffer，runtime 记录执行 |
| blit ICB | copy、reset、optimize | copy 继承源命令的资源引用；GPU reset 保留 CPU 引用至被替换或释放 |
| render pass | resolve textures/level/slice/depthPlane、depth/stencil filter、store options、sample positions | 所有附件的 resolve 目标随 command buffer 保留，外部 IOSurface 继续走现有缓存交接 |
| render encoder store options | color/depth/stencil store-action-options | 交给 runtime 更新当前 pass |
| device default sample positions | 检查 sample count 和 Vulkan `standardSampleLocations` 后返回标准位置 | 主代理在基础 frontend 与 Extended bridge 接入 |

参数编码器写入的是用户提供的 argument buffer。间接引用资源仍经 `useResource(s)` 交给 runtime 建立资源访问依赖；本轮的同步实现已经把 ICB 加入同一资源引用列表。CPU 参数写入和 ICB 记录写入所需的内存可见性由 runtime Buffer 与提交同步实现负责。

## 后端实现

前端传入的参数由以下实现消费。系统范围固定为 iPadOS 17，功能可调用性与该版本的 GPU family 表一起判断。

| 范围 | 当前具体位置 | 处理过程 |
| --- | --- | --- |
| 256M texture buffer | Turnip `tu_physical_device.cc`、A6xx descriptor 和 shader lowering | 使用 3D texture buffer 表示长缓冲区，shader 从描述符读取完整长度与偏移，贯通 fetch、读写、原子和长度查询 |
| depth/stencil resolve | runtime `resolve`、`depth_stencil_resolve`、`depth_copy`、`image_plan` | Sample0/Min/Max 选择样本，必要时通过整数颜色附件和 GPU transfer 写回深度，depth/stencil 可使用独立 resolve 目标 |
| 原生 tessellation 的 AIR 输入 | compiler `lower/tessellation/mod.rs`、`pipeline/link/tessellation` | 缺省控制点数从 draw push 数据进入 control/evaluation，PerInstance 属性按 step rate 取数，primitive ID 进入原生 patch ID 路径 |
| ICB 执行组合 | runtime `render/executions.rs`、`render/deferred_shadows.rs` | 同 pass 写入间接参数时切分提交并在完成后继续；commit 使用完整 residency 生成 shadow copy 计划；memoryless backing 在 resolve 规划前固定 |
| XR 采样 | compiler `lower/sampler/extended.rs` 及 cube、heap metadata helper | 软件解码后处理过滤、mip、数组层、跨面和各向异性采样；argument buffer 的 sampler 元数据与实际纹理视图贯通绑定和采样发射 |

PVRTC 已另外实现 GPU 解码和缓存采样，见 [PVRTC GPU 解码实施](PVRTC_GPU_DECODE_IMPLEMENTATION_2026-10-28.md)。其补丁为 0014，`texture.rs` 的共享修改折回 0011。

间接 tessellation 参数和 ICB 内的 tessellation 属于 Apple5，multiview amplification 属于 Apple6；这些路径中的额外 CPU 参数依赖按原有条件处理。TES SIMD 仍需要该阶段真实的 subgroup 能力。普通间接索引偏移恰好位于 buffer 尾部时，绑定有效的零索引存储，由已有 robust index 或 GPU bounds 路径处理读取。

已读取 `origin/master=f6a96b7d` 对应实现，构建仍使用固定基线和独立补丁。A642L 采用原生 Vulkan tessellation；本轮的控制点数、实例步进、ICB 分段和资源寿命实现记录在 AdrenoMetal 的 `TESSELLATION_AND_ICB_2026-10-28.md`。

现有 `TARGET_FAMILIES` 的兼容性策略按主代理指示保持；本次扩展没有改变 family bool。`supportsFamily` 当前结果继续不能充当全表能力已通过的证据。最终 family 审计应按 iPadOS 17/Metal 3 的接口范围核对，2026 表新增的后续系统 API 单独列出。
