# A642L 自有 BO 分配、映射与回收设计

日期：2026-10-28。本文是设计，未修改驱动，未构建、测试或操作设备。

目标是让 A642L 的普通 Metal 自有资源以及驱动生成的命令、descriptor、shader 存储默认使用 CPU WB 和 GPU 硬件一致性。分配、CPU 映射、SMMU 映射、提交、完成和复用必须保存同一份实际内存属性。普通 SpringBoard/backboardd 提交无需 CPU 逐行 clean/invalidate，也不以采样覆盖率决定是否启用。

CPU 页属性和所有 CPU alias 的统一持有机制由导入/页面管理设计负责；Metal 到 Vulkan 的 memory type 选择由 [Metal 内存策略设计](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/docs/design/GPU_METAL_MEMORY_POLICY_2026-10-28.md)负责。这里定义分配器和 GPU 生命周期所需的接口及调用顺序。

## 1. 当前实现与成熟参考

| 部位 | 当前实际行为 | 需要改变的地方 |
| --- | --- | --- |
| owned 分配 ABI | [NativeAdrenoABI.h:123](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoABI.h:123) 的 `NativeAdrenoAbiBoAlloc` 只有 bytes、align_log2、contiguous | 增加协商后的新方法，传入真实 CPU 缓存要求，返回实际属性；保持 v1 record 原样 |
| 用户 BO 分配 | [NativeAdrenoUserClient.cpp:268](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoUserClient.cpp:268) 通过全局 WC 开关分配并调用 `makeWriteCombined()` | 依据本次分配的实际 memory type 创建 WB 或 NC/WC backing |
| owned 属性记录 | [NativeAdrenoMemory.h:135](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.h:135) 把 WC 保存在 backing；`cpu_cache_mode_` 在外层对象；[NativeAdrenoMemory.cpp:756](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:756) 只为 import 查询属性 | 实际模式和页属性持有引用属于 backing，随 backing 进入复用池，owned 也必须记录 |
| descriptor 创建与初始化 | [NativeAdrenoMemory.cpp:592](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:592) 仅用 `reuse_write_combined` 控制 descriptor cache flags；[同文件:661](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:661) 清零后统一 clean | WB descriptor、实际页属性和设备访问要求一起确定；WB coherent 清零后只执行交接屏障 |
| CPU 受控映射 | [NativeAdrenoUserClient.cpp:940](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoUserClient.cpp:940) 仅允许 WC descriptor；[NativeAdrenoPublish.cpp:861](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoPublish.cpp:861) 已从持久 descriptor 创建默认缓存属性的映射 | 让受控路径接受持有固定 WB 属性的 descriptor；WB BO 不发放 legacy 物理映射 token |
| GPU 映射 | [NativeAdrenoVm.cpp:222](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:222) 仅允许 imported WB coherent；[同文件:454](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:454) 固定 `Mapping::io_coherent` | owned 使用同一依据实际 CPU 属性、设备能力与映射属性作出的决定 |
| backing 复用 | [NativeAdrenoMemory.cpp:306](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:306) 匹配大小、输出地址位宽及连续性；[同文件:334](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:334) 仅接收 WC | 增加真实缓存属性和设备访问要求的匹配，WB 与 NC/WC 分组保存 |
| 命令提交与完成 | [NativeAdrenoVm.cpp:936](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:936)、[同文件:964](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:964) 已依据映射属性免 clean/invalidate | 扩展到 owned，并将相同交接屏障合并到提交/完成批次边界 |
| 内核 ring/fence | [NativeAdrenoService.cpp:230](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoService.cpp:230)、[同文件:240](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoService.cpp:240) 直接调用 Memory，绕过 `Mapping::io_coherent` | 改为依据该设备映射的实际访问属性交接，避免 owned WB 被直达调用重新 clean |

本机需要的硬件和 SMMU 表达能力已经存在：

1. Samsung 的 [adreno-gpulist.h:1718](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/adreno-gpulist.h:1718) 为 A642L 声明 `ADRENO_IOCOHERENT`；[adreno.c:1493](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/adreno.c:1493) 转成 MMU 能力。
2. [kgsl_sharedmem.c:689](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/kgsl_sharedmem.c:689) 依据设备能力和实际缓存模式决定 IO coherence；[kgsl.c:3612](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/kgsl.c:3612) 支持对 cached owned 分配默认启用。WC/uncached 不带 `IOCOHERENT`，原因记录在 sharedmem 第 698 行。
3. [kgsl.c:4229](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/kgsl.c:4229) 从 memdesc 的实际 cache mode 设置用户 CPU 映射；[kgsl_iommu.c:1687](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/kgsl_iommu.c:1687) 把 IO coherence 转成 `IOMMU_CACHE`。
4. [io-pgtable-arm.c:591](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/iommu/io-pgtable-arm.c:591) 选择 cached MAIR 项，[同文件:392](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/iommu/io-pgtable-arm.c:392) 设置 Outer Shareable；[同文件:1208](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/iommu/io-pgtable-arm.c:1208) 安装 WBRWA MAIR。本地 [ArmSmmu4K.c:75](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/kits/ArmSmmu4K/ArmSmmu4K.c:75) 和 [ArmSmmu4KContext.c:63](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/kits/ArmSmmu4K/ArmSmmu4KContext.c:63) 已能表达 Outer Shareable、AttrIndx 1 和 MAIR `0xff44`。
5. [kgsl_sharedmem.c:636](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/kgsl_sharedmem.c:636) 对已确定 coherent 的 memdesc 直接结束 CPU 缓存维护。

因此 cached owned 的默认策略直接采用 KGSL 的完整流程。XNU 页属性持有接口、版本化 IOKit 分配方法、WB backing 复用、内核 ring/fence 接口是本项目需要新增的设计。

## 2. 所有权、模式和数据类型

`NativeAdrenoBackingStorage` 继续拥有 prepared descriptor、CPU kernel mapping、physical spans。增加以下明确字段，避免 `write_combined`、descriptor cache flags 和 `cpu_cache_mode_` 分散保存且可能互相矛盾：

| 字段 | 保存的事实及持有者 |
| --- | --- |
| `requested_cpu_cache_mode` | 调用者最终选择的 CPU cache 要求，供请求匹配及错误报告使用；Metal hint 已在上层转换 |
| `cpu_cache_mode` | 页面管理组件确认的实际 WB 或 Normal NC/WC 模式，包含准确 WIMG；不能仅保存 `NonWriteBack` |
| `page_attributes`、`page_attribute_generation` | 对物理页属性记录的内核引用及其代际；记录覆盖 CPU 用户映射、kernel mapping、物理 aperture |
| `descriptor_cache_mode` | 持久 descriptor 的内存项属性，与所有受控 CPU 视图一致 |
| `device_access` | 本次分配将被 A642L GPU、DPU、SMMU table walker 或 GMU 中的哪些访问者使用；每个访问者分别记录自己的硬件能力与映射 |
| `prepared`、`quarantined`、CPU view/device reference 数量 | 分配器能否释放、改属性或进入复用池的条件 |

`NativeAdrenoVm::Mapping` 保存实际 CPU 属性代际、SMMU data 属性、访问者、GPU read/write/execute 权限及 `io_coherent`。`io_coherent` 是内核创建映射的结果。页表维护及 GMU 交接各自保存访问者的能力，不用一个全局 bool 统管所有设备。

同一个 backing 可有 GPU 和 DPU 两个设备 attachment，各自持有 IOVA、SMMU 属性、完成 fence 和正在使用的引用。两者引用同一份物理页属性记录。WB 页在两者支持的通路都使用各自的 coherent 映射，NC/WC 页在两者都只需交接屏障。GPU fence 完成只解除 GPU 使用；DPU 换出扫描表面并完成其使用后才解除显示引用。分配器的最后释放条件覆盖两个访问者，不能在 GPU 完成后立即归还仍在扫描的页。

DPU 依据是原厂 [display/yupik-sde.dtsi:149](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/arch/arm64/boot/dts/vendor/qcom/display/yupik-sde.dtsi:149) 的 cached coherent hint，以及 [msm_ion_dma_buf.c:162](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/staging/android/ion/heaps/msm_ion_dma_buf.c:162) 按 cached backing 与设备 hint 选择 `DMA_ATTR_FORCE_COHERENT`、按 uncached backing 选择 skip CPU sync 的流程。本机 [NativeDpuSurface.cpp:287](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/NativeMobileDisplay/NativeDpuSurface.cpp:287) 的 leaf 与实际 backing 属性接通由 [导入和显示设计](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/docs/design/GPU_IMPORTED_MEMORY_DISPLAY_2026-10-28.md)负责。DPU 自身页表 walker 的配置单独由该组件处理，不能沿用 GPU context 的 `coherent_walk=1` 作为 DPU 页表的证明。

`NativeAdrenoMemory::allocate` 将 `pooled`、`reuse_write_combined` 两个混合用途的 bool 改为分配要求对象，至少包括 bytes、输出地址位宽、连续性、实际 CPU cache 请求、访问者、是否允许 backing 复用。分配来源单独保留 UserOwned/KernelOwned；是否尝试池不再决定来源。

页面管理组件提供的接口应执行实际操作：

- 分配/导入时，对全部物理页取得长期属性引用，返回准确 WIMG、代际和允许的设备访问方式。
- 根据该引用创建 CPU 视图；请求改变属性时，先阻止新设备引用，等待已有 GPU/DPU 使用完成，再完成所有 CPU alias 变更及必要的一次性维护。
- 属性引用有效期间禁止未协调的 `MAP_MEM_ONLY` 改属性。提交只检查记录有效状态和代际，不查每页 pmap。
- backing 进入池时保留引用；只有 backing 真正归还 VM 时释放。持有 GPU/DPU 使用引用或未解除 CPU 视图的页不能改属性或复用。

当前 [NativeAdrenoCpu.cpp:205](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoCpu.cpp:205) 的一次属性查询只给出分类；[同文件:239](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoCpu.cpp:239) 还记录物理 aperture 保持 WB。仅改 descriptor 或查询 pmap 一次不能构成上述长期接口。

## 3. ABI 和防止伪造属性

新增 `NativeAdrenoMethodBoAllocV2` 与对应 Params 能力位。输入包含 version、bytes、align_log2、contiguous、最终 CPU cache 要求及必须为零的 reserved 字段。输出包含 handle、实际 CPU cache mode、实际 A642L IO coherence 结果和可用的受控 CPU 映射方式。具体编号在实施时统一分配，既有 [ABI.h:11](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoABI.h:11) 的 v1 record 长度和既有方法号保持不变。

上层必须将 `mem_property` 传到底层。[tu_knl_darwin.cc:1523](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_knl_darwin.cc:1523) 目前构造旧 `BoAlloc` 时丢失该信息。Turnip 只有协商到新分配能力、受控 CPU 映射能力并收到符合所选 memory type 的实际结果，才能采用 cached coherent owned 路径。`NativeAdrenoDeviceCachedCoherent` 由实现完成的 KMD 发布；旧方法保留 WC 行为，不能因为一个旧设备能力位而声称旧分配返回 WB。

用户态只能请求 CPU 使用方式，不能请求 `io_coherent=true`、pmap WIMG 或任意 SMMU 属性。KMD 使用平台 A642L 能力、页面管理记录以及实际 descriptor 属性选择 SMMU `writeback=1/shareability=2`。现有 [mapBo:349](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoUserClient.cpp:349) 已让内核覆盖映射属性，扩展时保留这一原则。旧 `BoMap.writeback` 最多作为要求；不符合 backing 时失败。

受控 `BoCpuMapV2` 继续不接收用户 cache bits，从同一持久 descriptor 创建映射。`cpuMap(..., controlled=true)` 的条件改为持久 descriptor 和有效页属性引用，而不是 `descriptorWriteCombined()`。WB BO 的 legacy token 请求失败，防止 [legacyMemoryForType:1093](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoPublish.cpp:1093) 的物理 descriptor 被调用者 cache bits 改写。

## 4. 分配到回收的具体顺序

1. **选择属性。** KMD 解析真实分配要求。A642L 普通 owned 请求默认 WB，GPU 使用 WB/WA、Outer Shareable、IO coherence。实际选中的 NC/WC memory type 使用 Normal NC/WC CPU 映射和 GPU Normal NC；它同样无需逐行维护。Metal 的 WriteCombined hint 在 unified memory 上仍优先选择 cached coherent，不能直接转成 KMD 强制 WC。
2. **取得 backing。** 在相同属性的可用池中查找；否则创建匹配属性的 descriptor，prepare，枚举并保存物理 spans，取得页面属性引用。CPU kernel mapping 和持久内存项都必须符合实际模式。WB 分配无需先变 WC 再变回 WB。
3. **初始化内容。** 新分配及跨 BO/跨客户端复用时完整清零。WB coherent 清零后使用发布屏障；NC/WC 在页面管理组件完成必要的初始 alias 转换后清零并屏障。一次性的属性转换维护属于分配阶段。不能把每次清零都无条件送入 `NativeAdrenoMaintainOwnedPages`。
4. **创建 GPU 映射。** 固定页面代际和实际缓存模式，建立 physical alias 关系、IOVA 索引及 PTE；按增量页表操作发布 PTE，并执行所需 TLB 同步。CPU/GPU 属性均有效以后才向用户返回 BO。保留当前 [Vm.cpp:465](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:465) 的增量更新和 [同文件:470](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:470) 的静态 alias 登记。
5. **CPU/GPU 交接。** CPU 完成命令、descriptor 或资源写入后，submit 验证 handle、范围、权限和映射代际，按 BO 去重持有引用；一次发布屏障覆盖本次 WB coherent/NC 资源，然后按既有顺序提交 GPU command stream。GPU 自身的 UCHE/CCU/cache flush、invalidate 及命令排序继续由 Turnip/PM4 负责。
6. **完成与 retire。** 在 CP fence 确认完成后执行 acquire 屏障，发布完成状态并放掉本 job 的 BO 引用。WB coherent 与真实 NC/WC 均不逐行 invalidate。CPU 可读数据、UMD 可回收 command buffer 的时点仍由 fence 决定。现有 [Submit.cpp:408](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoSubmit.cpp:408) 和 [同文件:920](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoSubmit.cpp:920) 的完成后释放引用顺序保留。
7. **复用完整 BO。** Turnip 自己缓存的完整 BO 只在最后使用 fence 完成后进入可复用链表，保持 handle、受控 CPU mapping、GPU IOVA 及实际属性。跨 queue 使用以最后一次设备使用为准；context reset 必须先完成设备停止和访问撤销，不能把 error fence 等同 DMA 已停止。
8. **回收 backing。** `BoFree` 先移除所有 CPU 视图，GPU job/DPU 使用引用归零，再删 GPU PTE 并完成 TLB 同步，然后释放 mapping 引用。此时 backing 可以进入相同属性的池；它继续持有 prepared descriptor、页属性引用和 spans。压力回收时解除必要的页属性持有、complete descriptor、释放页。任何 teardown 失败继续持有并 quarantine。

参考的实际生命周期规则：KGSL [kgsl.c:2259](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/kgsl.c:2259) 把释放挂到 timestamp 事件，回调在 [同文件:2249](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/kgsl.c:2249) 放下引用，fence 回调在 [同文件:2349](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/kgsl.c:2349) 延迟销毁。主线 DRM msm 的本地版本在 [msm_gem.c:711](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/drm/msm/msm_gem.c:711) 等待其它 fence context，在 [同文件:742](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/drm/msm/msm_gem.c:742) 将新 fence 绑定到 BO reservation。硬件一致性与这些使用期限独立保存。

## 5. 复用池和提交工作量

backing 池的匹配条件是 bytes、对齐、输出地址位宽、物理连续性、实际 CPU cache mode、descriptor cache mode、访问者及其硬件一致性条件。WB 与 NC/WC 不能通过改一个 bool 交叉复用。池中的实际属性和页面代际必须有效；属性变化将该条目移出可复用队列，由页面管理组件完成转换或回收后才重新插入。

现有 64 个 backing/64 MiB/单项 16 MiB 上限可以保留。内存压力继续先释放空闲 backing；不能回收等待 fence 的对象。当前 [Memory.cpp:529](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:529) 的逐页 WC reassert 改为使用长期属性引用的有效状态。完整 BO 在同一客户端同一用途池内复用时，不重新枚举物理页、不重建 CPU/GPU mapping。

早期连续池当前只有一个全局 `Pool`，[Memory.cpp:460](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:460) 会整体转 WC。实施时可直接让普通资源使用 WB scattered backing 缓存；若继续使用连续池，每个池必须有不可变的实际缓存模式与独立 free extent 列表，WB 池永不借用 WC 子区。物理连续分配只服务真正要求 contiguous 的调用，不新增普通 BO 的连续探测。

KGSL [kgsl_pool.c:103](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/kgsl_pool.c:103) 拒绝仍有其它引用的页面回池，[同文件:78](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/kgsl_pool.c:78) 在初始化页时清零和执行设备同步。本项目复用 prepared descriptor 而不是 Linux page，因此按真实 CPU 模式分池是 XNU 的新增设计，不能声称 KGSL 原样提供了此 descriptor 池。

每次提交只处理提交包中的 IB、实际 resource ranges、去重后的 BO 引用及已登记的重叠关系。普通资源不读取 pmap，不展开 physical spans，不扫描页面，不执行 clean/invalidate。保留 [Submit.cpp:753](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoSubmit.cpp:753) 的 BO 引用去重；将 [同文件:784](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoSubmit.cpp:784) 的逐资源发布拆成范围验证和批次末屏障，避免每个 coherent resource 都执行一次 DSB。完成批次采用同样顺序。

`resourceNeedsMaintenance` 不再通过 imported 与否作决定。只有另经批准且具有具体外部内存证据的映射才可进入软件维护集合；未知分类是分配/导入问题，不能使普通提交自动落入按页循环。即使上层使用 AllMapped 保持资源期限，WB coherent 也不会因此变成全量 clean。

## 6. Metal 参数与软件维护审查

以下表覆盖 owned 范围内能影响分配的实际入口。相关上层行号来自 Metal 设计代理的直接源码调查。没有一项已确认的 Metal 分配参数要求 CPU 逐行维护；因此本设计没有给普通 owned BO 增加手工维护 memory type。

| Metal 入口及实际参数 | 传到下层的字段 | 目标 KMD 属性 | 为何硬件一致性不能用 | 为何 NC/WC 也不能免维护 | 软件维护结论 |
| --- | --- | --- | --- | --- | --- |
| `newBufferWithLength:options:`，`StorageModeShared + CPUCacheModeDefaultCache`；[Frontend:1362](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm:1362)、[buffer.rs:1559](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/buffer.rs:1559) | `MemoryClass::Shared`，unified 优先 `HOST_CACHED`，[memory.rs:217](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/memory.rs:217)，最终 `mem_property` 进入 BoAllocV2 | CPU WB，GPU WB/WA + OS，coherent | A642L 可用，直接启用 | 该参数不构成 NC/WC 也不能免维护的原因 | 无 |
| 同入口，`Shared + CPUCacheModeWriteCombined`；[buffer.rs:1560](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/buffer.rs:1560) | `MemoryClass::WriteCombined`，unified 仍优先 `HOST_CACHED`；沿用上游选择 | 最终 cached type 使用 WB coherent；若实际选择非cached type则为真实 NC/WC | Metal hint 不禁止 cached coherent | 真实 NC/WC 同样只需屏障 | 无 |
| 同入口，`StorageModePrivate` 或 `StorageModeManaged`；[buffer.rs:1563](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/buffer.rs:1563) | Private/Managed 的 unified memory type 选择必须落实为实际 cached coherent；Managed 保留上层内容同步语义 | WB coherent；GPU-only 资源不要求用户 CPU map | 这两个 storage mode 不禁止 A642L 一致性 | 无强制软件维护理由；复制、内容同步仍由上层执行 | 无 |
| `newTextureWithDescriptor:` 的 storage/cpuCacheMode；[texture.rs:3312](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/texture.rs:3312)、[image_plan.rs:333](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/image_plan.rs:333) | 当前 texture 只把 storage 转 memory class；普通图像的 Private 选择及真实 memory type 需统一选 cached coherent | 内存中实际存在的 owned backing 使用 WB coherent；tile 内部存储不产生这类 BO | 当前参数没有禁止理由 | 实际 NC/WC backing 也不要求逐行维护 | 无 |
| 上述 Metal render/compute 编码生成 Turnip 命令、descriptor、shader 和 scratch；[tu_knl.h:238](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_knl.h:238)、[tu_cs.cc:222](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_cs.cc:222) | 当前 `tu_bo_init_new` 固定 `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`，未携带 `HOST_CACHED`；需以 A642L 支持添加 cached 并贯穿新 ABI | WB coherent；CPU 频繁写命令/descriptor 后批次屏障 | 现有 helper 的缺省 flags 不是硬件限制 | 真实 NC/WC 也可免逐行维护，但普通目标优先 WB coherent | 无 |

其它内部 owned 调用点包括 [tu_device.cc:3052](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_device.cc:3052) 的 global BO、[同文件:3468](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_device.cc:3468) 的 scratch、[tu_shader.cc:2884](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_shader.cc:2884) 的 private memory、[tu_pipeline.cc:1181](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_pipeline.cc:1181) 的 tessellation BO。[tu_device.h:717](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_device.h:717) 已有按能力添加 cached 的 helper；将实际普通 BO 入口接到同一策略，避免只改 query pool。

外部 `bytesNoCopy`、IOSurface、文件映射或其它 exporter 的软件维护例外由导入设计逐项给出。`Unknown`、`Mixed`、`NonWriteBack` 三个分类本身不能批准例外，也不能回答表中两个原因。具体外部 backing 若属性冲突，先按页面管理接口解决；必须保留原始 CPU 指针时也不能偷偷换成另一块复制存储。

## 7. 非 Metal 的内核/固件内存

| 内存、持有者和本地调用 | 目标模式与交接 | 参考、设计边界和回收 |
| --- | --- | --- |
| `NativeAdrenoGpuContext` 的 SQE、ring、memptrs/fence、pwrup、scratch；[GpuContext.cpp:74](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoGpuContext.cpp:74)、[同文件:131](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoGpuContext.cpp:131) | 接入同一 A642L WB coherent allocation/mapping；保留 privileged/read/write/execute 权限。ring 写入后屏障，fence 读取前 acquire 屏障；Service 通过设备映射记录交接 | [KGSL ringbuffer:260](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/adreno_ringbuffer.c:260) 通过通用 global allocator 分配，kernel vmap 在 [sharedmem:575](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/kgsl_sharedmem.c:575) 使用 WC。将本机 ring 改 WB coherent 是本机的新设计。它们由 context 持有，GPU 停止且映射解除后才能回收 |
| GPU/GMU 的 SMMU tables；[GpuContext.cpp:67](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoGpuContext.cpp:67)、[SmmuMemory.cpp:10](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoSmmuMemory.cpp:10) | 保持 CPU WB。按 provider 的 coherent table walk 能力发布 PTE；当前 [SmmuContext.cpp:25](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoSmmuContext.cpp:25) 已配置 `coherent_walk=1`，需要把此事实传给 table publication，使其执行顺序屏障而不再逐页 clean | Linux [io-pgtable-arm.c:304](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/iommu/io-pgtable-arm.c:304) 对 coherent walker 省去 DMA sync，[同文件:442](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/iommu/io-pgtable-arm.c:442) 保留发布顺序。页表不作为 GPU 数据 BO 映射；context disable、TLB 同步结束后释放 |
| GMU icache、dummy、debug、log、HFI；[GmuMemory.cpp:34](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoGmuMemory.cpp:34)、[同文件:66](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoGmuMemory.cpp:66) | 使用准确 Normal NC/WC CPU 属性及 GMU Normal NC mapping，交接只用对应屏障。GMU 不接受 A642L GPU 数据 coherence 作为自己的授权 | Samsung [adreno_a6xx_gmu.c:1411](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/adreno_a6xx_gmu.c:1411) 使用 DMA allocation；[同文件:1424](/Users/rigoligo/expm/unixnu-sm-t733/references/android_kernel_samsung_sm7325/drivers/gpu/msm/adreno_a6xx_gmu.c:1424) 没有传 `IOMMU_CACHE`。由 GMU allocator 持有，GMU 停止并解除映射后释放 |
| ZAP 保留 DDR；[Service.cpp:332](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoService.cpp:332) | 保持平台固件加载所需 Normal NC/WC，复制/清零用普通 RAM 指令，加载完成按固件协议交接 | 生命周期由 Service 和安全固件协议管理，不进入用户 BO 池 |

硬件能力登记需要在相应映射建立前完成。当前 [Service.cpp:812](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoService.cpp:812) 才启用 cached imports；新实现应在 GPU SMMU context 已建立、板级 A642L 属性已匹配时为该访问者登记能力，使随后建立的内核 BO 和用户 BO 使用一致规则。用户态能力仍在完整服务可用时发布。能力登记不复用要求空页表的审计接口。

## 8. CPU 逐行缓存维护调用的删除与保留

普通 owned 提交、retire、ring、fence 和当前 coherent walker 页表路径直接删除 CPU 逐行维护调用。它们分别留下权限/期限检查以及发布或获取屏障。下表中的保留操作仅存在于实际改变缓存属性的事务中，不作为 submit 的可选分支。

| 当前调用 | 设计决定 | 必要性及执行时点 |
| --- | --- | --- |
| [Memory.cpp:579](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:579)、[同文件:671](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:671)，owned 清零后 `NativeAdrenoMaintainOwnedPages(..., Clean)` | 删除这两个统一 clean 调用 | WB backing 的 GPU/DPU 使用各自 coherent 通路；NC/WC 的页属性事务在清零前完成，清零后只需发布屏障 |
| [Memory.cpp:905](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:905)、[同文件:918](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:918)，owned 发布/获取分别 clean/invalidate | 从普通 owned 交接接口删除 | owned 分配只返回实际 WB coherent 或实际 NC/WC；调用者通过设备 attachment 交接，不能因 CPU 为 WB 就执行软件维护 |
| [Vm.cpp:459](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:459)、[同文件:503](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:503)、[同文件:948](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:948)、[同文件:971](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:971)，map/unmap/submit/retire 调用 Memory 维护 | 普通 owned 不再连接这条维护调用链 | 每个 Mapping 的实际模式在创建时完整确定；提交与完成按批次屏障，map/unmap 保留页表和 TLB 操作 |
| [Vm.cpp:581](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:581)，legacy `publishMapped()` 的全量发布 | 删除全量 owned cache sweep | legacy WC 也通过稳定实际页属性免逐行维护；AllMapped 只能扩大对象期限登记，不能扩大缓存清理范围 |
| [Memory.cpp:534](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:534)、[Vm.cpp:941](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:941)，复用/提交 reassert WC | 删除复用和提交中的逐页修复 | 页面属性长期持有接口阻止未协调变更；发现记录失效时停止该对象使用并处理属性事务，不能在提交时边查页边改页 |
| [Service.cpp:230](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoService.cpp:230)、[同文件:240](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoService.cpp:240)，ring 发布和 fence 获取 | 改为对应 GPU attachment 的屏障接口 | ring 与 memptrs 是 A642L coherent BO；fence 完成读取仍按硬件顺序进行，无逐行 clean/invalidate |
| [Service.cpp:683](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoService.cpp:683)、[同文件:781](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoService.cpp:781)，SQE 发布和 scratch 获取 | 改为对应 GPU attachment 的屏障接口 | 这两个已有启动操作使用同一已建立的 coherent mapping，不需另设软件维护 |
| [SmmuMemory.cpp:10](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoSmmuMemory.cpp:10)、[同文件:38](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoSmmuMemory.cpp:38)，table page 发布和初次 table 初始化发布 | 当前 coherent walker 删除逐行 clean，保留 PTE 发布屏障 | 根据 provider 的 table-walk 能力传递到 publication；不以 GPU 数据 coherence 推断。当前配置已有 coherent walk，Linux 参考也只在不一致 walker 上做 DMA sync |
| [Memory.cpp:464](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:464)、[同文件:784](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:784)，连续池和 backing 从 WB 改 WC 之前的 clean/invalidate | 从普通分配器调用链移走；合并到页面管理组件的真实属性转换事务 | 仅当既有 WB 页实际转 NC/WC 时处理旧 dirty/stale cache lines。全部 CPU alias、GPU/DPU refs 的停止和页属性更新由同一事务负责，只执行所需一次维护 |
| [Memory.cpp:233](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:233)、[同文件:842](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:842)，归还 VM 时恢复默认属性 | 移到页面管理组件的释放事务 | 页真正回 VM 且全部 CPU/GPU/DPU refs 消失时，按实际旧/新属性转换；WB 同模式释放无需额外逐行维护 |
| [GmuMemory.cpp:39](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoGmuMemory.cpp:39)、[A6xxZap.cpp:106](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoA6xxZap.cpp:106)，GMU 与 PAS metadata 改 WC | 分配器直接请求实际 NC/WC，必要初始属性维护由页面管理组件执行 | 固件使用期只有屏障，不加入逐行维护。PAS metadata 在 [A6xxZap.cpp:113](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoA6xxZap.cpp:113) 的发布明确变为 NC/WC store barrier，然后按第 120 行调用 SCM |

CPU cache 指令的底层实现可以保留给实际页属性转换使用；普通 GPU 分配器、提交器和显示交接不再调用其整 BO clean/invalidate 入口。本范围未找到需要在每次提交保留的软件维护操作。

## 9. 失败处理与实施边界

- 普通 WB coherent 分配要求不满足时返回明确错误及实际失败阶段。不能在同一个 BO 中静默退成 WB noncoherent 并开始每提交 clean。上层只有在 memory type 合同允许时才能重新选择真实 NC/WC 并重新分配。
- 某个 CPU alias 与要求冲突时，由页面管理组件在无设备使用期间完成属性统一；无法统一则分配/映射失败。不能以维护整个 BO 掩盖相互冲突的 cache attributes。
- 旧 ABI 是独立兼容路径，保留其实际 WC 返回；未升级的调用者不能得到新 cached memory type。新 KMD 与新 Turnip 的能力和返回属性必须一起接通。
- 未提交前失败按相反顺序撤销引用和映射；GPU 可能已经收到命令、PTE 同步失败、CPU unmap 失败或 descriptor complete 失败时保留相关 backing 并 quarantine。既有 [freeBo:396](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoUserClient.cpp:396)、[Vm::unmap:494](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoVm.cpp:494)、[Memory::release:814](/Users/rigoligo/expm/unixnu-sm-t733/sources/UniXNU-gts7fewifi/drivers/Adreno6xx/NativeAdrenoMemory.cpp:814) 的引用保护继续适用。
- 内存压力只处理空闲且无 CPU/GPU/DPU 使用的 backing。属性不匹配不在提交期间转换，尺寸不匹配不引入每帧物理页搜索。

需要成组实施的范围为：KMD 分配要求与 backing 属性、页面属性长期持有接口、新分配 ABI及受控CPU映射、GPU mapping 属性、Turnip owned memory type 与内部 BO helper、backing/完整 BO 复用匹配、ring/fence/table 的实际访问者交接。GPU ISA、调度策略、shader 编译及两平面 IOSurface 不属于此分配器设计。

本设计与 Metal 代理已对齐：WriteCombined hint 保留 unified cached 优先策略，Private/Memoryless 必须改实际 memory type 选择，普通 owned 路径无软件逐行维护例外。与页面管理代理已对齐：backing 持有实际页属性和代际，GPU/DPU 使用期间不能变属性，提交不逐页重查。公共接口的最终名称和新 ABI 方法编号由整合时统一确定，以上所有权和调用顺序不依赖这些命名。
