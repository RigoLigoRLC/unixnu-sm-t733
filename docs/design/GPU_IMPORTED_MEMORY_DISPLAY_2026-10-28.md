# 导入内存、IOSurface 与 DPU 的页属性和访问交接

日期：2026-10-28。本文是设计和现有源码证据，不表示实现、构建或实机结果。范围包括用户地址导入、IOSurface、CPU 所有映射、A642L 和 DPU；驱动自有分配器由同目录的分配器设计负责。

目标是让普通 SpringBoard/backboardd 渲染和直接扫描 IOSurface 默认使用 WB 内存以及 A642L、DPU 的硬件一致性，完全去掉逐行 CPU cache maintenance。普通 A642L 渲染 BO 没有已发现的硬件条件要求必须采用 NC。由本机控制的 IOSurface backing 也要在建立时选 WB；真正由外部协议约束的 NC 保留无缓存访问和屏障。页属性在分配、导入、建立映射、修改属性和释放时确定并维护，提交和完成回收读取已经建立的记录。

## 1. 59b 已保存的事实和真正需要修改的判断

工作区 `logs/gpu-coherent-59b-runtime/interaction-1.raw:3891-3892` 记录 `maps=142`、`mapped_bytes=51417088`、`non_writeback=5`、`unknown=0`、`mixed=0`。同文件 `3898-3905` 等提交记录中，`imports=1`、`import_bytes=32768000`，CPU `cache` 仍占主要提交时间。

`NativeAdrenoSubmit.cpp:784-796` 仅对 `resource.maintenance` 的范围累加 `import_bytes`，因此 32,768,000 是本次发布时需要维护的导入范围总量。`import_count` 按 `owns_reference` 累加，范围数、物理 backing 数、IOSurface 数不能从这个计数互相替换。日志没有保存这批范围的 handle、IOSurface ID、创建参数或原始 WIMG 值。尽管数据不完整，我们仍知道这些提交持续对一批已导入范围执行了软件发布，且现有分类丢掉了决定是否需要维护的信息。

现有判断链如下，路径相对设备仓库根：

| 位置 | 当前行为及影响 |
| --- | --- |
| `drivers/Adreno6xx/NativeAdrenoMemory.cpp:698-756` | `importTask` 以原任务地址创建 descriptor，`prepare` 固定物理页，收集物理 spans，再查询一次页属性。 |
| `drivers/Adreno6xx/NativeAdrenoCpu.cpp:205-236` | 页属性只有 `WriteBack`、`NonWriteBack`、`Mixed`、`Unknown` 四种结果；只有 `VM_WIMG_COPYBACK` 被识别为 WB。Normal NC、RT、WT 和其他属性全部并入 `NonWriteBack`。 |
| `drivers/Adreno6xx/NativeAdrenoVm.cpp:711-715` | 任意非 coherent 的 imported memory 都被标记需要维护。 |
| `drivers/Adreno6xx/NativeAdrenoMemory.cpp:892-918` | 任意 import 发布都 clean，GPU 写后都 invalidate 或 clean/invalidate；已确认 WC 的 owned BO 才直接使用屏障。 |
| `drivers/NativeMobileDisplay/NativeDpuSurface.cpp:287-289,535-590` | 每次 present 重新取得物理页，所有 DPU leaf 固定 Normal NC，然后对整个可见表面执行 `cleanSurfacePages`。 |

后一项是独立的显示成本。同一份 raw `2704-2710`、`2880-2881`、`3043-3044` 记录 1600x2560、stride 6400 的 DPU 表面和约 489-738 us 的 `clean`。这部分不包含在 GPU 提交的 `cache` 数字里。GPU 免维护后必须同时删除 DPU 的无条件整表面 clean。

当前配置已有一种具体的 Normal NC 来源。raw `743` 保存 `t733-managed-rt-normal=1`；当前 XNU `osfmk/arm/pmap/pmap.c:5859-5868` 将 managed DRAM 的 `VM_WIMG_RT` 转为 `CACHE_ATTRINDX_WRITECOMB`。这个属性实际是 Normal NC，而 59b 的分类仍把其原始 WIMG 归入 `NonWriteBack`。说明见工作区 `notes/SOFTWARE_DISPLAY.md:302`。这是已有代码和启动配置支持的候选来源，不能据此把全部五个导入认定为 RT，也不能把 RT 等同于 Metal 的 `WriteCombined`。

## 2. 原厂参考给出的可直接移植行为

下列路径相对工作区 `references/android_kernel_samsung_sm7325/`。

### 2.1 KGSL 的导入属性和缓存操作

1. `drivers/gpu/msm/kgsl.c:2599-2611` 从用户 VMA 的真实页保护区分 WC/NC 与 WB；`2688-2696` 在 cached backing 和 GPU 支持同时成立时开启 `KGSL_MEMFLAGS_IOCOHERENT`。
2. `kgsl.c:2815-2832` 从 ION exporter 的 `ION_FLAG_CACHED` 决定导入是否 cached，并决定是否启用一致性。`3010-3016` 对 coherent attachment 设置 `DMA_ATTR_SKIP_CPU_SYNC`。
3. `kgsl_sharedmem.c:689-705` 限制一致性只用于 cached allocation；NC backing 不能因为芯片支持一致性就强制打开 snoop。
4. `kgsl_iommu.c:1687-1688` 将实际 `IOCOHERENT` 转为 `IOMMU_CACHE`。`kgsl_sharedmem.c:636-637` 直接跳过 coherent BO 的 CPU 维护；`kgsl.c:3414-3419` 直接跳过非 cached BO 的维护。

本机可移植的是“分配或导入时由实际 backing 决定策略，并在映射上保存”的程序。Linux VMA、ION 和 dma-buf 的记录由 XNU 页管理、IOSurface descriptor 和本机 BO 记录提供；不能把应用提交的 MTL options 当作 exporter 证明。

### 2.2 SM7325 DPU 也有按 backing 选择的一致性路径

1. `arch/arm64/boot/dts/vendor/qcom/display/yupik-sde.dtsi:149-155` 的非安全显示 context 使用 SID `0x900`、mask `0x402`，并声明 `dma-coherent-hint-cached`。
2. `include/linux/device.h:1242-1244` 定义此属性为对 CPU cached buffer 按 DMA coherent 处理。`drivers/of/device.c:158-176` 将其与无条件 `dma-coherent` 分开传递。
3. `drivers/staging/android/ion/heaps/msm_ion_dma_buf.c:162-180` 对 cached、HLOS 可访问、具有该 hint 的显示 attachment 设置 `DMA_ATTR_FORCE_COHERENT`；对 NC buffer 设置 `DMA_ATTR_SKIP_CPU_SYNC`，并拒绝把 NC buffer 强制映射成 coherent。
4. 同文件 `198-205` 把这些属性传入实际 DMA mapping。`drivers/iommu/dma-iommu.c:629-638,435-450,957-984` 把 `FORCE_COHERENT` 转为 `IOMMU_CACHE` 并建立页表。
5. `drivers/iommu/io-pgtable-arm.c:163-169,591-593,392` 选择 MAIR `0xff`、index 1、Outer Shareable。NC 使用 MAIR `0x44`；LLCC hint 有独立属性。
6. `drivers/iommu/dma-iommu.c:785-798` 依据实际 IOVA 是否 coherent 决定跳过 CPU 同步。快速 mapping 实现的同一判断在 `dma-mapping-fast.c:305-349`，并不是仅设置一个未被使用的标志。
7. `techpack/display/msm/msm_gem.c:1298-1307,1231-1250` 对 NC 外部 buffer 跳过 map/unmap 的 CPU 同步。`msm_gem_prime.c:174-195` 提前 attachment cached buffer，以便使用 CPU begin/end access；`msm_gem.c:741-748,866-869` 只在实际 CPU 映射访问时调用对应接口。

本机 DPU 已在 `NativeDpuSurface.cpp:505` 设置 MAIR `0x0000ff44`，但 leaf 创建函数 `287-289` 永远选 index 0。显示 SMMU 的 ACTLR `0x103` 已在 `507` 设置，与原厂 `msm-arm-smmu-yupik.dtsi:164-171` 的显示项一致。设计应让 WB 表面使用 index 1、Outer Shareable、只读，NC 表面保留 index 0。这有原厂同 SoC 路径可依据，不应为 DPU 另保留每帧全表面 CPU clean。

SMMU 读页表的一致性、图像数据的一致性、LLCC allocation hint 是三个独立配置。页表发布和 TLB 同步继续按各自接口完成；表面内容免维护不能删除页表发布，也不能用 LLCC hint 代替对 CPU cache 的一致性。

### 2.3 GPU 到 DPU 的完成交接

原厂 `drivers/gpu/msm/adreno_ringbuffer.c:651-676` 在流水线结束发出 `CP_EVENT_WRITE` 的 `CACHE_FLUSH_TS`，`a3xx_reg.h:37` 定义该事件为 4；显示在 `techpack/display/msm/sde/sde_plane.c:617-637` 等待 input fence，然后使用 framebuffer mapping。

本机 A642L `NativeAdrenoRing.cpp:212-216` 同样写事件 4 的完成 timestamp，`224-235` 在其前发出颜色、深度 cache 事件。`NativeAdrenoPublish.cpp:1954-1975` 等待 fence、回收已完成任务，再执行 DSB。CPU cache 操作不能代替这些 GPU cache 事件；DSB 也不能把尚未完成的 GPU 内部写入变成已完成的写入。

当前 `native_adreno_capture_surface_producer` 在 `NativeAdrenoPublish.cpp:1998` 保存的是整个 ring 的最后提交 fence，没有表面或字节范围身份。设计将其升级为与 backing 身份和写入范围绑定的交接记录，显示只等待相应生产者。现有全 ring 等待仍可以提供顺序，但不能证明某个表面的 CPU 曾写过或从未写过。

## 3. 由页属性决定的正常策略

新记录必须同时保留原始 `cpu_wimg` 和由当前 pmap 配置解析出的 `cpu_memory_type`、`inner_cache`、`outer_cache`、`shareability`、CPU alias 状态。`NonWriteBack` 不再是可执行策略。

| 实际 backing 状态 | A642L mapping | DPU mapping | 每次访问交接的 CPU 操作 |
| --- | --- | --- | --- |
| 所有 CPU alias 为兼容的 Normal WB，页属性受到生命周期管理 | WB/WA、Outer Shareable，硬件一致性启用 | WB/WA、Outer Shareable、read-only，硬件一致性启用 | 发布屏障、GPU 完成 fence、读取屏障；无逐行 clean/invalidate |
| 所有可访问 CPU alias 为 Normal NC，且外部 owner 有具体 NC 要求；既存 RT-to-NC 可用作过渡输入 | Normal NC，硬件 snoop 关闭 | Normal NC、read-only | 使 CPU stores 在设备启动前完成的屏障；设备完成后读取屏障；无逐行操作。新版普通本机 IOSurface 必须从创建时使用 WB。 |
| 不同物理页分别为上述 WB 或 NC | allocation/import 时合并为属性相同的连续区间，各区间选择对应 leaf | 复用同一页区间属性 | 遍历已经提交的资源和少量区间；没有逐页查询或逐行数据维护 |
| 同一物理页仍同时存在 WB 和 NC 可访问 alias | 导入事务先统一所有 alias；完成之前不发布 GPU mapping | 同样不发布 DPU mapping | 属性转换时一次性完成 cache/TLB 操作；不能用每帧维护保留长期不一致属性 |
| WT、inner-WB 或其他可缓存 RAM | 由 pmap 解析实际属性，在 exporter 允许时规范为与两个设备一致的 WB；只有确定硬件支持相同内外缓存和 shareability 才直接映射 | 使用同一规范结果 | 不把字符串 `non_writeback` 作为软件维护依据 |
| 未知物理区或 Device 类型页 | 导入阶段向拥有该内存的 descriptor/exporter 取得属性和许可；无法建立契约则明确拒绝这次导入 | 同一条件 | 不接受为普通慢速 BO；不从物理地址落在某一区间推断一定是 MMIO 或 NC |

可正常共享的混合页分布不需要整 BO 降级。属性区间在 import 时建好；相同 PA 的重复导入引用同一个物理页记录，使用相同数据属性。资源映射中的属性与物理页记录代次不符时禁止新提交，由映射更新流程处理。

已有 WB 页必须启用两个设备的一致性。由本机控制的新 IOSurface backing 从创建时使用 WB，发布后保持到 backing 释放。旧 NC/RT IOSurface 只有在 owner 能提供全部 CPU 和设备使用者的独占访问时才一次性转换；首次 import 不能强制接管任意活跃 CPU mapping。新版本重启后的普通表面均经创建接口直接取得 WB，不能依赖旧 NC 兼容路径完成目标。应用的 `MTLCPUCacheModeDefaultCache` 不是单方面改变另一个 owner 页属性的授权。

## 4. 页属性与全部 CPU alias 的完整生命周期

此节是本机新增设计。KGSL/ION 的 exporter 生命周期提供参考，XNU 当前没有直接可移植的同名对象。

### 4.1 当前 XNU 的约束

此处 XNU 行号来自工作区当前组装源码：

`out/t733-kernel/gts7fewifi/ipados-17.7.11-21H461/gts7fewifi/tree/.cache/xnu-work/`

- `osfmk/arm/pmap/pmap.c:10465-10490` 的 `pmap_cache_attributes` 返回物理页记录中的 WIMG，不是全部 CPU PTE 的枚举结果。
- `iokit/Kernel/IOMemoryDescriptor.cpp:968-971,1142-1145` 可通过 `MAP_MEM_ONLY` 修改 named memory entry 的 cachemode。descriptor 已 prepare 不阻止此操作。
- `pmap.c:6304-6315` 新增映射可以携带显式 WIMG，也可继承物理页属性；因此仅限制 GPU user client 自己建立的 CPU view 不足以管住其他 alias。
- `pmap.c:10982-11041` 在 PVH 锁下更改物理页属性并更新已有 PTE，随后执行属性转换同步。`11116-11139` 更新 physical aperture 的分支依赖 `__ARM_PTE_PHYSMAP__`。
- `osfmk/arm64/proc_reg.h:2726-2732` 只在 XNU_MONITOR 配置定义该宏。`arm_vm_init.c:1113-1139` 允许把受管理 DRAM 的 aperture 建成 WB、Outer Shareable 的 L2 block。当前驱动 `NativeAdrenoCpu.cpp:239-244` 也明确记录本机 aperture 仍为 cached block。

因此物理页标记为 WC、用户 PTE 已为 NC，仍不足以宣称不存在 cached alias。现有“aperture 不被使用”的约定没有覆盖所有内核调用者；NC 正常路径必须在页管理中处理这个映射。

### 4.2 拟增加的持有记录

沿用分配器设计的拟议名字 `NativeAdrenoPageAttributeLease`。记录本体由 XNU 页属性管理组件拥有，GPU 和显示仅持有引用；名字不意味着 XNU 将页属性所有权交给 GPU。

| 字段或关系 | 需要保存的内容 |
| --- | --- |
| `physical_page` | 受管理页身份；外部 reserved RAM 使用 exporter 登记的区间身份。 |
| `cpu_wimg`、`effective_attributes` | 原始请求和当前实际 Normal/Device、内外缓存、shareability。 |
| `generation`、`state` | 当前映射代次及 `Active`、`Changing`、`Retiring` 状态。 |
| `cpu_aliases` | PVH 覆盖的 kernel/user 映射、named entry 属性，以及 physical aperture 的实际 leaf 或 block 拆分引用。 |
| `gpu_mappings`、`display_mappings` | 已建立设备映射的引用和使用的实际数据属性。 |
| `gpu_jobs`、`display_reads` | 活跃 GPU job 和仍在扫描的 DPU 读引用；用于决定何时允许修改或回收。 |
| `owner` | descriptor、IOSurface backing 或外部内存提供者；负责分配、CPU 访问和最终释放许可。 |

`NativeAdrenoBackingStorage` 持有这份记录及预先合并的物理属性区间，`NativeAdrenoVm::Mapping` 保存所使用代次和实际 GPU leaf 属性。显示 mapping 保存同一记录，不能自行重复推断。两个不同用户地址指向同一物理页时，必须得到同一页记录。

### 4.3 导入、创建 alias 和改属性的顺序

1. import 固定原 owner 和 descriptor，prepare 并收集实际物理页；在页管理事务中登记这些页，同时取得现有映射的实际属性。锁保证读取属性和登记持有之间没有属性修改窗口。
2. 管理组件决定每页 WB coherent 或 Normal NC。NC 页对应的 aperture block 在此阶段拆成页表，保持其余页原属性；对目标页完成必要的一次性 cache、break-before-make、TLBI、DSB/ISB 顺序。不得仅为 GPU 另建 NC mapping，却继续留下可访问 WB aperture。
3. 更新已经存在的 CPU alias；后续建立 user/kernel PTE、named entry 映射和 pmap 显式 WIMG 映射时，都必须通过同一记录选择兼容属性。prepare 负责 pin，持有记录负责防止属性被独立改变，两项职责同时存在。
4. 全部 CPU alias 达到目标后，生成 GPU/DPU leaf 并保存 `generation`。CPU 写入初始化内容后在设备访问前执行发布屏障。
5. owner 确需不兼容属性变更时，先提供全部 CPU 使用者的独占访问和新 CPU mapping 停止条件，再把记录设为 `Changing`，禁止新 job 和 scanout 引用。持有原 GPU/DPU fence，等所有旧访问完成，解除旧设备 mapping，执行属性转换，再以新代次重建映射。无法独占已发布 CPU view 时返回错误，保持原属性。
6. GPU 提交和完成回收只持有 mapping 引用并检查其有效状态/代次；不枚举物理页、不调用 `pmap_cache_attributes`、不扫描所有 CPU alias、不把 reassert 页属性放回热路径。

等待设备 fence 不能发生在 PVH 或 pmap 自旋锁内。VM/IOKit 的可失败外层接口先完成等待或返回明确的忙状态，再进入短暂的 PTE 修改阶段。当前 `pmap_set_cache_attributes` 返回 void，因此不能在它内部静默忽略不兼容变更；新增可返回状态的事务入口，并让 named entry、批量属性变更、pmap 显式属性调用者先取得该事务，再调用底层修改。全部底层入口都要遵守持有记录，不能仅修 `makeWriteCombined`。

NC aperture 的拆分页表在记录存活期间保留。解除最后一个设备 mapping 和 CPU 视图引用后，owner 才能归还页；属性恢复和可能的 block 合并属于释放事务。backing pool 继续保留记录和已经完成的属性，不允许空闲期间恢复默认 WB、复用时再反复切 NC。

### 4.4 每一个实际修改入口如何返回或延后

新增的无等待入口暂名为 `pmap_try_change_device_page_attributes`，返回 `kern_return_t`，接收页集合、目标属性和 owner 授权的事务对象。它先预留整个范围，再执行修改；某页有冲突时整个范围保持原状态。`KERN_PROTECTION_FAILURE` 表示调用者请求不兼容属性；`KERN_RESOURCE_SHORTAGE` 表示准备页表或记录失败。不能借用当前 `pmap_batch_set_cache_attributes` 的 bool 返回值表达错误，它在 `pmap.c:10612-10641` 实际一直完成循环后返回 true。

| 现有入口 | 可实现的改动、返回与锁规则 |
| --- | --- |
| `vm_user.c:2667-2717` 的 `MAP_MEM_ONLY` | `vm_object_change_wimg_mode` 改为返回 `kern_return_t`。在修改 `parent_entry->access` 前预留 object 属性事务；成功后同时提交 object 和 entry 属性。当前 `2701-2703` 提前写 entry 的顺序必须移动。冲突直接从现有 Mach 接口返回错误，保留旧 entry、object 和所有 PTE。 |
| `vm_user.c:3155-3165` 的新 memory entry | 同样取得并传播属性事务返回值。失败时释放本次临时 copy/entry 引用，不能发布一个声称新属性而物理页仍为旧属性的 entry。 |
| `vm_object.c:7334-7355` | 该函数当前持 object 锁逐页修改。新实现先检查 object 的设备引用与属性事务状态；不兼容 live 变更立即返回。owner 已安排且设备已完成的转换使用预先准备的事务，无 fence 等待。批量 pmap 更新成功后才写 `set_cache_attr` 和 `wimg_bits`。 |
| `IOMemoryDescriptor.cpp:1142-1146`，以及同文件 `1275-1278,1450` 的映射分支 | 显式 cachemode 调用 `mach_make_memory_entry` 的失败必须正常传播到 mapping 创建者；移除“必定成功”的断言假设。相容属性和默认 cachemode 按 object 已登记的 WB/NC 建映射。 |
| `osfmk/device/iokit_rpc.c:465-520` 的 `IOMapPages` | 使用返回状态的全范围事务代替 `514` 的单页 void 修改。完成预留后才更新 cache 属性和建立 PTE；冲突返回 `kern_return_t`，不影响旧映射。reserved RAM 走其 exporter 区间登记。 |
| `IOLib.cpp:1449-1482` 的 `IOSetProcessorCacheMode` | 在 `1472` 解除旧页之前，为整个 address/length 预留事务并准备新页表。冲突返回 `kIOReturnBusy` 或相应参数错误；不能先 unmap 再发现不能换属性。成功时通过一次事务切换整个范围。 |
| `pmap.c:6304-6315` 的新 PTE | 在既有 PVH 锁内查询该页记录。default 请求采用已登记属性；相容显式属性成功，不相容请求从已有 `pmap_enter` 状态接口返回错误。只检查正在建立映射的页，绝不移到 GPU 提交。记录是预先准备好的，PVH 锁内不分配、不等待。 |
| `vm_resident.c:1871`、`vm/pmap.h:468-503` 的新页插入与批量属性宏 | 新页插入前按 object 已登记的 effective attributes 准备页；不能从旧 `wimg_bits` 再恢复 RT。对已有页的批量操作先获得全范围事务，原宏改为调用带状态的函数并向 UPL/VM 外层传播失败。只有确定是新分配且没有设备记录的页，才允许使用内部不可失败的属性设置。 |
| `vm_resident.c:2298-2300` 的移出 object/default 恢复 | pin 与 backing 引用保证 GPU/DPU 仍持有时页不进入此释放阶段。最后一个使用者通过工作队列先完成设备解除映射、CPU alias 解除和属性恢复，再 unpin/归还。这里的 void 路径只处理已经满足“没有设备引用”的页，不承担等待。 |
| `pmap.c:10563,10806,11036` 的直接和批量底层更新 | 全部接受同一事务的预留状态；对无设备持有的新页允许原路径，对持有页要求相容属性或 owner 已完成 drain 的事务。compressor 等内部路径不能绕过：设备页保持 wired，不参与压缩，解除 pin 后方可恢复普通 VM 生命周期。 |

原 `pmap_set_cache_attributes` 可以保留为只供“无设备记录的新页”或“已持事务”的内部包装，但所有上述可遇到用户请求失败的调用者都要迁移到有返回值的入口。内部包装遇到违规是内核调用契约错误；设计不通过吞掉错误、声称成功或临时取消硬件一致性来继续正常运行。

live backing 确需转换时，owner 先取得全部 CPU 使用者的独占访问，停止新增 CPU view；然后在未持 VM object、named entry、map、pmap、PVH 锁的工作队列上取得强引用并标记 `Changing`，收集 GPU/DPU fence 后放开页记录锁等待。设备完成回调只更新记录和唤醒等待者，不获取 VM object 锁。等待结束后按固定 object/map/PVH 顺序重新取得锁，确认 object 和代次仍匹配，执行预分配的转换，再发布新代次。kernel direct/aperture 使用者也必须包含在 owner 的独占访问条件中。发生 reset、超时或 owner 无法独占时撤销新事务并保留旧页和旧属性，不能提前 unpin。

同一 owner 的临时 import 结束只释放 import 持有；仍被 IOSurface 使用的 WB backing 不恢复为 RT/NC。default 恢复是 backing 最终回 VM 时的动作。池中的 backing 继续持有原页记录，无需再次切换属性。

### 4.5 新建和既存 IOSurface 如何获得 WB backing

需要增加可信的 IOSurface backing 适配接口，暂名 `native_iosurface_register_render_backing(surface, descriptor, backing_kind, requirements)`。调用者提供原生 IOSurface 对象和真实 descriptor；XNU/IOKit 由 descriptor 的 memory reference 找到实际 VM object/named entry。应用不能通过一个 import flag 自报自己是 IOSurface，也不能仅按进程名、调用栈地址、RT 属性或表面宽高认定用途。

`backing_kind` 明确区分原生 IOSurface 自行分配的 managed RAM、外部提供者的 RAM 和物理保留区。`requirements` 保存创建时明确的 CPU cache 请求以及额外设备 attachment 约束。原生自行分配、用于本机 Metal/DPU 且没有外部硬件 NC 要求的内存，选择 canonical WB。`kIOSurfaceCacheMode` 的旧 RT 选择在该 owner 的请求解析层转换为本机 WB backing 策略，并让 IOSurface 查询和后续 IOKit 映射读到实际有效属性；不在通用 `wimg_to_pte(VM_WIMG_RT)` 分支全局替换。

原生组件的创建处理必须接入上述接口。具体程序是在 native IOSurface 创建 handler 完成对象和 descriptor 建立后、向用户发布 surface ID/port 或 CPU 地址之前，由本机适配 handler 调用登记接口。若可在 descriptor/VM object 产生之前传入 owner 和用途，直接分配 WB；若原实现内部已经创建了 RT 页，则对象尚未发布且无设备引用，此时完成一次全 alias WB 转换，再允许创建 handler 返回。该适配 handler 应使用匹配当前系统的原生 ABI；当前设备源码仅已有 `native_legacy_surface_descriptor(surface)` 的原生 descriptor 接入（`NativeLegacyDisplay.cpp:1418-1424`），创建 handler 的绑定是本次需新增的原生组件集成，本文不虚构现有 selector 编号或称它已经实现。

既存 surface 的接入点是第一次 Metal IOSurface 导入或第一次 DPU descriptor 登记。现有 Metal 内核 import 只收到 task/address/length，不能从这三个参数取得可信 IOSurface owner；需要新增一条明确的 IOSurface import ABI，而非给普通 raw import 加一个可伪造的布尔标志。

| 新增接口 | 输入、输出和受控调用方 |
| --- | --- |
| Metal 用户态 `ImportIOSurface` 请求 | `surfaceBuffer` 在 `NativeBridgeMetalFrontend.mm:195` 原先降为 raw no-copy 的位置，传入由真实 surface 取得的 Mach send right、allocation 范围及 plane 信息。新请求保留 IOSurface 身份至内核。用户态地址只用于建立其 CPU view，不能决定 owner。 |
| `native_iosurface_copy_backing_from_port` | 输入调用 task 和其 port name，使用原生 IOSurface port/kobject 规则取得并 retain 真实 IOSurface，取得原 descriptor 和原创建属性。输出 retained surface、retained descriptor、实际 VM object/named entry 引用、allocation 身份、外部 backing/保护标志及 CPU cache 请求。此新适配函数仅供内核导入 handler；不能把任意 OSObject 或用户指针转换成 IOSurface。 |
| `native_iosurface_copy_backing_from_object` | 显示已有 `native_legacy_surface_descriptor(surface)` 的位置调用同一登记逻辑，输入由原生已验证 swap request 持有的 surface。输出与 port 路径相同。当前显示路径已能拿到真实对象和 descriptor，新增部分是 VM object 关联、创建属性和 owner 登记。 |
| IOKit `register_graphics_memory_reference` | 输入上述 retained descriptor、真实 IOSurface owner 和固定的 `MetalAndDpu` 用途。沿 descriptor 内部 memory reference 取得 VM object，登记 WB policy 和页属性记录，返回持有引用、属性区间、代次和必要的转换事务。这个接口位于 IOKit 内部，不能要求驱动读取 descriptor 私有布局。 |

port/kobject 到 IOSurface 的绑定和创建属性提取需要在当前系统原生 IOSurface ABI 中新增适配。现有 `native_legacy_surface_descriptor` 已覆盖对象到 descriptor 的一段；它尚未覆盖 port 解析、外部 backing 标志和 cache 请求。实施时要精确补齐这些 native 边界并连接上述有类型的接口，不能凭任务名或 descriptor 地址直接标记可信。创建 handler 返回前完成 WB 登记是本方案的完成条件；首次 kernel import 的完整桥接只负责绑定并持有已经确定的属性，不能取代创建入口或兜底强改活跃页。

原生自行分配的 managed 图形 RAM、无外部内存提供者、无保护域且全部设备 attachment 已纳入管理时，平台内存管理组件有权改变实现上的缓存方式，同时保持 IOSurface 数据与共享语义。显式来自外部的 NC 协议必须由真实创建/导入属性确认。只持有一个 IOSurface 引用不构成外部 owner 同意改变硬件契约；不能确认上述条件的对象不得执行转换，也不能作为普通慢速 import 发布。

新的原生创建 handler 在发布 surface ID/port/CPU 地址前完成登记。此时创建者持有 backing，CPU user view 和设备 attachment 尚未对外可用；原实现内部已经建立的 kernel mapping 也仍由创建者独占。若原分配先用了 RT，则创建者在返回前停止自己的初始化访问，转换该受控 kernel mapping、named entry 和 aperture 为 WB，然后继续发布。这样无需暂停全内核，也无需每次 CPU store 加检查。

已发布的旧表面采用以下收束规则：

1. 首次导入取得真实 owner、descriptor 和引用，查询已有属性；实际 WB 直接建立 coherent mapping。
2. 实际 NC 若需转 WB，owner 必须证明所有 CPU view 的使用者已经退出访问、不能新建 view，且没有未纳入协议的 kernel direct/aperture 访问。原生 IOSurface fence本身不足以证明这一点，永久 raw pointer 也必须由持有者交回独占访问。
3. 获得该条件后才标记 `Changing`、等待 GPU/DPU fence、转换全体 alias。不能获得独占条件就保持原属性；允许范围仅限明确的旧对象过渡或真正外部 NC 契约。普通新对象必须修正创建 handler，不能转入常规 NC 列表或每帧 clean。
4. 下一次正常重建表面或重启后，从已接通的创建 handler 获得 WB backing，普通路径不再需要迁移旧映射。本文不要求为转换任意旧表面建立全 kernel 暂停或拦截每次内存访问的系统。

新建 WB surface 发布后的 ID、CPU 地址、像素内容和跨进程共享关系沿用原对象。新的 CPU 映射沿 object 的 WB 属性建立，来自已登记 IOSurface owner 的旧内部 RT 请求按该 object 的有效 WB policy 处理；真正不兼容的外部属性变更按 4.4 返回失败或在完整独占条件下安排转换。

普通 raw no-copy 内存没有可信 IOSurface owner，按实际 WB 页直接 coherent；确实属于外部 NC 内存的 pointer 由外部 owner 契约处理。任何需要把原生组件创建策略改成 WB 的工作都在 backing 建立或一次性接管中完成，不能把现有 NC 作为普通渲染永远不迁移的理由。

## 5. IOSurface 和 DPU 的访问交接

现有 `NativeLegacyDisplay.cpp:1433-1448` 先检查原生 IOSurface 的读 fence，再等待 producer，最后进入 `t733_dpu_surface_begin`。`NativeDpuSurface.cpp:696-704` 接管这个 fence；`765-782` 在 CTL 和帧计数确认切换后解除前一帧 mapping；`313-334` 最后 complete descriptor 并完成该读 fence。这些活跃读取引用必须保留。

目标过程如下：

1. IOSurface 第一次作为 GPU 或显示资源导入时，从真实 backing 建立统一页记录。显示取已有记录和预先合并的物理属性区间；无 GPU 导入的 CPU 绘制表面也通过同一入口登记。
2. GPU job 保存它实际写入的 backing、区间和完成 fence。该 fence 包含 A642L 的 cache flush timestamp，完成后生成与该 backing 代次对应的生产者完成记录。
3. IOSurface 的原生 read fence 阻止并发 writer。DPU 获取同一 backing 的读引用，等待对应 producer fence，确认页属性记录仍为当前代次，执行必需的内存屏障。
4. WB 表面使用 coherent DPU leaf；NC 表面使用 NC leaf。两种路径都跳过 `cleanSurfacePages`。来自 CPU 的 WB 写入由 DPU coherent 读取，来自 CPU 的 NC 写入由发布屏障完成。
5. DPU 更新指向目标 mapping 的地址并发出 CTL flush；帧切换完成后释放旧表面的扫描读引用，完成旧 IOSurface read fence。错误或超时保持旧引用，直到硬件停止访问或恢复到固件 framebuffer 已被确认。

显示 mapping 可按 backing 身份、页属性代次和扫描范围复用。空闲 mapping 的缓存有容量和内存压力回收上限；保留页映射不等于表面一直处于扫描状态。每次扫描的读 fence 独立获取和释放。页表只有新增、删除、属性变化才更新和 clean/TLBI，普通切帧不再重新走 `getPhysicalSegment` 的逐页循环。

对于后续接入的确实不支持一致性的外设，设备能力和访问方向必须登记在该外设 attachment 上。WB CPU writer 的 begin/end access 在交出内容时维护实际写入区间，然后发出外设可消费的 fence；设备结束后按方向取得 CPU 可见性。这个接口不能反向扩展为所有 GPU 或 DPU 表面的每帧 clean，也不能用 IOSurface seed 或普通 lock/unlock 对来推断发生了 CPU store。

## 6. 每个软件维护候选对应的 Metal 参数或真实来源

本表同时给出可达入口和处理结论。现有源码证据没有给出一个必须在普通 Metal 数据提交中长期逐行维护的合法参数组合。不能把未知属性、混合页、外部 owner 或 scanout 身份直接加入正常慢速列表。

| 候选项 | 可供 review 的具体入口/来源 | 参数能否决定页属性 | 本设计的处理及软件维护结论 |
| --- | --- | --- | --- |
| 普通 no-copy 用户内存 | `newBufferWithBytesNoCopy:pointer length:length options:MTLResourceStorageModeShared deallocator:block`；frontend `NativeBridgeMetalFrontend.mm:1373-1382` 原样传入。已有具体来源为 `bridge/no_copy_import_host.mm:35` 的 page-aligned anonymous `mmap(PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON)`，`55-56` 用 Shared 导入。 | `pointer` 和实页属性决定；Shared 仅说明共享资源语义。 | 通常 WB，查实际页并持有后启用 A642L coherent。不存在凭 Shared 开启逐行维护的理由。 |
| no-copy 的 WC options | 同一 no-copy selector，`options` 可含 `MTLResourceCPUCacheModeWriteCombined`；前端把 options 传入 bridge，原内存已经存在。 | options 不会证明原页已是 NC，也不能覆盖其他 owner alias。 | WB 实页仍按 WB coherent；实际 NC 走屏障；是否接受与 owner 属性不符的 options 由 Metal API 参数校验单独决定。不能因该位就软件 clean。 |
| IOSurface texture | `newTextureWithDescriptor:d iosurface:surface plane:p`，frontend `1392-1407` 取 `IOSurfaceGetBaseAddress`、`IOSurfaceGetAllocSize` 和 plane offset。`surfaceBuffer:186-195` 内部固定 `MTLResourceStorageModeShared` 导入整个 allocation。 | descriptor、plane、format 均不能单独决定 WIMG；需要 backing 实页和所有 alias。 | WB coherent、Normal NC 屏障；多 plane 只是同 backing 的区间。普通显示完全覆盖这一处理。 |
| IOSurface 创建时显式 cache 参数 | 外部 owner 可用 `IOSurfaceCreate(properties)`；已有本地 `bridge/no_copy_import_host.mm:30-31` 只给 width=64、height=16、BPE=4、row=256、RGBA 格式，未给 `kIOSurfaceCacheMode`。当前保存日志没有 SpringBoard/backboardd 的创建字典。 | 必须取得创建请求的实际 cache mode，并以 pmap 结果为准。`MTLTextureDescriptor.cpuCacheMode` 无法补出这个值。 | owner 要求 NC 时保留真正 NC 并统一 alias；默认可缓存分配采用 WB coherent。不能写成“这个 IOSurface 参数必定等于 RT”。 |
| 本机 managed RT RAM | XNU 的 `VM_WIMG_RT`，且 `t733-managed-rt-normal=1`。它可由现有 IOSurface/图形内存 owner 的映射请求产生；那五个导入的具体关联尚未保存。 | Metal options 不编码 RT；pmap 当前配置将其解析为 Normal NC。 | 统一 aperture 和 user/kernel alias 后无逐行维护。它是要补齐的正常分类，不是软件维护例外。 |
| 固件 framebuffer 导入 | `GpuScanoutPresenter.h:35` 使用 `newBufferWithBytesNoCopy:scanout.base length:scanout.bytes options:MTLResourceStorageModeShared deallocator:nil`。外部来源为固件保留 DDR，现有显示 `NativeLegacyDisplay.cpp:1875-1885` 按配置请求 WC。 | 必须依据固件内存登记和所有现存 CPU alias；Shared 不提供这个证据。 | 以已登记 Normal NC/WC 访问并使用屏障。不能因其不在 `gPhysBase/gPhysSize` 就跳过属性管理或把其当 Device。 |
| Read-only file/constant no-copy | 同一 no-copy selector，pointer 来自外部只读映射。KMD `NativeAdrenoMemory.cpp:720-728` 在写 pin 被拒后重建 `kIODirectionOut` descriptor，GPU mapping 去掉写权限。 | pointer 的实际权限和页属性决定。 | WB coherent 的只读资源仍免软件维护。禁止写入属于权限校验。 |
| 同一页可缓存 alias 尚存 | 可从上面任一 no-copy/IOSurface 入口到达；常见实际 alias 是内核 physical aperture。 | 没有某个 MTL options 能描述全部 alias。 | 在 import/改属性事务中统一 alias；若必须转换属性，只有该次转换执行维护。不能以“外部 alias”批准每帧 clean。 |
| 来自已登记非coherent外设的缓存内存 | 仍可经 no-copy pointer 或 IOSurface 导入，但必须提供 exporter 的设备能力、CPU/device ownership、写入范围和 completion fence。当前正常 SpringBoard/backboardd 路径没有这类必要性证据。 | Metal 参数不足，必须是具体外部 attachment 契约。 | A642L/DPU 自己仍使用可用的一致性；软件维护发生在确实不支持一致性的那个外设交接，不作为本项目已成立的 Metal 数据例外。 |

内核自用内存单列：GPU ring、fence/status、GPU 和 DPU 页表、固件和 ZAP 输入都没有对应 Metal selector。普通 ring/fence/tables 采用分配器设计中已指定的一致性配置，删除对应逐行 helper。页表 CPU 写入到 table walker 的发布屏障、TLB 操作、真正属性转换时处理旧 cache lines，以及明确不一致的外设控制内存，按各自硬件协议处理。DPU 当前 `cleanNormalMemory(leaves,...)` 在 `NativeDpuSurface.cpp:588,605` 是页表维护，`cleanSurfacePages` 在 `579` 是表面数据维护，二者必须分别实现移除条件。

## 7. 现有 CPU cache helper 的删除和保留位置

| 现有调用位置 | 设计中的处理和调用时机 |
| --- | --- |
| `NativeAdrenoMemory.cpp:896,899,905` 发布数据的 clean | 从普通 BO/导入发布路径删除。WB 使用硬件一致性，真正外部 NC 使用发布屏障。 |
| `NativeAdrenoMemory.cpp:912,918` 完成后的 invalidate/CIVAC | 从普通资源完成路径删除；GPU 完成 fence 和读取屏障保留。 |
| `NativeAdrenoMemory.cpp:922-924` 的 `maintainImported` | 正常 import 不再调用，不保留 unknown/imported fallback。外部不支持的属性在导入阶段建立契约或拒绝。 |
| `NativeAdrenoVm.cpp:459` 映射时的 `publishForDevice` | WB 初次建 coherent 映射只发布屏障；已有 NC 迁移为 WB 的一次性处理由页属性事务完成，不复用普通提交 helper。 |
| `NativeAdrenoVm.cpp:941`、`NativeAdrenoCpu.cpp:277-293` 的 WC reassert | 删除热提交里的属性重查和重设；属性稳定性由 XNU 修改入口保障。 |
| `NativeDpuSurface.cpp:579` 的 `cleanSurfacePages`，及函数 `219-277` | 删除整条普通 present 调用和专用逐行表面 helper。没有因显示失败而每帧 clean 的备用路径。 |
| `NativeDpuSurface.cpp:442,588,605` 的 `cleanNormalMemory` | 按分配器设计启用正确的 coherent table-walk，并使用相容的 WB 页表分配后删除。发布顺序、页表变更和 TLB 同步保留。数据 leaf 改为 WB 本身不能替代 table-walk 配置。 |
| `NativeAdrenoMemory.cpp:784` 的 owned WB-to-WC 初始 CIVAC | 普通渲染分配全部 WB 后删除。只有确有外部 NC 要求的属性转换保留同等操作，迁入中央页属性事务。 |
| XNU `pmap.c:10529-10541` 的 `pmap_sync_wimg` 等属性转换处理 | 保留必要的通用 cache 属性转换操作：例如 cached 页确需变为 NC 时，先处理旧 dirty/stale cache lines，再执行相应页表/TLB 屏障顺序。其调用单位是一次属性转换，不是每个 GPU submit/present。新建 WB 页不触发。 |

`NativeAdrenoRing.cpp:212-216` 的 A642L `CACHE_FLUSH_TS` 是 GPU 命令，让 GPU 完成流水线写出并写 timestamp，必须保留。它与 CPU 执行的 `dc cvac`、`dc ivac`、`dc civac` 是两种不同的实际操作。移除后者不删除前者，也不删除 DPU 扫描结束的 fence。

## 8. 模块修改边界和所需结果

| 模块 | 具体需要完成的工作 |
| --- | --- |
| XNU 页管理/IOKit | 页属性持有记录、全部 alias 登记、aperture block 拆分、可返回错误的属性变更事务、属性变更完成后的代次更新。 |
| `NativeAdrenoMemory` | import 取得持有记录；保存实际属性区间；解除最后一个使用者后释放；保留原 owner 和只读权限。 |
| `NativeAdrenoVm` | 按属性区间建立 leaf，mapping 保存实际 coherent/NC 和代次；去掉“imported 即维护”的判断；复用现有 map/unmap 期间建立的物理重叠关系。 |
| Metal/Turnip 导入接口 | 传递 `NoCopy`、`IOSurface` 或 `FirmwareScanout` 的来源、Metal 创建形态和已有 surface 身份供诊断；可信属性始终取自内核。绑定真实读写区间和完成 fence。 |
| `NativeMobileDisplay` | 取得相同页记录，DPU WB leaf 使用已有 MAIR index 1，NC 使用 index 0；替换全表面 clean；按 backing/代次保留可复用 mapping；保留原 IOSurface 读 fence 和帧切换确认。 |
| 提交/完成回收 | 只遍历实际资源、属性区间及活跃别名关系；发布屏障、GPU cache 事件和完成 fence 保留；禁止按物理页重查属性。 |

记录来源和 WIMG 的目的，是让下一次 review 可把每个导入对应到实际创建参数和 owner，而不是让正常快路径等待一次新采样才能设计。记录在 allocation/import/属性变化时写入有界表，提交只累加按策略分类的字节、范围和操作数量。每帧逐 BO、逐页打印不进入设计。

已成立的参考程序是 KGSL/ION 的 cached coherent、NC skip、SDE 的 cached hint、GPU flush timestamp 和显示 input fence。需要本机新增的是 XNU 跨所有 CPU alias 的页属性生命周期、物理页到 GPU/DPU backing 的共同记录，以及 IOSurface 对应的精确生产者 fence。当前那五个导入的 WIMG 和创建字典仍缺少保存值，这个证据缺口不改变上述任何正常路径的选择。
