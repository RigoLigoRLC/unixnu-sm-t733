# 54 提交资源、CPU 映射和缓存一致性设计

2026-10-27。本文只给出源码依据和实施设计，没有修改实现、构建、部署或增加测试。

54 最近 256 次提交的 CPU 准备平均 7.260 ms，其中全 WC 页属性检查
3.064 ms、全 VM 内存发布 4.063 ms。目标是让这些工作分别随实际 CPU
映射变化、实际资源使用和 CPU/GPU 所有权交接发生。现有测量与限制见
[54 实测](../../../notes/GPU_BO_MAP_COST_AUDIT_2026-10-27.md#54-runtime-and-remaining-stutters)。

## 1. 源码版本和已经确认的边界

设备驱动根为 `sources/UniXNU-gts7fewifi/drivers/Adreno6xx/`，本次读取提交
`548b68101a0029d3b5cf5fc468e60fb2e48ec2a2`。下面设备文件行号均相对该根。
实际用户态 Mesa 根为 `sources/UniXNU/.cache/mesa-work/`，提交
`85fda3b36405bcaea8bffc64aa4f2651dc90770d`；Metal runtime 根为
`sources/UniXNU/.cache/gpu-reference/metal2vulkan/`，提交
`720ef1ce9ae2a2068999a8675717418b9416283e`。这些 checkout 的本机补丁也是
当前输入，实施与交付必须另按项目规则提交并记录。本设计没有采用 out 树中的
Mesa 作为 54 用户态来源。

XNU 阅读对象是本次构建使用的 `xnu-10063.141.1` 与本地补丁，路径为
`out/t733-kernel/gts7fewifi/ipados-17.7.11-21H461/gts7fewifi/tree/.cache/xnu-work/`。
以下 XNU 行号指这个组装树；原始源码链接固定 Apple tag。

| 位置 | 当前行为及设计要求 |
| --- | --- |
| `NativeAdrenoABI.h:75-86` | submit 只有最多 16 条 `{handle,iova,bytes}` IB。它没有纹理、render target、descriptor、shader、scratch 等资源列表。 |
| `NativeAdrenoSubmit.cpp:333-366` | 当前仅给 IB 对应的 VM mapping 加 job 引用。新增完整资源引用是必须完成的寿命修正。 |
| `NativeAdrenoSubmit.cpp:378-408` | 每次 submit 先遍历全 VM 重查 WC，然后发布全 VM。 |
| `NativeAdrenoVm.cpp:421-439,466-474` | 发布、GPU 写回取得及 WC 检查都遍历全部 mapping，VM 本身由多个连接共用。 |
| `NativeAdrenoMemory.cpp:398-404,550-597` | owned BO 以默认缓存属性创建 IOBufferMemoryDescriptor，之后另用 pmap 设置 WC。 |
| `NativeAdrenoPublish.cpp:485-521` | CPU 用户 view 是另外创建的 physical descriptor。传出的 map options 还会被 XNU 的用户 map flags 覆盖。 |
| `NativeAdrenoUserClient.cpp:638-664` | BoCpuMap/BoCpuUnmap 修改 token lease。token 申请发生在实际 IOMemoryMap 创建前。 |
| Mesa `src/freedreno/vulkan/tu_knl_darwin.cc:848-880,1064-1127` | submit 仅收集 IB，然后每 16 IB 调一次内核提交。每块都重复全 VM 维护并占用一个 job。 |
| Mesa `tu_device.cc:3969-4005`, `tu_knl.cc:192-205` | Vulkan flush/invalidate 已有范围入口；当前通用实现直接维护用户地址，尚未把所有权变化通知 Darwin 内核。 |

## 2. 成熟上游可以直接借用的部分

下载参考在 `logs/gpu-bo-research-20261027/raw/`。这些 Linux/Mesa 文件是
2026-10-27 获取的官方 master/main 快照，原采集没有逐文件记录固定 commit。
表内据实标明分支与采集日期；正式移植时应固定对应源仓库提交，不能给这些旧副本
事后配一个未经对应的当前 HEAD。

| 上游和本地函数位置 | 可以移植的具体步骤 | 本平台仍需完成的部分 |
| --- | --- | --- |
| Linux master，`linux-msm_msm_gem_submit.c:127-188` 的 `submit_lookup_objects`；294-345 的对象锁与依赖；404-430 的 `submit_attach_object_fences`；518-542 的失败/退休释放 | 命令数组与资源数组分开；handle 表短锁内取得每个 BO 引用；按读写用途取得依赖；完成 fence 登记在所有实际资源；失败只释放已取得引用 | IOKit wire ABI、NativeAdreno BO/VM 引用、XNU 锁和 A642L ring 接口 |
| Mesa main，`mesa-tu_knl_drm_msm.cc:486-520,1218-1262` | 资源数组与 IB 数组分开，IB 引用资源索引，GPU 工作结束前保存 BO | 此版本 `submit_bo_list` 包含设备所有 BO，不能直接称为精确资源集合；Darwin 需扩展命令资源记录 |
| Linux master，`linux-panfrost_panfrost_gem.c:121-169,314-364` | VM mapping 同时保留 BO 与 MMU context；dma-buf begin CPU access 取得 CPU 可见性，end CPU access 向设备发布；对附件逐一同步 | IOSurface/host import 的 exporter、CPU 写入通知和全局共享 backing 关系 |
| Linux master，`linux-panfrost_panfrost_gem.c:450-480` | CPU cache mode 在 BO 建立时按设备能力决定；缓存属性取决于全部 CPU 别名 | 保留 A642L 非一致性配置，使用 XNU Normal WC，不把 coherent 标志作为性能开关 |
| Mesa main，`mesa-panfrost_vulkan_panvk_device_memory.c:385-425,469-488` | flush/invalidate 按 memory offset/size 收集，可聚合；import 的 host-visible/coherent 能力由 backing 实际能力决定 | Darwin cache-range methods、IOSurface fence 与 CPU ownership 接入 |
| SM7325 原厂 `references/android_kernel_samsung_sm7325/drivers/gpu/msm/kgsl.c:3268-3307`，`kgsl_sharedmem.c:607-671,689-705`，提交 `36b842cf1fc87a989dcc8158a142d7575cee41c1` | 缓存操作分别表示 clean、invalidate、两者；核对 offset/length 和 cached 属性后维护页；只有硬件能力允许才接受 I/O coherence | 用本机物理 span 和已有 ARM64 cache helper 执行范围维护，保留本机 SMMU 属性 |
| Linux master，`linux-msm_msm_gem.c:133-145,929-956` | WC 新页准备与 CPU/GPU 依赖分开处理 | `msm_gem_cpu_prep/fini` 的 cache maintenance 在该文件仍是 TODO，不能把它们作为本机完整一致性实现 |

原始来源：[MSM submit](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/msm_gem_submit.c)、
[Mesa MSM](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/freedreno/vulkan/tu_knl_drm_msm.cc)、
[Panfrost GEM](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/panfrost/panfrost_gem.c)、
[PanVK memory](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/panfrost/vulkan/panvk_device_memory.c)、
[SM7325 KGSL](https://github.com/Bush-cat/android_kernel_samsung_sm7325/blob/36b842cf1fc87a989dcc8158a142d7575cee41c1/drivers/gpu/msm/kgsl_sharedmem.c)、
[MSM GEM](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/msm_gem.c)。

Linux dma-buf 的 exporter 拥有 backing，importer 持有 attachment；CPU access
与 DMA fence 是两项分别执行的工作。把 buffer 送入 GPU 的引用列表，不能替代 CPU
cache clean；cache clean 也不能替代等待前一个 GPU writer。该分工直接用于下面
对象关系。[dma-buf 文档](https://docs.kernel.org/driver-api/dma-buf.html)

## 3. 对象所有权和并发顺序

下列名称是待实现对象的具体职责，不要求照搬 Linux 类型。

1. `NativeAdrenoBackingStorage` 拥有 IOBufferMemoryDescriptor 或 retained import
   descriptor、已 prepare 的物理页、CPU cache mode 和 CPU/GPU 可见性状态。
   同一 IOSurface 的重复导入共享一个 backing 记录。没有 exporter 身份时，通过
   已 prepare 的物理页区间登记重叠关系；维护范围按 backing offset 合并。发生物理
   重叠但不能建立共同所有权时使用完整共享范围维护，并保存所有引用。
2. `NativeAdrenoCpuMapping` 拥有实际 IOMemoryMap、task 引用、map_id、backing
   引用、offset/length、只读/可写属性和实际完成状态。CPU 映射存活与 CPU 正在读写
   是两个字段；永久映射并不表示 CPU 永久拥有缓存内容。
3. VM mapping 拥有 backing 引用、GPU VA、offset/length、权限和稳定 generation。
   解除 GPU mapping 要等该 mapping 的全部 job 引用结束。
4. `NativeAdrenoSubmission` 拥有复制后的 IB、全部资源条目、去重 BO/VM 引用、
   每资源读写范围、依赖 fence 和 `{reset_epoch,queue,sequence}` 完成标识。
   接受后保留到 GPU 完成，或硬件停止且确认不再访问这些页。
5. 每个 backing 保存 CPU 写入代数、已发布代数、GPU 写入代数，以及各 queue 的
   最后读/写 fence。range 记录固定上限，溢出时合并为该 BO 的完整范围。64-bit
   代数溢出触发停用该对象并重新建立，不把回绕后的值当成旧状态。
6. 已完成对象进入 BO 管理方案的 ready 队列。完成观察路径只更新状态、放 job
   引用和移动已分配节点；CPU unmap、SMMU unmap、complete 和 descriptor release
   由回收线程执行。

handle 表锁只做查找、插入、移除和 retain；放锁后才做缓存维护、分配或等待。
多 BO 事务按稳定 backing ID 排序后取得 backing 锁，再取得 VM metadata 锁，最后
取得 ring 提交锁；GPU completion 不倒序取得这些锁。CPU map/unmap 先持连接生命周期
gate 和 backing 的 map 操作引用，释放 handle 表锁后调用 IOKit。IOKit 的内部锁持有期间
不回调 GPU 全局锁。等待 fence 前释放全部 backing/VM/ring 锁，醒来后按相同顺序重取，
重新比较 generation。回收线程从 ready 队列取引用后在锁外完成资源销毁。

第一阶段可以继续用现有串行 submit 锁实现相同状态机，并让 map 操作与 submit 通过
backing 的状态协调；拆锁时沿用上述顺序。不能在一处去掉全局锁，却让 handle、VM
mapping 和 backing 继续依赖原锁的隐含寿命。

当前 Darwin 后端的 `dw` 连接由进程内多个 Vulkan device 共用，device 关闭与连接
关闭的寿命边界还需要统一持有计数。BO 诊断因此使用独立 device_id 区分报告来源；
本次诊断改动保持现有 GPU 对象寿命，连接与 device 的关闭关系在后续对象管理实施
中一起完成，避免只改变报告对象却改变仍在使用的 GPU 连接。

## 4. WC 属性跟随 backing 和实际 CPU 映射

XNU 的 `IOBufferMemoryDescriptor.cpp:248-255,284-285,383-418` 已接受
`kIOMapWriteCombineCache`：分配按页对齐，缓存模式写入 memory entry，并建立持久
descriptor。`IOMemoryDescriptor.cpp:364-369` 将 WriteCombine 对应到
`VM_WIMG_WCOMB/MAP_MEM_WCOMB`；InhibitCache 对应 `VM_WIMG_IO/MAP_MEM_IO`。
owned BO 的 RAM 应从创建时选择 WriteCombine。原始来源固定于
[IOBufferMemoryDescriptor](https://github.com/apple-oss-distributions/xnu/blob/xnu-10063.141.1/iokit/Kernel/IOBufferMemoryDescriptor.cpp)
和 [IOMemoryDescriptor](https://github.com/apple-oss-distributions/xnu/blob/xnu-10063.141.1/iokit/Kernel/IOMemoryDescriptor.cpp)。

具体 procedure：

1. owned BO 创建原始 IOBufferMemoryDescriptor 时就传 WC，取得 prepared 页和 kernel
   view。该 descriptor 及其 memory entry 一起存入 backing cache，复用不重建别名。
   初始化清零和由 cached 转 WC 的必要 clean/invalidate 在新 backing 阶段完成。
2. V2 CPU map 通过新的外部方法直接调用该原始 descriptor 的 createMappingInTask。
   驱动固定 WC、task 身份和范围，拒绝用户另选缓存模式。成功创建 IOMemoryMap 后，
   对该 backing 完成所需的属性处理和 barrier，再登记 map_id，最后返回 VA/length。
   一旦创建失败，撤销本次 map 引用；如果 map 已创建而后续处理失败，先 unmap，
   teardown 失败则保留 backing 并记录故障。
3. V2 CPU unmap 用 map_id 找到实际 IOMemoryMap，标记正在解除，调用 unmap，成功后
   才清掉记录并释放 map/backing/task 引用。多个 CPU mappings 各自计数；结束一个
   不能解除另一个。连接关闭遍历同一记录执行相同 procedure。
4. 实际 map/unmap 完成只使这个 backing 的 CPU-map generation 变化。如果本机 pmap
   的适配仍要求显式属性设置，把一次性的页处理放到这一完成位置，对此次 BO 执行，
   提交只比较该 BO 的已完成 generation。不能在 token 申请时提前标为完成。
5. V2 BO 不向 `clientMemoryForType` 暴露通用 map token，以免调用者改变 map flags。
   placed mapping 同样由新方法使用指定 VA 建立，保留/释放 VA 的行为明确区分。
   raw `munmap` 使用户地址消失时，驱动仍保存 map 对象和 backing，直到显式 unmap 或
   close 清理；这只延长寿命，不允许提早回收。Turnip 的普通和 placed unmap 都改走新方法。
6. 导入已存在的 IOSurface/host pages 保留原 cache mode。不得把别人的 cached 页面
   改为 WC。owned 页被再次 import 时查 owner 记录，复用已存在 backing 或拒绝冲突
   cache mode，不能创建第二个会修改其模式的 physical descriptor。
7. backing 真正销毁时先完成全部 CPU map teardown、GPU jobs 和 GPU unmap；然后按
   descriptor 拥有的生命周期恢复/释放页。旧手工 WC 路径必须与新 descriptor-owned
   路径分开记录，避免两次恢复属性。

XNU `IOUserClient.cpp:2043-2050` 在 clientMemoryForType 返回后才创建 mapping，
且覆盖 user-option bits。`is_io_connect_map_memory_into_task:4715-4749` 把 mapping
加入 client 集合后才离开 IPC gate；unmap 入口在 `4828-4857` 重新找 descriptor。
当前头 `IOUserClient.h:307` 的 mapClientMemory64 不是 virtual，不能用一个假想
override 截获完成事件。新的 V2 显式映射方法在驱动内掌握完成点，沿用 IOMemoryMap
本身完成真正映射。[IOUserClient 源码](https://github.com/apple-oss-distributions/xnu/blob/xnu-10063.141.1/iokit/Kernel/IOUserClient.cpp)

成功迁移后的 submit 不调用 `NativeAdrenoReassertWriteCombined` 扫描所有驻留页。
页属性工作累计量应随 backing 创建和真实 CPU mapping 操作增加。若尚有老 token
映射或未知 cache-mode 修改者，相关 BO 仍走旧属性处理路径，直到这个入口也迁移完成。

## 5. 资源列表和范围如何取得

资源收集必须放在实际建立 GPU 地址引用的位置。Turnip 的命令记录增加每 command
buffer 的 BO 引用集合：IB backing、descriptor backing、shader binary、scratch、
VSC/LRZ、query、上传区、纹理和 render target backing、vertex/index/indirect buffer
全部纳入。命令 buffer reset/释放只能放用户态记录引用；已接受 submission 有自己的引用。
secondary command buffer 执行时合并其集合。descriptor update-after-bind 在提交取得
更新后有效的 descriptor 和其资源引用，不能只使用录制时的旧集合。

Metal runtime 已有可用起点：
`crates/metal2vulkan-runtime/src/used_resources.rs:47-58,83-113,154-165` 记录
buffer、texture 根对象和 heap；但它明确排除内部 scratch/uploads，因此不能单独作为
内核完整列表。`command_buffer/submit.rs:335-343,528-553` 合并首次 image 工作和
实际 VkQueueSubmit，是把 runtime 资源使用信息交给下一层的边界。
[该 runtime 固定版本](https://github.com/steelbrain/metal2vulkan/blob/720ef1ce9ae2a2068999a8675717418b9416283e/crates/metal2vulkan-runtime/src/used_resources.rs)

范围按实际 backing 表达：buffer copy 和已知 binding 用 offset/length；未知 shader
索引覆盖整个已绑定 buffer；纹理的 tiling/UBWC、mip、plane 只有地址布局算法能给出
完整字节范围时才缩小，先以整 BO 维护；heap alias 合并到 heap backing；未知读写
用途按 read+write。CPU cache 范围首先按 4 KiB 合并，沿用当前物理维护 helper 的合法
范围要求；以后按 cache-line 缩小需要同时处理同一 cache line 的 CPU/GPU 交接。

bindless、buffer device address、GPU 生成地址、descriptor buffer 和间接命令有两类：

- 有受保留资源集合约束的调用，合并该集合，并让整个 descriptor/地址表所属 BO
  在提交期间存活。
- 无法枚举地址最终指向的 BO，生成该连接所有存活且 GPU 可寻址 BO 的快照，按
  read+write、完整范围保留。snapshot 在提交排序前建立，后续新建 BO 不会被已经
  固定的命令引用；允许提交后修改 descriptor 的用法必须把全部可能目标纳入快照，
  或让对应接口延后更新到该提交完成。快照包括驱动内部 GPU BO，并单独统计原因。

这个回退仍只针对相关连接，不能让一个连接触发另一个连接全 VM 的 CPU cache
维护。数据不完整时，已知的是完整 backing 范围和连接拥有的可寻址 BO，可以据此
保留正确性并明确记录额外成本。

## 6. 导入内存的 CPU 写入通知和可见性

没有 CPU 写入通知，就不能把 import 的“上次已发布”视为继续有效。
`BoCpuMap` 只能通知映射存在，持续映射后的普通 CPU store 不产生 ioctl。

| backing 类型 | CPU 写入通知来源 | 提交前和 CPU 访问前的动作 |
| --- | --- | --- |
| 驱动 owned WC | 同线程提交调用加 Vulkan/Metal 规定的线程同步；真正的 map generation | 提交前 store barrier；GPU 写入完成且准备 CPU 观察时执行对应 acquire barrier |
| 驱动 owned cached 非一致性内存 | `vkFlushMappedMemoryRanges` 的 offset/length；CPU begin/end 方法 | CPU 写结束更新代数并 clean 对应范围；GPU write 前需要对冲突 CPU cache line 先 clean/invalidate，CPU 读取前等 writer 并 invalidate |
| Metal managed | `did_modify_range` 中实际 managed-buffer 上传/flush；同步 encoder 返回 CPU 数据的边界 | 按 runtime 真实存储类型执行，不把 Unified Memory 等同于 A642L 硬件一致性 |
| IOSurface exporter 接入完整 | exporter 的写 lock/unlock 或明确生产结束事件，与同一 surface 的 GPU producer/consumer fence | write-end 更新共享 backing 代数/范围，device acquire 等 producer，再发布 CPU 脏范围；CPU begin 等 GPU writer 并取得 CPU 可见性 |
| no-copy、外部 host import、没有完整 exporter 通知的 IOSurface | 每次逻辑提交取得对该 BO 的使用权 | 每个实际使用的 import 完整范围 clean；可能 GPU 写时提交前 clean/invalidate，GPU 完成后 invalidate 再暴露 CPU 完成 |

IOSurface 的内容 seed 变化可以辅助统计，但不足以证明所有 CPU 写入已通知，不能
仅用 seed 相等跳过维护。接入 exporter 必须覆盖所有映射该 surface 的进程及驱动；
只在 AdrenoMetal 自己调用 IOSurfaceUnlock 的位置通知，不覆盖其他 producer。
当前 ABI 的任意 host-address import 因此属于最后一行，直到调用者明确采用新的
begin/end CPU access 合约。

CPU begin read 等最后 GPU writer；CPU begin write 等全部 GPU readers/writers。
end write 提交范围，CPU access 结束后才能允许冲突 GPU job。host coherent 的 API
能力可由 WC 映射和 barrier 实现，cached import 则必须有相应维护；不能把已有
`native_adreno_io_coherent` 改成 1 来绕过这些步骤。

缓存维护只在依赖满足后进行。前一个 GPU writer 未结束时，对同一 import 先 clean
CPU 旧行可能覆盖 GPU 新数据。正确顺序是等待冲突所有者，完成旧所有者的 release，
再执行新所有者的 acquire。共享 cache line 内的访问也按这一顺序执行。

GPU 写完后以 invalidate 取得已归设备所有的范围；前面的 clean/invalidate 已处理
CPU 脏数据。若允许 CPU 同时写同一 cache line 的另一部分，cache 操作本身不能修复
这个冲突，调用者必须扩大交接范围或使用独立存储。已有 GPU command stream 的
cache flush、memory barrier 和 fence 顺序完整保留。

## 7. ABI 兼容和一次逻辑提交

保持 V1 `Query` 的版本值、selector 1-16 和全部结构大小。新增 V2 capability/query
与新 selector，并用独立 V2 header 声明 version、record_size、flags、reset_epoch。
新用户态先查能力；旧内核返回 unsupported 时按 V1 兼容路径工作。V2 BO 创建/CPU
映射模式按连接选择，不能让同一 BO 混用新 mapping ID 和旧 token。

V2 需要以下操作及固定含义：

- `CpuMapCreate/CpuMapDestroy`：handle、offset、length、requested VA、map flags；
  返回实际 VA/length/map_id。cache mode 由 BO 固定。失败不输出可用 map_id。
- `CpuAccessBegin/CpuAccessEnd`：handle、offset、length、read/write、access_id；
  Begin 在依赖满足后返回，End 才使写入代数生效。非一致性 Vulkan flush/invalidate
  调用映射到这些对象的范围维护方法，不冒充已结束一个仍在进行的 CPU access。
- `SubmissionCreate` 预留一个带 generation 的 staging ID；`SubmissionAppend`
  用有界固定大小数组逐段复制 IB、资源和 wait fences；`SubmissionCommit` 封存
  所有数组并原子接受，返回逻辑 fence；`SubmissionCancel` 只取消未接受对象。
  每个资源条目包含 handle、mapping generation、offset、length、read/write。
- `WaitFenceV2/GetCompletedV2` 接受 `{reset_epoch,queue,sequence}`。旧 epoch
  返回 device lost，不把 reset 后同号硬件 timestamp 误认为旧任务成功。

initial limits 建议每连接最多 8 个已接受逻辑 job、2 个 staging 对象，每 staging
最多 4096 个不同 BO、4096 条 IB、每次 Append 最多 64 条记录。所有数组及去重
空间在接受前分配，硬限制由 capability 返回。超限在接受前明确拒绝，用户态按
图形提交的同步边界重新组织。range 数量溢出合并到对应整 BO，不能静默丢条目。

Commit 不再把每 16 IB 当作独立事务。若总命令放得进 ring，则一次写完 prologue、
全部 IB、epilogue、fence；若超过 ring 一次可用容量，内核保存一个逻辑 job，把
IB 顺序分段送入 ring，资源维护只在开始时执行一次，末段之后才完成逻辑 fence。
等待 ring 空间时不持资源管理锁。调度器保持该逻辑 job 的 IB 顺序及要求的提交
连续性；其他 job 不能在中间改变本批必须连续的上下文状态。分段不放资源引用，
不重新 clean，同一资源不得在逻辑 job 完成前重新交给 CPU 写入。

## 8. 提交事务、失败和完成

1. 复制并封存用户数组；检查记录大小、计数、重复条目、所有 handle/range/权限及
   mapping generation。IB 引用必须包含于资源集合，实际命令所用其他 BO 由资源
   收集器补全。取得全部 BO 和 VM mapping 引用，分配完成记录及回收节点。
2. 在 handle 表锁之外等待 CPU access 结束和前序冲突 fence；重取锁后比较
   generation。按 backing 合并引用和缓存范围，先处理不完整通知的 import，再
   处理有写入代数的 dirty range，最后执行一次本批 store barrier。
3. 预留 ring/job/fence。可失败的内存分配、检查和维护全部在 WPTR 发布前完成。
   把本批读写 fence 与 job 记录一起登记为待接受，在 ring 发布成功后同一临界区
   转为 accepted；等待者必须认识 pending 状态，不能观察未绑定 job 的 fence。
4. WPTR 尚未发布的失败撤销 pending fence、job、VM/BO 引用，保留脏范围，允许
   下次重新发布。缓存 clean 已执行无需倒放，但不能错误地认定任务已经运行。
5. WPTR 可能已经发布而返回失败，按现有 poisoned-ring 规则保留全部资源，停止
   接受新工作。逻辑 job 中间段失败同样处理。只有完成硬件停止和访问终止后才能
   回收；超时本身不代表 GPU 已停止。
6. 观察硬件 fence 后，GPU->GPU 依赖可通过该设备完成条件推进。需要 CPU 可见性
   的 V2 fence、Vulkan fence/semaphore 主机等待和 Metal completion callback，
   在相关 GPU-write import 的 invalidate 完成后才报告成功。使用有界工作队列
   每次处理至多 64 页，保存游标；observer 不在全局提交锁里执行大范围维护。
   同一 import 在后续 GPU job 仍使用时，其状态仍属于 GPU；CPU access 必须等待
   最后冲突 job。不能对已重新被 GPU 写入的范围提前执行 host-acquire。
7. 完成缓存工作后放 job 引用，向 BO ready 队列交付现成节点。每 BO 保存实际
   使用的 fence，不再用某连接最后一次无关提交的 fence 代替该 BO 的完成条件。
   完整 BO cache 可以保留自己的 CPU map_id 和 GPU VA；物理 backing cache 要等
   所有实际 CPU/GPU 映射都已解除。
8. reset 增加 epoch，把旧 jobs 标为 device lost。销毁路径先确认 GPU/DPU 等实际
   使用者 quiescence，才能解除页引用；旧完成值不会进入新 epoch 的空闲缓存。

V1 compatibility 仍保留原 cache 操作语义，但应为该连接全部可寻址 BO 加引用并
登记完整范围的保守 fence；54 目前只有 IB 引用，这一步不能省略。V2 路径完整后，
实际 54 用户态切换 V2，V1 的额外成本只影响仍使用旧协议的连接。

## 9. 实施顺序与有界记录

1. 先完成 backing 的 cache mode、实际 CPU mapping 对象与 V2 map/unmap；由 BO
   管理工作同步接入完整 BO cache、backing cache、ready 队列和关闭流程。
2. 新增 V2 capability、staging submit 和全部资源引用。先用每连接全可寻址 BO
   集合，保留正确维护，先消除 16 IB 分块重复维护和 V2 的全 WC 页扫描。
3. 接入 Turnip 实际命令资源与 Metal 使用记录，覆盖 driver 内部 BO、secondary、
   bindless 和 device address 回退。每次仅维护实际提交集合中的 import。
4. 接入 CPU begin/end、Vulkan flush/invalidate、IOSurface exporter 和共享 backing。
   有完整通知的对象按代数/范围维护，未知通知对象继续完整范围维护。
5. 最后让 CPU 可见完成和后台回收协调；相关 fence 未完成的 BO 永不入可复用队列。
   图形工作结束前发生的错误保留资源，报告真实失败状态。

沿用 256 项内存记录及固定直方图。每项记录逻辑 sequence、IB 数、ring 分段数、
资源数、完整连接回退原因、WC 属性处理页数、import clean/invalidate 页数和字节数、
CPU ownership 等待时间、cache 时间、ring 准备时间。累计计数包括 backing 创建、
真实 map/unmap 成功/失败、pending mapping 数、dirty 代数变化、范围合并、未知
import 数、回收队列长度、持有引用数和 device-loss 保留字节。默认不逐 BO 或逐页打印。

后续用户授权运行时的验收条件是：没有 CPU map/属性变化的连续 V2 submit，WC 页
扫描增量为零；一个逻辑 job 无论多少 IB，相关 cache ownership 转换执行一次；
未用 import 不增加维护字节；已通知且无新 CPU 写入的 GPU-read BO 不重复 clean；
未知 import 的完整范围维护次数与实际逻辑使用次数相符；每个实际 GPU 资源都有
对应 job 引用；完成后回收不再在 submit 中批量销毁。CPU 准备的均值/p95 与相同
动作的 54 记录对照，性能目标是让当前 7.26 ms 的固定全驻留成本消失，保留实际
新增写入字节、依赖等待和命令数量对应的成本。

上述计数属于拟议实现的验收设计。本次仅研究并写入本文，没有新增或执行测试。
