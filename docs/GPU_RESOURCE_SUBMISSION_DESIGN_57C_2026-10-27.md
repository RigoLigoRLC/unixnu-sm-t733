# 57c 后的 GPU 资源提交与 CPU 缓存同步设计

日期：2026-10-27。本文是待实施设计，基于已部署的 `system-gpu-concurrency-57c`。
设计目标是消除每次提交遍历整个 GPU VM 的缓存维护，补齐实际资源引用和错误处理，
同时保留 Metal、Vulkan、IOSurface 和 DPU 的访问顺序。旧版设计与 55b 的实施事实仍在
[54-55b 设计记录](GPU_SUBMIT_COHERENCY_DESIGN_2026-10-27.md)，后续实施采用本文的决策。

## 1. 当前证据与需要改变的行为

57c 的两个用户操作窗口共 512 次提交，来自工作区
`logs/gpu-concurrency-57c-runtime/submission-costs-summary.json`。
窗口分别为 seq 710-965、1118-1373；初始化和两窗口之间未保存的提交不参加统计。

| 软件记录 | 平均值 | p95 |
| --- | ---: | ---: |
| CPU 提交准备 | 3.981 ms | 6.635 ms |
| 遍历映射并使 CPU 数据对 GPU 可见 | 3.775 ms | 6.394 ms |
| CPU 准备结束至 CPU 观察到 fence 完成 | 33.575 ms | 61.825 ms |

第二项还包含首个 IB 数据的前后取样。第三项包含排队、GPU 执行和主机观察延迟，
不表示 GPU 持续满载。用户确认 Frame Duration 已有数值，但持续小卡顿变化不大。

| 当前入口 | 实际行为 | 本设计的改动 |
| --- | --- | --- |
| `NativeAdrenoVm.cpp:421` | `publishMapped()` 遍历共享 VM 的全部 mapping，按整 BO 维护 | 提交持有自己的资源集合，按对象属性和访问范围执行必要操作 |
| `NativeAdrenoVm.cpp:431` | `acquireImported()` 遍历所有可写 import | 只处理需要交给 CPU 的已完成写入范围 |
| `NativeAdrenoSubmit.h:41` | 每 job 仅保存最多 16 个 IB handle | job 保留全部实际资源与 VM mapping，直到对应使用结束 |
| Mesa `tu_knl_darwin.cc:1820` | 每 16 IB 提交一次，各自做完整维护 | 一次逻辑提交封存全部 IB 和资源，内部装入 ring 不重复资源交接 |
| `NativeBridgeMetalFrontend.mm:164` | `didModifyRange` 没有传递修改信息 | 正确实现 managed 路径；shared 和外部 no-copy 使用各自真实语义 |

用户随后报告的两帧闪烁已经保存为[独立现场记录](GPU_TWO_FRAME_FLICKER_57C_2026-10-27.md)。
已确认一次 Submit 返回 BadArgument 后用户态 device lost，已有 GPU 工作全部完成。
同一现场还暴露了大量 BO 缓存占满 mapping 资源而 BoMap 失败不回收的缺口。
资源容量、拒绝错误和设备故障的处理也是本设计的一部分。

## 2. 采用的成熟实现

| 参考实现 | 采用的做法 | 本项目接入点 |
| --- | --- | --- |
| Linux MSM `submit_lookup_objects`、`submit_attach_object_fences` | 命令与资源分开，保留 BO，按读写用途关联依赖和完成 fence | NativeAdreno 提交事务和资源寿命 |
| Panfrost `panfrost_gem_sync`、dma-buf begin/end CPU access | 按内存属性和范围同步，CPU 访问交接与 GPU 完成分别处理 | CPU access、缓存范围和共享内存提供方接口 |
| SM7325 KGSL `kgsl_cache_range_op` | 校验对象与范围，区分 clean、invalidate、两者，依据硬件能力处理 coherence | ARM64 物理 span 缓存维护 |
| Turnip A6xx timestamp 和性能查询 | 从 GPU 命令流写入时间戳，按实际硬件定义选择计数器 | 排队、GPU 执行、观察延迟和 busy/stall 诊断 |

参考代码已保存在 `logs/gpu-bo-research-20261027/raw/`。MSM/Panfrost 的旧下载副本没有
逐文件固定 commit，实施移植时固定实际使用的来源提交；KGSL 参考提交为
`36b842cf1fc87a989dcc8158a142d7575cee41c1`。实际 Mesa 基线为
`85fda3b36405bcaea8bffc64aa4f2651dc90770d` 加 core 中登记的补丁。

具体来源：[MSM submit](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/msm_gem_submit.c)、
[Panfrost GEM](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/panfrost/panfrost_gem.c)、
[KGSL](https://github.com/Bush-cat/android_kernel_samsung_sm7325/blob/36b842cf1fc87a989dcc8158a142d7575cee41c1/drivers/gpu/msm/kgsl_sharedmem.c)。
MSM 的 CPU prep/fini 缓存维护仍有 TODO，不能将这两个函数当作完整的 cached import 实现。

## 3. 对外图形 API 的约束

Metal shared 的 CPU 写入在提交前对 GPU 可见，GPU 写入在相关完成边界对 CPU 可见。
应用没有义务为 shared 每次调用 `didModifyRange`；该方法服务于 managed buffer。
当前 runtime 只选择 Vulkan coherent memory，不能通过新增一个应用从不调用的 flush
接口就认为获得了 shared/no-copy 的 CPU 写入记录。
[Metal shared](https://developer.apple.com/documentation/metal/mtlstoragemode/shared)、
[didModifyRange](https://developer.apple.com/documentation/metal/mtlbuffer/didmodifyrange(_:))。

Vulkan 的持久映射、CPU 访问期间和 GPU 使用期间分别记录。`vkFlushMappedMemoryRanges`
同步指定范围内此前的写入，不表示解除映射或结束全部 CPU 访问；非 coherent 内存的
`vkInvalidateMappedMemoryRanges` 要结合已满足的执行依赖。coherent 内存继续提供其
不要求应用显式 flush/invalidate 的能力。经外部原始指针进行的 host import 访问还需要
平台规定的同步，不能只看 Vulkan memory property 名称。
[Vulkan 内存规范](https://docs.vulkan.org/spec/latest/chapters/memory.html)、
[同步规范](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)。

只有实际使用硬件一致性互连和正确映射属性的对象才能标记 hardware coherent。
owned WC 的性能优化来自 CPU 映射属性和访问屏障。外部 cached 页面保留原属性，
`native_adreno_io_coherent` 保持与 A642L 实际配置一致。

## 4. 存储、引用和访问范围

`NativeAdrenoBacking` 表示驱动保留的实际存储，持有 prepared descriptor、物理 spans、
缓存属性、稳定编号及存储代数。BO handle、CPU mapping 和 GPU mapping 各自保留它。
实际 CPU mapping 对象继续由现有 V2 map/unmap 管理，持久映射不自动获得独占 CPU 访问权。

`NativeAdrenoSubmission` 分别保存三类记录：

| 记录 | 用途与寿命 |
| --- | --- |
| 保留的 BO 与 mapping 引用 | 整个对象在 GPU 使用及必要的完成维护结束前不能释放或重用；无需扩大访问范围 |
| GPU 访问区间 | backing、offset、length、READ/WRITE，以及本次 fence；用于找到冲突访问 |
| CPU 缓存操作区间 | backing、offset、length、clean/invalidate、等待依赖和完成状态；只在需要交接时建立 |

区间按 backing 内偏移排序，只有重叠区间，或访问状态、依赖和写入代数均相同的相邻
区间才能合并。记录数量不足时在接受前返回明确的资源错误，不能静默扩大成整块 heap。
BO 寿命允许整对象保留，缓存维护和执行依赖仍按实际访问区间处理。

每个 backing 保存活动 CPU access、GPU 使用和待完成 invalidate 的区间。记录关联
access ID 或 `{reset_epoch, queue, sequence}`，节点在提交接受前分配。一个统一的
完成 worker 处理缓存操作；状态锁只保护记录，不在持锁期间逐行维护或等待 GPU。

受控 exporter 提供内核持有的存储对象和代数，各进程导入的是它的子区间。匿名 host
import 在 prepare 后，利用真实物理 spans 建立全局重叠索引；部分重叠只共享重叠部分
的访问状态，各 descriptor 仍承担自己的 prepare/complete 和释放责任。用户 VA、数字
IOSurface ID 和内容 seed 均不充当跨进程存储身份。

现有 `NativeAdrenoVmAlias.h` 只检查物理映射属性冲突，需要增加共享访问记录。
索引查找发生在导入、释放和构建资源集合时，普通提交不扫描所有物理页。

## 5. CPU 缓存维护的具体规则

| 存储类型 | CPU 写入通知 | 提交与完成动作 |
| --- | --- | --- |
| 驱动 owned WC | 保持图形 API 规定的线程同步和提交顺序 | 本次提交一次必要的 store barrier；CPU 取得 GPU 结果时执行 acquire barrier，不逐页 clean |
| 明确为非 coherent 的 cached 内存 | flush/invalidate 的明确范围 | flush 同步此前写入；GPU 使用前处理必要的所有权交接；CPU 读取在完成依赖后 invalidate |
| 全部生产方已接入的共享存储 | CPU access begin/end、真实写入范围和 producer fence | 按变化范围维护；相同范围无新 CPU 写入且持续 GPU read 时可以跳过重复 clean |
| 现有 cached no-copy 或未知 IOSurface 生产方 | 没有覆盖全部 CPU store 的通知 | 保留每次实际使用时的缓存维护，不凭 seed、旧 fence 或“未收到通知”跳过 |

新接口的含义固定为：

- `CpuAccessBegin` 等待冲突 GPU 使用，必要时取得 CPU 可见性，返回 access ID。
- `CpuAccessEnd` 关闭该 ID，登记本次实际写入范围；它只适用于确实使用此协议的生产方。
- `CpuFlushRanges` 和 `CpuInvalidateRanges` 执行范围同步，不关闭持久映射或其他 access ID。
- GPU 准备过程在 producer 依赖满足之后同步输入，然后准许命令执行。
- GPU 完成过程分别记录硬件完成、CPU 可见性完成及对外通知，不用一个布尔值代替三者。

当前 ARM64 helper 只接受 4 KiB 对齐范围，并维护每页全部缓存行。实施字节范围同步时，
helper 必须接受 span 内 offset/length，仅向真实 CPU cache-line 边界取整，并保证交接
覆盖这些缓存行。不能只缩小上层参数，底层仍然处理整页。

对已经完整交接的 cached 范围，GPU 写入前 clean+invalidate，GPU 完成后 invalidate。
当前 `NativeAdrenoMemory.cpp:910` 对 import 在完成时 clean+invalidate：若 CPU 仍
持有旧脏行，clean 可能覆盖 GPU 新内容。改动必须同时建立交接和冲突访问顺序，不能
只将这一条指令换成 invalidate。

未知 cached import 的原有维护属于兼容路径，其限制需要保留在能力模型中。它没有
自动解决 CPU/GPU 同时写入同一缓存行不同字节的问题；Metal shared 和 Vulkan coherent
不能被悄悄改成要求应用按页同步。新导入能力只有在 exporter、映射属性和访问协议能
满足对应语义时才发布；不能满足的新增模式在创建时返回 Unsupported。现有系统路径
通过实际资源集合减少维护，全面取消维护需要 IOSurface 生产方集成。

前端 `flushSurfaces` 当前在提交与完成两端对整张 surface 调用 `sys_dcache_flush`，
见 `NativeBridgeMetalFrontend.mm:273,922,955`。新能力协商成功后由内核统一承担该
对象的维护，前端退出对应重复操作；旧内核继续沿用旧路径。map、unmap 的隐式缓存
操作也必须迁入同一访问协议，不能只改 `publishMapped()`。

Metal shared event 的 GPU signal、CPU wait/write/signal、GPU wait 后继续执行，也要
形成访问交接。用户态在 CPU 参与的 event 边界拆分可执行工作，先完成 GPU 输出的
CPU 可见性，再通知 CPU；收到 CPU signal 后发布输入，才提交后续部分。保留一个
逻辑 command buffer 的完成和错误状态。不能先提交等待 CPU 的后半段，再阻塞完成
worker 去等它。该路径与最终 completion 使用同一协议。
[Apple shared event](https://developer.apple.com/documentation/metal/synchronizing-events-between-a-gpu-and-the-cpu)。

## 6. 一次 SubmitV2 与完整资源收集

新增一次 `SubmitV2`，使用固定 header 加 offset/count 数组；不建立跨调用 staging 对象。
header 含 version、header_size、total_size、flags、queue、reset_epoch 和各数组位置。
资源条目含 handle、mapping generation、BO 内 offset/length、READ/WRITE。
IB 条目引用资源数组索引和 BO 内 offset/length，GPU VA 由内核保留的 mapping 计算。
回复返回 epoch、queue 和 64-bit logical sequence。

第一版能力上限定为 packet 256 KiB、4096 条资源区间、512 条 IB，全设备仍最多 8 个
已接受 job。512 IB 约占 8 KiB ring，适合当前 32 KiB ring 的连续空间要求。超过协议
上限在执行前明确返回；上层命令生成按能力限制组织工作，不把同一请求悄悄提交一半。
更大的逻辑 job 需要另行完成内核连续分段发送，不能仅提高常量。

IOKit 已支持 inline 和 `structureInputDescriptor` 两种输入。驱动先检查总长度并完整
复制到有界内核 packet，再解析副本，核对 count 乘加、数组边界、未知字段和资源范围。
不重复读取用户可修改的数组。Darwin 后端新增可变输入路径，不沿用 `dw_call` 当前
只容纳 V1 Submit 的栈缓冲。selector 1-19 及现有 V1 结构保持兼容，能力查询选择新路径。
第一版 Vulkan wait 仍在用户态依现有顺序完成，不同时增加新的 GPU scheduler wait 协议。

Turnip 必须收集以下内容，而不是只交出 Metal 的应用资源或 `tu_cs` 的存储 BO：

| 资源来源 | 记录时机 |
| --- | --- |
| vertex/index、indirect/count、copy/fill/update、query、predication、marker | 命令建立地址引用时记录 BO 和范围 |
| attachment、resolve、纹理、depth/stencil、UBWC/metadata、mip/plane | 从实际 image binding 与布局取得完整存储范围，首版不按可见像素裁剪 |
| descriptor 普通更新、复制、push、template | 同步维护每个槽的资源记录；保存各 draw/dispatch 实际使用的 set，不只保存最后绑定值 |
| dynamic/push descriptor | 记录复制到命令存储的最终内容及应用 offset 后的范围 |
| secondary、动态渲染追加命令 | 合并命令时同时合并资源和 descriptor 使用记录 |
| shader binary、scratch/private、global/tessellation、visibility、临时 MSRTT、上传及所有 CS | 包含 Turnip 内部资源及子分配范围 |
| submit 时创建的 visibility/control patch、trace、autotune、fence BO | `tu_queue.cc` 完成所有追加后再封存最终集合 |

`UPDATE_AFTER_BIND` 的 descriptor 在 submit 取最终快照。允许 pending 更新的未使用槽
按 Vulkan 规则处理，不将它错误解释为应用能随意修改正在使用的 descriptor。
[descriptor binding flags](https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorBindingFlagBits.html)。

BDA、descriptor buffer 或 GPU 生成地址无法证明完整集合时，内核接受前快照该连接
所有已映射 BO 并保留引用，记录具体原因；不能扩展到其他连接的整个 VM。这个集合
保证对象寿命，维护范围另由对象访问协议决定。未知访问范围按相应完整资源处理，
明确记录额外成本，不宣称已取得精确 dirty 区间。

完整 BO 引用先于精确收集落地。只有已覆盖的命令种类才使用精确集合，其余走显式
回退；逐项完成上表后再移除对应回退。资源引用属于本次 job，command buffer reset
和用户态对象释放不影响已经接受的工作。

## 7. 接受、执行、完成和错误

1. 在锁外复制输入并分配 job、区间和去重存储。连接活动调用引用保持 owner 存活。
2. 短持 `g.lock` 检查连接、queue、epoch、generation 和权限，一次取得全部稳定 BO
   与 mapping 引用，登记访问区间。job 此后直接持有对象，不能重新用已复用的 handle 查找。
3. 释放状态锁。提交顺序锁或单一 worker 协调本次维护；等待冲突依赖时不持资源锁。
   同 ring 的纯 GPU 顺序继续由命令和 barrier 保证，只在 CPU 维护或外部访问确实需要时等待。
4. 取得相应区间访问权后执行缓存维护和屏障。短锁内重新核对 epoch，预留 fence/job。
   发布 WPTR 前所有资源和完成记录已就绪；成功发布后返回已接受的逻辑 fence。
5. 观察 GPU fence 后记录硬件完成。需要给 CPU 的写入范围由完成 worker 在锁外
   invalidate，完成 acquire barrier 后才发布相应 CPU 可见状态。
6. 需要这一处理的范围在接受时就为后续冲突 writer 登记依赖。下一个 writer 不能先
   进入 ring，完成 worker 再去维护上一个 writer 的输出。无冲突范围独立推进。
7. Metal completion、shared event 通知以及需要隐式 CPU 可见性的 host wait 消费
   CPU 可见状态；Vulkan 非 coherent 内存仍保留显式 invalidate 语义。GPU 内部依赖
   只消费硬件完成状态，不为未发生的 CPU 读取多做维护。
8. job 完成后放引用。完整 BO 缓存可继续保留自己的受控 CPU/GPU mapping；底层物理
   backing 交给其他 BO 前必须解除旧映射。DPU scanout 引用由显示路径在替换后释放，
   GPU fence 不能代替显示端完成。

输入错误返回 BadArgument；协议永久超限、NoMemory、临时 Busy 和 device lost 分开。
WPTR 前失败释放本次保留记录，保留尚未完成的交接状态；缓存 clean 无需反向操作。
WPTR 可能已发布的错误保留全部资源并停止新工作，不能自动重试造成重复执行。
超时只报告未完成，reset 先停止访问、推进 epoch，再释放可释放的旧引用；DPU 使用
中的存储继续保留。回复丢失时也不能盲目重发已可能接受的请求。

## 8. 映射预算与命令录制失败

本节处理两帧闪烁现场暴露的确定缺陷。内核负责共享 VM 的实际资源预算，在 `g.lock`
下原子预留 mapping 记录，并区分已预留、已映射和等待最终释放。新 V2 的创建并映射
操作一次取得名额、物理存储和 GPU mapping；失败统一归还。既有 V1 调用同样受预算
管理，不能绕过 V2 的预留占满剩余记录。

每连接取得基本份额并可借用空闲份额，所有份额之和由全局 4096 容量决定。出现其他
连接需求时，内核限制超额连接新增缓存占用，并发布回收要求；现有活跃 mapping 在
正常引用结束后释放。预算还应分别报告页表空间、IOVA 和物理内存压力，避免把它们
全部混为一个 NoResources。

用户态缓存同时遵守字节预算与内核给出的对象数量预算。BoMap 可重试资源不足时，
worker 回收 fence 已完成的缓存对象，等待实际 BoFree 成功后有界重试；当前申请的
BO、IOVA 和 ID 始终由同一事务持有，最终失败统一撤销。跨连接回收通过内核预算
通知完成，调用方不直接释放别人的对象。活动或 DPU 正使用的资源不能拿来满足缓存预算。

命令流扩容失败必须保存第一次错误。`tu_cs_reserve()` 当前只 assert 成功，release
构建会继续运行；修改必须同时覆盖 reserve、单字/数组写入、packet builder、直接写
指针、子流结束与 IB entry 生成。失败之后不再写入、不推进指针、不产生可提交 IB。
`tu_cs_end`、动态渲染追加、visibility、trace/autotune 准备将错误传入 command buffer
的 `record_result`，EndCommandBuffer 和 queue submit 返回原始 OOM。实现可以采用
显式返回值或统一首次错误状态，但所有写指针调用点必须遵循同一失败规则。

已有部分工作执行后才发现后续错误时，继续保留已经提交的引用并报告实际失败；不能
向上报告整批未执行。SubmitV2 的全部准备在接受前完成，可缩小这种部分执行的边界。

拒绝记录保存首个错误的阶段、请求序号、连接、IB/资源索引、handle/generation、
请求范围、实际映射范围、对象数和资源容量。固定内存记录按 USB 查询输出，正常路径
不逐 BO 打印。显示端只接受成功完成的 producer 内容；device lost 时保留最后成功
画面并向上报告失败，不能把失败的新绘制当成成功的新 frame。

## 9. 区分排队、GPU 执行和 CPU 观察延迟

每个逻辑 job 保存 CPU 入口、缓存处理结束、WPTR 发布前后、GPU 开始/结束 ticks、
CPU 首次观察 fence、CPU invalidate 完成和用户完成通知时间。GPU 开始标记说明 CP
开始消费本次命令；结束标记使用完成事件，不能用单纯 CP 取指位置冒充全部渲染完成。

A6xx 时间戳过程参考 Mesa `tu_device.cc:2279` 的 `RB_DONE_TS` 与
`tu_query_pool.cc:2078` 的 `CP_REG_TO_MEM`。后者取得完整计数需要的同步会改变流水线，
不能为了计时默认给每个 draw 插入全 GPU idle。优先在已有提交边界记录，诊断模式才
启用更细采样，并记录采样带来的同步成本。A7xx 的事件参数不直接用于 A642L。

CPU 在读 GPU always-on counter 前后分别取时间，以夹取区间校准时钟；保存误差范围、
counter 位宽、回绕与 reset epoch。图中分别显示：

- WPTR 发布至 GPU 开始标记：已提交但尚未开始消费本次命令的时间。
- GPU 开始至结束标记：包括设备内部等待的命令执行区间。
- GPU 结束至 CPU 首次观察：轮询、调度及观察延迟。
- 观察至 CPU 可见完成：缓存维护和通知准备。

相邻 GPU 工作可以流水重叠，不能把每个 job 的时间段直接相加称作整帧 GPU 时间。
整帧关联需要另外传递 Metal command buffer/frame ID，并以实际起止区间汇总。

硬件计数器由一个负责人配置，保存 chip/revision、selector、寄存器和计数宽度。
候选项来自本机 A6xx 定义：CP busy、PM4 starve、WFI/sync stall、SP busy/指令、
UCHE/纹理或内存停顿与流量；结合实际频率和频率驻留时间分析。具体组合按 A642L
支持的计数器数量选择，不把任一部件 busy 百分比称为所有计算单元利用率。

性能采样前启用支持的硬件 counters 与 shader profiling，并确认保存数据中存在有效
counter 样本、时间边界和对应 shader 标识。无法获得的项目明确记为未采集；当前
57c 软件记录不冒充这些测量。所有记录使用有界内存，默认按需汇总输出。

## 10. 实施顺序与完成标准

| 顺序 | 一次完成的修改范围 | 完成后应具备的行为 |
| --- | --- | --- |
| 1 | mapping 预算、BoMap 回收重试、命令流首次错误及全部传播调用点 | 资源不足在接受前返回明确错误，不继续写出坏 IB，不靠增加4096上限掩盖问题 |
| 2 | SubmitV2、全部资源引用、一个逻辑 fence、协议输入/失败分类 | 512 IB 范围内一次提交；已接受工作拥有全部资源；资源不足和device lost可区分 |
| 3 | 完整命令资源收集、共享存储重叠记录、提交和完成 worker | 未使用的外部资源不参与维护；缓存操作不触碰后续正在写入的范围 |
| 4 | owned WC 批量屏障、前端维护迁移、受控生产方访问接口 | 同一次逻辑使用不重复维护；有完整写入通知的未变化输入不重复clean |
| 5 | IOSurface 全部生产方与shared-event交接、所需范围helper | 按真实范围交接；未知生产方不会被误标为已通知；对外能力符合实际实现 |
| 并行 | GPU时间戳、硬件计数器和shader采样 | 能分别解释排队、执行、观察、CPU完成时间以及设备停顿原因 |

软件记录增加资源条目数、去重 BO 数、未知地址回退原因、import clean/invalidate 字节、
维护重复次数、访问等待、mapping 预留/使用/释放、缓存回收结果和首个拒绝原因。
分析覆盖用户操作全过程，初始化另列。目标是消除当前随所有驻留对象增长的固定提交
成本，保留随实际写入、执行依赖和命令数量增长的必要成本；不预先承诺帧率或毫秒收益。

实际修改位置：设备 `drivers/Adreno6xx/`、`drivers/NativeMobileDisplay/`；公共 core
的 `patches/mesa/` 及补丁登记；`drivers/AdrenoMetal/` 的 frontend/bridge 与 runtime
补丁。`.cache/mesa-work` 和 `.cache/gpu-reference` 是阅读与展开来源，不能作为唯一
保存修改的位置。全部实际源码按组件提交后再统一构建，新镜像部署完成后记录实测。

本文完成的是设计与现场证据整理，尚未实施这些修改、构建新包或改变设备运行状态。
