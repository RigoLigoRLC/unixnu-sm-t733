# Metal 分配参数与 A642L 内存一致性

日期：2026-10-28。范围：当前 `NativeBridgeMetalFrontend`、metal2vulkan runtime、Vulkan memory type、Turnip Darwin ABI。本文记录源码事实与目标设计，本轮未修改实现、构建或运行测试。

总设计见[GPU 内存一致性设计](../GPU_MEMORY_COHERENCY_DESIGN_2026-10-28.md)，外部页与显示映射见[导入内存及显示一致性](GPU_IMPORTED_MEMORY_DISPLAY_2026-10-28.md)。

当前已支持的 Metal 参数组合中，没有找到一种本身要求 CPU 对 BO 逐行执行 clean/invalidate。普通新建资源、Shared 资源、IOSurface 导入与扫描输出的目标都是每次提交和完成时逐行维护为零。A642L 可用的 WB 硬件一致性必须接通；真实 Normal NC 内存以屏障完成交接。设计不以某个资源比例或使用频率为前提。

`VK_MEMORY_PROPERTY_HOST_COHERENT_BIT` 表示 Vulkan 调用方无需显式 flush/invalidate。它可以由 WB 硬件一致性或真实 NC 映射实现。59b 的 type 0 还用于导入：KMD 根据导入页选择硬件一致性或软件维护。因此，单看 Vulkan 的 `HOST_COHERENT` 位不能判断硬件是否参与。

## 参数评审表

以下数字为 `MTLResourceOptions` 的 storage/cache 部分，hazard 字段可叠加，缓存策略不由 hazard 字段决定。`0x00` 是 Shared + DefaultCache，`0x01` 是 Shared + WriteCombined，`0x10/0x11` 是 Managed，`0x20/0x21` 是 Private，`0x30/0x31` 是 Memoryless。源码索引 F、R、V、K 见文末。

表中的 type 0 是当前 `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`；目标 type 1 是 `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED`。type 1 只有在完整的 owned WB 分配和映射能力可用时才公布。索引沿用当前 Turnip 表顺序。

| Metal 入口及具体参数组合 | 来源与所有权 | Vulkan 实际 / 目标字段 | KMD 实际 / 目标策略 | CPU 逐行维护与时机 | 阻碍 WB 硬件一致性的具体原因 | 源码依据 |
| --- | --- | --- | --- | --- | --- | --- |
| `newBufferWithLength:options:` 或 `newBufferWithBytes:length:options:`，`options=0x00` | KMD 新建 backing；`Bytes` 复制输入内容 | 当前 type 0；目标 type 1 | 当前 owned WC；目标 owned WB + coherent | 目标提交与完成均为零 | 当前能力未公布，`BoAlloc` 未传缓存要求 | F1，R1，R2，V1，V2，K1 |
| 同两入口，`options=0x01` | 同上；WriteCombined 在本 runtime 是 hint | 当前 type 0；现有 unified 排序在 type 1 可用时优先 type 1 | 目标按实际选择分配 WB + coherent | 零 | 不得把 hint 强制解释成必须 NC；当前仍受 owned ABI 阻碍 | R1，R2，R3，V2 |
| 同两入口，`options=0x20/0x21` | KMD 新建；Metal 调用方不映射 `contents` | 当前 Private 候选选 type 0；目标 type 1 | 目标 owned WB + coherent，保持 Private 的访问规则 | 零 | 当前 Private 排序未优先 cached coherent，同分选择 type 0 | R1，R2，R4 |
| 同两入口，`options=0x10/0x11` | 本 frontend/runtime 接受 Managed；A642L 为 unified memory | 当前 type 0；目标 type 1 | 目标 owned WB + coherent | 零；`didModifyRange` 和同步命令保留范围/顺序语义 | 当前 owned WB 未接通；Managed 参数不要求非一致性内存 | R1，R2，R5，V1 |
| `newTextureWithDescriptor:`，`storageMode=Shared/Managed/Private`，`cpuCacheMode=DefaultCache/WriteCombined` | KMD 新建 image backing；CPU 访问可通过 host copy 或 staging | 当前 image 默认按 Private 选 type 0；目标 type 1，CPU staging 也取 type 1 | 目标 image 与 staging 都是 owned WB + coherent | 零；格式转换、复制与 GPU cache event 仍按原语义执行 | image 选型只接收 storage，未把缓存要求交到最终分配；owned ABI 仍丢要求 | F2，R6，R7，V2 |
| `newTextureWithDescriptor:`，`storageMode=Memoryless`，`cpuCacheMode=DefaultCache/WriteCombined` | runtime 建 transient attachment | 当前没有 lazy BO，落普通 type 0；目标普通 type 1 | 目标普通 backing WB + coherent；维持 Memoryless load/store 语义 | 零 | `has_lazy_bos=false` 不构成软件缓存维护理由；普通候选尚未优先 WB | R6，R8，V3 |
| `newBufferWithBytesNoCopy:length:options:deallocator:`，`options=0x00/0x01/0x10/0x11`，原有页为 WB | 调用方保有原映射；import pin 并延迟 deallocator 至 GPU 引用释放 | 当前 `HOST_ALLOCATION_EXT` + type 0；目标导入时保留所选 type 与实际属性，WB 可选 type 1 | 59b 已可对合格 WB import 建 coherent 映射；目标完整属性合同 | 零 | WB 本身无硬件阻碍；现有 pointer query 固定 bit 0，import ABI 未保存选择要求 | F3，R9，V4，K2 |
| 同 no-copy 参数，原有页实际为 Normal NC，包括本机 managed DRAM 的 `VM_WIMG_RT` 对应 Normal NC | 外部所有者创建，import 保留全部 CPU aliases 的属性 | 当前仍 type 0；目标按真实 NC 保持 type 0，记录实际属性 | 当前可能归 `NonWriteBack` 并维护；目标 NC GPU 映射 + 屏障 | 零 | 实页采用 NC，所以不记为 WB coherent；无需逐行 cache 指令 | F3，R9，K2，K3 |
| `newTextureWithDescriptor:iosurface:plane:`，有效 2D plane，descriptor 为 Shared/Managed/Private 及两种 cache mode | IOSurface 持有 backing；frontend 固定以 `options=Shared` 导入整个 allocation，再建立 plane texture | 当前 `HOST_ALLOCATION_EXT` + type 0；目标 WB 用 type 1，NC 用 type 0 | WB：GPU 与 DPU 各建 coherent 映射；NC：两者匹配 NC | 零 | descriptor 的 cache/storage 不重建 IOSurface 页；实际属性来自 surface 创建者与内核映射 | F4，R10，K2，K3，K4 |
| `newHeapWithDescriptor:`，`type=Automatic/Placement`，`storageMode=Shared/Private`，两种 cache mode；子资源 storage/cache 与 heap 相同 | 一个 heap backing 被多个 buffer/texture 共享；runtime 池还按 memory type 分开 | 当前 heap backing type 0；目标 type 1；子分配继承 backing 属性 | 一次分配确定属性，所有子范围继承 WB + coherent | 零 | 不能在子分配时换 backing 的 CPU/GPU cache 属性；当前 owned ABI 未接通 | F5，R11，R12，V2 |
| `buffer newTextureWithDescriptor:offset:bytesPerRow:`、`newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:`、texture view | 复用 buffer/texture 原 backing；需要 shadow 时另建 owned image | 当前沿用父 backing；目标父 backing 为 WB coherent 或真实 NC，shadow 为 WB coherent | 沿用实际 mapping 属性 | 零 | view 的 descriptor 不能把外部 NC 页改为 WB；格式或 pitch 的复制需求不是 cache 维护要求 | F6，R10，R7 |
| buffer Memoryless；no-copy Private/Memoryless；heap Managed/Memoryless；Sparse heap；`newTextureWithBytesNoCopy:...` | 当前不形成成功分配 | 对应入口拒绝，或 frontend 未接入 | 无 BO | 不存在维护调用 | 当前接口不支持；不能列成软件维护例外 | F7，R1，R9，R11 |

`newTextureWithDescriptor:iosurface:plane:` 的 Memoryless 组合由 runtime 拒绝。普通 buffer-backed texture 取父 buffer 的 resource options；IOSurface texture 保存自身 descriptor 的 options，但实际 backing 仍是 `surfaceBuffer()` 已导入的页。Shared、Managed、Private 的 API 标签与物理页属性分开保存。

## 当前数据流和丢失字段

### 普通分配

1. Objective-C frontend 原样传 `options` 或 `descriptor.resourceOptions`。Rust bridge 使用 `ResourceOptions::from_raw()` 解出 storage、CPU cache 与 hazard。
2. buffer 的 `Modes` 保存三种模式，`buffer_class()` 把它们转换为 `MemoryClass`。Shared + DefaultCache 为 Shared，Shared + WriteCombined 为 WriteCombined，Private 和 Managed 各有自己的 class。buffer Memoryless 在创建前拒绝。
3. `Memory::candidates()` 对 Shared、Managed 和 unified memory 上的 WriteCombined 优先选择 `HOST_CACHED | HOST_COHERENT`。A642L 的 `VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU` 使 runtime 走 unified memory 分支。
4. Private 与 Memoryless 当前优先条件没有 `HOST_CACHED`；当前普通 image 的 `ImagePlan::memory_class()` 只从 storage 决定 Private 或 Memoryless。开启 type 1 后，仅靠 existing Shared 排序仍不能把这两类 owned backing 全部转到 WB coherent。
5. `BlockMaker::make()` 把 type index 填进 `VkMemoryAllocateInfo`，整块 host-visible memory 映射一次。`Memory` 分别建立 buffer pool 与 image pool，并各按 memory type 分开。小资源从该 block 子分配；分配要求不能在加入 pool 后丢掉。
6. `tu_AllocateMemory()` 从 `memoryTypeIndex` 取 `mem_property` 并传给 Darwin `bo_init`。Darwin 只把该值保存在 `tu_bo::darwin_mem_property` 和复用条件中，发送 `NativeAdrenoAbiBoAlloc` 时仍只有 `bytes`、`align_log2`、`contiguous`。KMD 因而无法执行 Vulkan 所选缓存要求。
7. KMD 当前 `boCreate()` 对 owned BO 强制 `write_combine=true`；`NativeAdrenoDeviceCachedCoherent` 未启用。其结果与当前 type 0 一致，但不能据此公布带 `HOST_CACHED` 的 type 1。

普通 texture 的 host-transfer 分支还会主动筛掉非 coherent host-visible type，并优先 cached coherent type。该行为说明 runtime 的 CPU image 访问依赖无需手动 flush 的合同。加入一个 `HOST_CACHED` 但没有 `HOST_COHERENT` 的类型，不能作为 Shared/Managed 默认路径的实现。

### 外部页导入

1. no-copy frontend 传原有地址、长度与 resource options；Rust `HostMemory::new()` 保留调用方指针。native bridge 没有提供 dma-buf source。
2. `buffer_over_host_memory()` 只允许 Shared 或 Managed。它验证 CPU cache mode 枚举，但导入 class 只按 storage 选择 Shared 或 Managed。`WriteCombined` 没有改变现有 CPU 映射。
3. runtime 通过 `vkGetMemoryHostPointerPropertiesEXT()` 查询 type bits，再用 `VkImportMemoryHostPointerInfoEXT { handleType=HOST_ALLOCATION_EXT, pHostPointer=... }` 和完整 import window 的长度分配。
4. 当前 Turnip pointer query 固定返回 `memoryTypeBits=1`。`tu_AllocateMemory()` 的 host import 分支直接调用 `tu_bo_init_host(pointer, size)`，未传 `memoryTypeIndex` 或 `mem_property`；Darwin 向 KMD 发 `NativeAdrenoAbiBoImport { address, bytes }`。
5. 59b KMD pin 原 backing，读取实际页属性，合格 WB import 的 GPU PTE 使用 WB/WA 与 Outer Shareable，并保存 `io_coherent`。提交和退休使用已保存的结果。UMD 仍未取得 CPU 属性与最终一致性结果，导入 BO 的 `darwin_mem_property` 也未像 owned BO 一样赋值。
6. IOSurface 的 `surfaceBuffer()` 固定传 Shared。其缓存按 surface ID、base address、allocation size 复用仍被 texture 或命令持有的 import，retained surface 保持原 mapping 存活。plane、texture view 与不同 descriptor 不应重新决定这批页的硬件属性。

Vulkan 的 pointer query 没有 length 输入，不能单独决定完整 import window 的属性。目标是查询可支持的类型，在真正 pin 完整范围的导入调用中核对全部页与 aliases，并返回最终属性。若 type 与 backing 不匹配，导入应返回明确错误；不能把 type 1 的要求悄悄改为 type 0。

## 需要保存并传递的分配要求

目标延续 KGSL 的步骤：创建或导入时确定真实 backing 属性，GPU 映射使用这些属性，提交读取已经建立的映射结果。

| 保存位置 | 已有内容 | 目标内容与用途 |
| --- | --- | --- |
| Metal buffer、texture、heap 对象 | `resourceOptions`，storage/cache/hazard；父 heap 或 buffer | 继续保存原调用语义。`WriteCombined` 保持当前 runtime hint 行为；不能用它伪造外部页属性 |
| m2v 资源分配与 pool | `MemoryClass`、`memoryTypeIndex`；buffer/image 按 type 各建 pool | 在 unified memory 且存在 cached coherent type 时，普通 Private/Memoryless backing 也优先该 type；真正 lazy type 仍按支持情况选择。显式保留原 class 与最终 type，子分配只继承所在 block |
| Turnip owned BO | `darwin_mem_property`、alloc flags、alignment、fence、cache 复用键 | 通过新版本分配 ABI 传实际所选 Vulkan memory property 对应的 CPU cache 要求；保留 KMD 回答的实际 CPU cache 属性与 `io_coherent` |
| Turnip host import BO | pointer、size、handle；当前未保存 `mem_property` | import 签名、调用点、KMD ABI 一起传所选 type 的要求。返回实际页属性、GPU 一致性结果，供 BO 与诊断记录保存 |
| KMD allocation/backing | `write_combine`、import 状态、物理 spans；import 的 CPU 属性分类 | 显式区分 owned WB、owned NC、import WB、import NC 与尚未支持的属性。只有内核从真实 backing 得出的结论能决定 `io_coherent` |
| KMD CPU map | owned WC 已有 descriptor CPU map V2 | WB BO 继续通过 descriptor 默认 cache 属性建立 CPU map。不能走旧版强制 WC 的物理映射 fallback |
| KMD GPU map 与退役 | `io_coherent`、span、fence 与 alias 索引 | 在建立映射时固定实际策略；WB coherent 与真实 NC 各保留所需屏障，提交与退休均免逐行数据维护。释放和复用等待最后 fence |
| BO/backing 复用池 | UMD 已比较 `mem_property`；KMD backing 池目前只接受 WC | 复用必须同时匹配实际 CPU 属性、GPU 属性、地址约束和使用目的；同一块 backing 的子资源不能各自切换属性 |

ABI 可沿用 allocator 设计中的 `BoAllocV2`：请求表达 CPU 缓存要求与用途，回答表达实际 CPU 属性与 GPU 映射结果。`coherent=true` 不是用户可指定的可信输入。原版 `BoAlloc` 保持原合同，cached coherent 能力与 V2 分配、V2 CPU mapping、GPU PTE 支持一起公布。

每个 `HOST_CACHED` 分配必须成功得到 WB backing 与硬件一致性映射。若正常 owned 请求失败，返回具体分配错误；不能一面公布 type 1，一面继续创建 WC backing。

### Turnip 自己创建的 BO

Metal 命令会让 Turnip 创建命令流、global、scratch、shader private memory、tessellation 和 query 等 BO。它们是同一目标的一部分。

`tu_bo_init_new()` 当前固定传 `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`，没有 `HOST_CACHED`。`tu_cs.cc:222`、`tu_device.cc:3052` 与 `3468`、`tu_shader.cc:2884`、`tu_pipeline.cc:1181` 使用它。`tu_bo_init_new_cached()` 已按能力加 `HOST_CACHED`，当前调用点是 `tu_query_pool.cc:412`。因此，打开 capability 只会自动覆盖 cached helper 的调用者。

目标是在 Darwin 可用的 WB coherent 分配合同下，让这些普通 owned BO 同样请求 cached coherent。需要固定属性的内部资源必须在实际创建调用中表达该要求，并解释该硬件访问为何不能使用 WB coherent。目前上述普通 BO 调用没有这样的要求证据。

## 软件维护例外的具体判定

当前可成立的“某个 Metal 分配参数要求手动缓存维护”条目数为零。以下附表说明现有慢路径的来源以及对应处理，避免把尚未完成的属性接入变成长期例外。

| Metal 实际入口与参数 | 触发条件和当前处理 | 目标处理 | 为什么不能列为必须手动维护 |
| --- | --- | --- | --- |
| 任意上述成功的 owned buffer/texture/heap 分配 | owned WB 未接通，当前全部 WC | 创建者直接分配 WB，CPU aliases 与 GPU PTE 一致，启用硬件一致性 | 创建者拥有属性选择权；A642L 支持 cached coherent |
| `newBufferWithBytesNoCopy(..., options=Shared/Managed + 任一种 cache mode, ...)` | 完整页范围为 WB | pin 后建立 coherent GPU 映射，保留原 CPU cache 属性 | 59b 已实现这条硬件路径 |
| 同 no-copy 参数；或 `newTextureWithDescriptor:d iosurface:s plane:p` | 实际为 Normal NC，含本机 managed DRAM 的 `VM_WIMG_RT` 映射；59b 的 WB-only 分类可能把它记为 non-writeback | 对照实际 pmap 属性分类为 NC，确认全部 CPU aliases 与 GPU mapping 相容，只执行交接屏障 | NC CPU 访问没有要 clean/invalidate 的数据 cache line；NC 不应记入 WB coherent 计数 |
| 同外部导入入口 | `Unknown` 或 `Mixed` 属性分类 | 创建时解析实际 CPU 类型与 aliases；可支持的每页组合建立正确映射；不支持的范围明确拒绝 | 不知道属性或属性不同，不证明硬件一致性与 NC 都不可用。当前也没有保存到可复现的 Metal 参数与具体页属性组合 |
| `newTextureWithDescriptor:d iosurface:s plane:p`，该 surface 同时给 DPU 扫描 | GPU 完成后的相同 backing 交给显示 | WB 页分别接通 GPU/DPU coherent mapping；NC 页分别匹配 NC；等待 fence 后切换扫描地址 | SM7325 原厂非安全显示通路使用 cached coherent hint；显示扫描本身没有要求 CPU 逐行 clean |

如果未来存在真实外部所有者规定的 cached 内存，且对应设备确实不参加一致性，必须记录原始创建参数、全部 CPU/GPU/外部设备 aliases、不可改为 WB coherent 的硬件约束，以及不可由创建者改为 NC 的合同。只有这些证据连同实际 Metal 导入调用齐备，才形成一条新的数据维护例外。当前没有这样的有效条目。

页表更新、TLB、GPU cache event、command ordering、fence 以及必要的旧属性到新属性转换，仍由各自拥有的代码执行。它们不是每次提交遍历普通 Metal BO 并对其内容逐行维护的理由。

## IOSurface 创建者与扫描输出

`NativeBridgeMetalFrontend` 接收已经创建的 IOSurface。它不调用 `IOSurfaceCreate()`，也不拥有外部 CPU aliases，所以不能在导入时以 descriptor 的 `cpuCacheMode` 重写该 backing。

本地自有示例 `bridge/no_copy_import_host.mm:30-31` 使用 `IOSurfaceCreate` 创建 64 x 16 RGBA surface，参数包含 width、height、bytesPerElement、bytesPerRow、pixel format，未传 `kIOSurfaceCacheMode`；随后分别以 Shared IOSurface texture 与 Shared no-copy buffer 使用同一 backing。这证明两条 Metal 入口能共用原有页，不能证明 SpringBoard/backboardd 创建时传了相同字典。

当前保存证据尚不包含 SpringBoard/backboardd 的具体 IOSurface 创建字典。现有资料仍明确：常规已存在 WB surface 应直接用硬件一致性；真实 NC surface 无需逐行维护；修改新建 IOSurface 的默认属性必须放在实际分配者建立 backing 的入口，并同时建立所有 CPU aliases 与 GPU/DPU 映射的属性合同。不能在 importer 只改一个标签或只改 GPU PTE。

对于自有 `IOSurfaceCreate` 调用，创建参数应表达 WB CPU 使用要求，由 IOSurface backing 分配和内核属性共同落实。当前 frontend 中没有可修改 SpringBoard/backboardd 创建者策略的调用点。为原生创建者新增的适配必须在创建 handler 发布 surface ID、port 或 CPU 地址前取得真实 surface 和 descriptor，对本机自行分配的图形 RAM 建立 WB backing 及全部 CPU 映射，随后固定属性直至释放；接口及已有 descriptor 接入位置见[外部页面设计第4.5节](GPU_IMPORTED_MEMORY_DISPLAY_2026-10-28.md#45-新建和既存-iosurface-如何获得-wb-backing)。Normal NC 不是普通新分配无法改变的硬件限制。

既有外部 NC 的处理只保留兼容性，不能代替新建图形内存的 WB 实现。不能通过首次 Metal 导入强改任意活跃外部映射；旧 surface 无法取得全部使用者的独占访问时，沿免逐行维护路径存续到正常销毁，新创建者再产生 WB backing。新增镜像重启后的常规 IOSurface 必须由 WB 创建路径覆盖。

SM7325 原厂 `yupik-sde.dtsi` 非安全 context bank 配有 `dma-coherent-hint-cached`；ION 根据 cached/uncached backing 分别使用 `DMA_ATTR_FORCE_COHERENT` 或 `DMA_ATTR_SKIP_CPU_SYNC`。本机显示接入应沿同一资源所有权和映射属性实施。GPU coherent 也不能独自替代 DPU 的对应映射。

## 实现落点与源码保存

- frontend 与 bridge 的实际资源对象、参数转交位于公共 `drivers/AdrenoMetal`。若新增诊断或字段，保持资源 options 与实际 backing 属性两份信息。
- m2v 的分配策略变更必须形成 `drivers/AdrenoMetal/bridge/patches/` 内的维护补丁，并登记 `scripts/build-adreno-metal-bridge.py:26-31` 的显式清单；不能只改 `.cache/gpu-reference/metal2vulkan`。
- Turnip type 选择、host import 签名、Darwin allocation/map/retire 数据流变更必须保存到公共 `patches/mesa/` 的维护补丁，并更新 `scripts/data/adreno-compiler-inputs.json:48-59` 的清单。当前相关补丁为 `turnip-darwin-kmd.patch`、`turnip-host-import-images.patch`、`turnip-darwin-bo-lifetime-v2-map.patch`、`turnip-resource-submit-v2.patch`。不能只改 `.cache/mesa-work`。
- KMD 与 `NativeAdrenoABI.h` 位于设备 `drivers/Adreno6xx`，与 Turnip 的所选 ABI header 同步修改。owned allocator、import/display 的详细页属性处理由各自专题文档负责。

后续实现应以每类实际调用的分配结果为合同：Metal options、memory type、KMD CPU cache 属性、GPU/DPU 映射属性和必要屏障都可对应。性能记录用于衡量结果，不能以“多数资源已经命中”代替剩余常规资源的正确接入。

## 源码索引

行号对应 2026-10-28 本地源码，只用于本次设计审阅。

| 索引 | 源码与具体位置 |
| --- | --- |
| F1 | [NativeBridgeMetalFrontend.mm:1362](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm:1362)，普通 buffer 两个入口；[NativeMetalObjects.rs:541](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/drivers/AdrenoMetal/bridge/NativeMetalObjects.rs:541)，解 options 并调用 runtime |
| F2 | [NativeBridgeMetalFrontend.mm:210](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm:210)，descriptor 原样保留 resourceOptions；1384 普通 texture；[NativeMetalObjects.rs:754](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/drivers/AdrenoMetal/bridge/NativeMetalObjects.rs:754)，转为 runtime descriptor |
| F3 | [NativeBridgeMetalFrontend.mm:1373](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm:1373)；[NativeMetalObjects.rs:579](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/drivers/AdrenoMetal/bridge/NativeMetalObjects.rs:579)，deallocator 与 `HostMemory` |
| F4 | [NativeBridgeMetalFrontend.mm:186](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm:186)，195 固定 Shared；1392 IOSurface plane 入口；[no_copy_import_host.mm:30](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/drivers/AdrenoMetal/bridge/no_copy_import_host.mm:30)，自有创建示例 |
| F5 | [NativeBridgeMetalFrontend.mm:1337](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm:1337)，heap；1199 子 buffer，1209 子 texture；[NativeMetalObjects.rs:657](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/drivers/AdrenoMetal/bridge/NativeMetalObjects.rs:657)，Sparse 拒绝 |
| F6 | [NativeBridgeMetalFrontend.mm:315](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm:315)，buffer/linear texture；250 texture view |
| F7 | [NativeBridgeMetalFrontend.mm:1351](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm:1351)，raw-memory texture 未实现 |
| R1 | [buffer.rs:899](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/buffer.rs:899)，Modes；1559 `buffer_class` |
| R2 | [memory.rs:206](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/memory.rs:206)，候选条件和稳定排序；140 cached coherent mask |
| R3 | [values.rs:50](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/values.rs:50)，cache mode；120 options 位布局 |
| R4 | [buffer.rs:2103](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/buffer.rs:2103)，Private contents；[device/properties.rs:25](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/device/properties.rs:25)，Integrated 对应 unified memory |
| R5 | [managed.rs:7](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/managed.rs:7)，unified Managed 语义；39 `did_modify_range`；[buffer.rs:1146](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/buffer.rs:1146)，仅非 unified 创建 mirror |
| R6 | [image_plan.rs:333](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/image_plan.rs:333)，image class；[texture.rs:3312](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/texture.rs:3312)，普通 texture 实际调用 |
| R7 | [texture.rs:3184](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/texture.rs:3184)，host transfer 类型过滤；2024 shadow image 分配 |
| R8 | [texture.rs:1857](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/texture.rs:1857)，Memoryless 创建；3143 backing 初始化选择 |
| R9 | [buffer_descriptor.rs:406](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/buffer_descriptor.rs:406)，Shared/Managed import；[memory.rs:982](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/memory.rs:982)，实际 Vulkan host import |
| R10 | [buffer_texture.rs:403](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/buffer_texture.rs:403)，IOSurface plane；472-487 选择 descriptor 或父 buffer options；[linear_plan.rs:418](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/linear_plan.rs:418)，IOSurface Memoryless 拒绝 |
| R11 | [heap.rs:467](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/heap.rs:467)，heap 分配与模式限制；1008 子资源模式匹配 |
| R12 | [memory.rs:682](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/memory.rs:682)，按 type 建两组 pool；1140 `BlockMaker::make`，1162 传 memoryTypeIndex |
| V1 | [tu_device.cc:1299](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_device.cc:1299)，Integrated GPU；1763-1788 memory type 表 |
| V2 | [tu_device.cc:3798](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_device.cc:3798)，owned mem_property；[tu_knl_darwin.cc:1470](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_knl_darwin.cc:1470)，Darwin 接收属性；1523 旧 BoAlloc；1576 保存 mem_property；1761 CPU map V2；1803 旧映射强制 WC |
| V3 | [tu_knl_darwin.cc:2901](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_knl_darwin.cc:2901)，lazy/sparse 与 cached coherent capability |
| V4 | [tu_device.cc:3732](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_device.cc:3732)，host 分支未传 type；3868 pointer query 固定 bit 0；[tu_knl_darwin.cc:1663](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_knl_darwin.cc:1663)，BoImport 请求；1695-1701 保留 pointer 与 handle |
| K1 | [NativeAdrenoABI.h:123](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoABI.h:123)，BoAlloc；[NativeAdrenoUserClient.cpp:268](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoUserClient.cpp:268)，强制 owned WC |
| K2 | [NativeAdrenoVm.cpp:222](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:222)，当前仅合格 import 使用 WB coherent；454 保存结果；[59 记录](../GPU_COHERENT_IMPORTS_59_2026-10-28.md)，import 属性分类及交接流程 |
| K3 | [SM7325 managed RT 补丁](/Users/rigoligo/expm/unixnu-sm-t733/sources/unixnu-sm7325-common/patches/xnu/native-t733-managed-rt-memory.patch)，managed DRAM 的 RT 映射 Normal NC；具体别名和显示接入见[导入内存及显示一致性](GPU_IMPORTED_MEMORY_DISPLAY_2026-10-28.md) |
| K4 | [yupik-sde.dtsi:149](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/arch/arm64/boot/dts/vendor/qcom/display/yupik-sde.dtsi:149)，非安全显示 cached coherent hint；[msm_ion_dma_buf.c:162](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/staging/android/ion/heaps/msm_ion_dma_buf.c:162)，cached/NC 的 DMA 属性；[dma-iommu.c:629](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/iommu/dma-iommu.c:629)，coherent 转换到 IOMMU 属性；785 对应 CPU sync 条件 |

