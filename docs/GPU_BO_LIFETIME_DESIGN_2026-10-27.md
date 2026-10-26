# BO 复用、分配与回收设计

日期：2026-10-27。目标设备：SM-T733，Adreno 642L。本文给出 54 之后的选定方案及实施条件。当前轮先增加缓存原因统计与慢操作上下文，继续采样；本文中的缓存、异步回收和 ABI 改动尚未实施。

## 1. 已知问题与选定方案

54 的 kernel backing cache 命中 1,374 次、未命中 10,809 次，命中率 11.3%，峰值约 3.1 MiB。9,873 次 eviction 同时包含 client close 的全池清理，现有数据尚未拆开容量原因和关闭原因；64 个对象上限是否是主要原因由本轮新增计数确认。Turnip 的完整 BO 缓存是另一层，不能用 kernel backing 的命中率代表它。分配 42.105 ms、descriptor release 25.586 ms 的最大样本没有尺寸、来源和导入类别。

选定结构如下：

1. Turnip 按 Freedreno 的细分大小桶复用完整私有 BO，保留 native handle、GPU VA/PTE 和有效 CPU map。实际最后使用 fence 决定何时可复用。
2. kernel 保留已 prepare、已设好 Normal WC 属性的整块 backing，供完整 BO 缓存未命中的申请使用。backing 缓存不保留前一个 client 的用户映射或 GPU VA。
3. TTM 的对象引用和延迟销毁关系用于最终释放：最后应用引用消失只转交所有权，完成通知只解除 GPU 引用。真正的 CPU unmap、GPU unmap、属性恢复、complete 和 descriptor release 由独立回收任务执行。
4. Metal 继续复用 command pool；超过保留预算时，将已完成且由它独占的 pool 交给回收任务。完成处理线程不承担整池销毁。
5. command/state 的小块分配优先使用 Turnip 已有 `tu_suballocator`。只有采样证明大量独立小 BO 在完整复用之后仍占主要成本时，才扩展子分配范围；无需先引入另一个通用 slab 框架。

这套结构改变对象存活和工作执行的位置。单纯把数组的 64 改大、加 sleep 或把一次全量销毁搬进每次 allocation 都不能完成这些要求。

## 2. 先采哪些数据，再启用哪些改动

两层分别累计 request count/bytes、eligible、hit、miss，按实际申请尺寸分桶。每次 request 只有一个主 miss 原因，候选对象数另外累计。kernel 的主原因按 empty、size、attributes、quarantined 区分；Turnip 额外区分同尺寸同属性对象仍等待 fence。导入、共享、implicit sync、固定 IOVA、地址重放、超单对象限制和压力拒绝各有 bypass 原因。bucket 只是统计区间，本轮保持 exact-size lookup。

淘汰原因分别记录 object limit、byte limit、client close、memory pressure、allocation retry；未来增加 age 和 metadata limit 时各占独立项。insert reject 和 request miss 分开记录。容量事件同时保存当前/峰值 bytes 和 objects，以及本次申请尺寸。

慢分配和释放使用固定 64 条环，默认阈值 8 ms，记录操作、耗时、尺寸、物理 spans、地址位宽、contiguous/WC/import 类别、来源枚举、client 标识、缓存结果、保留量与 available pages。现有 MemoryAllocate/MemoryDescriptorRelease 范围已足够；本轮不继续拆 XNU 内部步骤。热路径只更新固定内存，GPU 查询一次拷贝后输出。

| 新采样结果 | 选择的改动 |
| --- | --- |
| object limit 在低 bytes 下频繁发生，且相同 bucket 很快再次申请 | 去掉独立的 64 项缓存数组，改为对象内链节点和有界 metadata 页块，保持总字节预算。 |
| client close 占主要 eviction | close 只交还该 client 的对象，保留服务拥有的全局 backing cache。 |
| 大量相邻尺寸不能 exact match，且属性匹配 | 启用 Freedreno 大小分档，保留 requested size 与实际 capacity 两个值。 |
| eligible miss 的同类对象主要在 pending fence | 用每 BO 的真实最后使用 fence，并由完成事件及时转入可复用队列。 |
| command pool 销毁与大量 free 同时出现 | 将已完成 pool 的销毁交给回收线程，按实际 command BO bytes 管理保留预算。 |
| 大量同类小对象每次仍要独立 BO | 在明确的 command/state 使用者接入已有 `tu_suballocator`，保留每个 slice 的完成条件。 |
| 大对象冷分配占慢样本，缓存可复用工作量很少 | 保持大对象直接分配策略；先消除全局锁内分配和重复清零，不预留大块物理连续内存。 |

## 3. 源码依据和移植范围

实际 54 Mesa 输入是 `sources/UniXNU/.cache/mesa-work`，基准提交 `85fda3b36405bcaea8bffc64aa4f2651dc90770d` 加设备 Darwin 补丁；Metal 输入是 `sources/UniXNU/.cache/gpu-reference/metal2vulkan`，提交 `720ef1ce9ae2a2068999a8675717418b9416283e` 加本地补丁。下列本机行号来自本轮读取，后续诊断修改可能移动行号。

| 依据 | 函数和准确入口 | 本方案采用的部分 |
| --- | --- | --- |
| Freedreno | Mesa `src/freedreno/drm/freedreno_bo_cache.c:105` `fd_bo_cache_init`，`:138` `fd_bo_cache_cleanup`，`:233` `fd_bo_cache_alloc`，`:279` `fd_bo_cache_free` | 1/2/3 页及每二次幂区间四档大小；flags 匹配；缓存链内摘除后在锁外实际删除。 [固定版本](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/freedreno/drm/freedreno_bo_cache.c) |
| Mesa pb_cache | Mesa `src/gallium/auxiliary/pipebuffer/pb_cache.c:145` `pb_cache_is_buffer_compat`，`:111` `pb_cache_add_buffer` | 区分尺寸/属性不兼容与兼容但未完成。其锁内 destroy callback 不用于本机慢 descriptor 销毁。 [固定版本](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/gallium/auxiliary/pipebuffer/pb_cache.c) |
| Mesa pb_slab | Mesa `src/util/pb_slab.c:125` `pb_slab_alloc_reclaimed`，`:211` `pb_slab_free`，`:77` `pb_slabs_reclaim_locked` | 每 heap/大小一组，free 后待 `can_reclaim`，新 slab 在 mutex 外分配；两个失败候选后停止无效扫描。保留这些边界，当前优先现有 Turnip 分配器。 [固定版本](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/util/pb_slab.c) |
| TTM | `logs/gpu-bo-research-20261027/raw/linux-ttm_ttm_bo.c:235` `ttm_bo_delayed_delete`，`:249` `ttm_bo_release` | 对象保存自己使用的 fences，末次 put 将对象交给 workqueue，完成后释放。 [上游](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/ttm/ttm_bo.c) |
| TTM 页池 | `raw/linux-ttm_ttm_pool.c:326` `ttm_pool_type_give`，`:361` `ttm_pool_type_take`，`:377` `ttm_pool_type_init`，`:1306` `ttm_pool_shrinker_scan` | 按缓存属性等条件分组；短锁移动队列；归池清零；内存压力按要求释放页数。XNU 使用整块 prepared descriptor，不能复制 Linux struct page 的释放方式。 [上游](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/ttm/ttm_pool.c) |
| AGX | `raw/mesa-asahi_lib_agx_bo.c:51` `agx_bo_cache_fetch`，`:138` `agx_bo_cache_put` | 完整 flags、尺寸、alignment 匹配，SHARED 不缓存。fetch 没有 fence 等待，Darwin 必须保留自己的完成条件。 [上游](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/asahi/lib/agx_bo.c) |
| V3DV | `raw/mesa-broadcom_vulkan_v3dv_bo.c:93` `bo_from_cache`，`:330` `v3dv_bo_map_unsynchronized` | 按页数找完整对象，复用前确认 idle，保留已存在 CPU map。 [上游](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/broadcom/vulkan/v3dv_bo.c) |
| Panfrost | `src/panfrost/lib/pan_bo.c`，`panfrost_bo_cache_fetch` / `panfrost_bo_cache_put`，ChromeOS Mesa 固定提交 `a17edd5fe64daa46c3f2ed8f248d481c991a6c91` | 大小桶、flags、对象 busy、madvise 保留结果共同决定复用。Linux purgeable backing 在 XNU 本方案中由显式 pressure 回收替代。 [固定版本](https://chromium.googlesource.com/chromiumos/third_party/mesa/+/a17edd5fe64daa46c3f2ed8f248d481c991a6c91/src/panfrost/lib/pan_bo.c) |

`raw/` 的完整前缀是 `logs/gpu-bo-research-20261027/raw/`。Linux master 和 Mesa main 的这些副本下载于 2026-10-27，原采集没有保存不可变上游 commit；以保存的副本定位上述行号，实施时固定移植来源 revision。Panfrost 的引用明确是固定历史版本，用于比较成熟机制。

可直接移植的是 Freedreno 分档与链表操作、属性匹配、Mesa 已有子分配器，以及 TTM 的引用和完成后销毁关系。跨进程缓存预算、XNU 内存压力通知、`IOMemoryMap` 所有权、异步 BoFree 确认、reset epoch 和线程调度预算属于本项目的 XNU/Darwin 适配。它们必须作为一个明确的生命周期实现完成。

## 4. 对象和引用

`darwin_bo_storage` 代表一个仍由当前连接拥有的 native BO：保存 native handle、Mesa ID、capacity、申请属性、IOVA、有效 CPU map_id/VA、每队列最后 fence、缓存/退休链节点。应用的 `tu_bo` 持有它；应用释放后，缓存或退休管理器接过同一份记录。进入回收链时无需另行分配一个记录，也不复制仍可被提交引用的内存对象。

kernel BO 对象拥有 backing 引用。handle 表拥有一个 BO 引用，GPU mapping、已接受 job、实际 CPU map 和回收管理器各持独立引用。最后一个应用 handle 关闭后，job 与 map 仍能保留对象。表槽只有在句柄不可再被查找、旧异步操作完成且 generation 不再匹配旧请求后才可重新使用。

BO 的生命周期依次为 allocation reservation、live、waiting for its jobs、reusable 或 reclaiming、released。quarantine 是独立终止路径：发生不确定的硬件停止、PTE/TLB 操作失败、CPU map 解除失败或属性恢复失败时，保留必要引用和预算占用，禁止进入 reusable。

V2 submit 按完整资源表取得 BO 与 VM mapping 引用，逻辑提交的所有 IB 共用一个 fence。fence 表示 `{reset_epoch, queue, sequence}`；只有成功发布 ring 才把该 fence 登记到实际使用的每个 BO。WPTR 已发布之后发生错误，资源仍由 job 持有到硬件 quiescence。每 BO 跨队列使用保存各队列最后 sequence；不能把不同队列数字直接取 max。

完成观察先完成必需的 GPU-to-CPU 可见性工作，再更新 CPU 可见完成值，解除 job 引用，将符合条件的已释放 BO 移入 ready 队列。Turnip 不再在 `darwin_bo_finish` 读取 `dw.last_fence` 代表本 BO；未使用过的 BO 无需等一个无关提交。

54 的 `NativeAdrenoSubmit` 只对 IB 调用 `holdJob`。若保留旧 ABI 而没有完整资源表，兼容实现需要新增“对该连接全部存活、可被 GPU 寻址的 BO 取快照并持引用”的保守路径；不能将此视为 54 已有保证。V2 的资源表完成前不得仅缩减为 IB 的生命周期。

完整 BO cache 仍拥有自己的有效 CPU map 引用，因此其可复用条件是没有应用/GPU 使用且 cache 持有的 map 保持有效。kernel backing cache 的条件更严格：所有 CPU 用户映射已实际删除、所有 GPU mapping 已安全解除、所有 job 已完成，剩余 kernel alias 与 WC 属性一致。

## 5. 大小桶与实际复用单位

私有 BO 申请先向上取到 4 KiB，再选 Freedreno 的 4/8/12 KiB；从 16 KiB 起按 `S, 5S/4, 3S/2, 7S/4` 分档，直到 64 MiB。每次新建、IOVA 分配、CPU map 长度、PTE 范围和字节记账都使用实际 capacity。逻辑 resource 的可访问范围继续使用 requested size；不能只把 cache 比较改成 `>=` 而保留旧分配长度。

bucket 内按完整分配 flags、memory properties、cache mode、地址位宽、连续性和对齐要求分组。同一完整 BO 的 IOVA 必须满足新申请的 alignment。特殊 replay/fixed address、导入、共享、implicit sync、超过单对象限制的对象直接走其原生命周期。kernel backing 仍要求其真实 capacity 和属性完全匹配，由 Turnip 统一分档后的申请提高匹配机会。

Turnip 的 cache 复用单位是完整 BO；kernel 的复用单位是完整 prepared backing。shader、pipeline state、event 的小块继续由 `tu_suballocator` 复用一块 BO 内的区间。源码是 `src/freedreno/vulkan/tu_suballoc.cc:52` `tu_suballoc_bo_alloc`、`:107` `tu_suballoc_bo_free`；`tu_cs.cc:63` 支持外部子分配的 stream，`:141` 普通 stream 仍创建独立 BO。这几个使用者不需要换成 kernel slab。

若后续接入 command 子分配，按 command pool 的使用和完成寿命分配 arena：pool 独占 arena 的 slice，pool 所有 job 完成后才能重置 offset；物理 BO 保留引用直到所有 slice 无使用者。shader 等长寿命数据保持现有独立 suballocator，避免少量长期 slice 留住大量短期 command storage。空 arena 按相同 idle budget 记账，压力下释放；pending arena 计入未完成占用。每个 slice 的元数据在创建时预留，free 不分配。

## 6. 字节预算、metadata 与背压

各层保留量由设备服务统一记账，转移所有权不增加新的物理内存计数。账目分为 live、waiting、ready cache、reclaiming 和 quarantine；另计 imports 的 wired bytes、mapped VA bytes 和 metadata bytes。物理内存直到真正释放才从总 committed 中扣除。内核持有的全局 backing 与用户态持有的完整 BO 不能各自声称同一物理内存已经释放。

以下是实施配置的起始边界，当前采样轮不启用。它们是有界策略，后续由分桶占用和运行峰值调整：

| 项目 | 起始值与动作 |
| --- | --- |
| 全设备可复用内存 | 高水位 128 MiB，回收到 96 MiB。包括所有连接的完整 BO、闲置 command arenas 和 kernel backing。 |
| kernel backing | 高水位 32 MiB，回收到 16 MiB；包含在上述 128 MiB 中。 |
| 单连接完整 BO | 上限 `min(64 MiB, 该连接GPU VA窗口/4)`，仍受全局余额限制。 |
| 等待最终物理释放的 backlog | 高水位 64 MiB，低水位 32 MiB；达到高水位后不接受增加 committed 的新 allocation，优先已有完整 BO/backing 命中。free 仍能转交原有对象节点。 |
| 全设备 committed | 起始硬上限 `min(1 GiB, 可管理内存/4)`，包括 live、pending、cache、reclaiming、quarantine；启用前必须把当前 active 峰值列入评审。导入 wiring 单独记账并参与可用页 admission。 |
| 系统内存压力 | 沿用现有 available-pages 低阈值 32768 页作为降额触发；恢复阈值设为 49152 页形成滞回。warning 停止新增 idle retention，critical 将 idle 目标设为 0。 |

新申请先预留物理 bytes、VA、handle slot 和 metadata credit，成功之后才执行锁外分配。后台正释放内存时，申请者可在无管理锁状态下等待“实际释放 bytes 或 fence 前进”的事件；同一次申请最多一次回收后的重新分配。超时、没有可释放对象或硬上限仍不足时返回明确的 memory error。不能先分配成功、把旧对象无限加入后台队列，再等未来压力处理。

metadata 使用按需增长的固定大小页块，例如每次 16 KiB，每连接最多 1 MiB；实际对象容量由 `sizeof(storage node)` 算出，受 ABI 的 handle/VM object 上限共同限制。现有每 client 4096 handles 和全局 VM 容量是存活对象上限，不是新的缓存目标。缓存节点嵌在已经计费的 storage 中，bucket 和 LRU 只保留指针。稀疏 ID 页和物理 span 数组也按实际分配长度记入 metadata；归还最后节点的空页块可释放。

一次 free 不新增对象或 metadata credit，所以退休队列无需再单独设置 256 项并在溢出时等待全设备 idle。所有 pending 节点数天然小于已经获得 credit 的存活对象数。缓存 metadata credit 不足时直接选择 reclaiming；若实际释放落后，阻止新的 allocation，现有 free 仍可完成移交。quarantine 不释放 credit，故持续故障最终触发 allocation 拒绝和设备恢复，不会无限增加内存。

压力来自内核可信状态。服务向用户态发布 pressure generation 与目标 bytes，Turnip 将符合释放条件的完整 BO 转交回收；内核只能立即清理自己拥有的 backing。用户态未响应期间内核继续计费并拒绝增加 committed。IOUserClient 被关闭时由服务接管其对象，解除它的实际 CPU mappings 后继续处理。

## 7. 锁与锁外分配

连接的 handle 表锁只保护查找、状态和引用；找到 BO 后保留引用并释放表锁。cache lock 只保护桶、LRU、credit 和节点状态。VM lock 保护 IOVA/PTE 更新，submit lock 保护队列和 job 发布。需要同时操作多层时先摘下并持引用，再进入下一层，避免持 cache/handle 锁执行 IOConnect 调用、prepare、complete、CPU map 或等待 fence。

kernel `allocBoInner` 改为以下完整流程：在连接锁内预留 generation 标识的表槽和 credit，并持连接引用；锁外取得 backing 或执行 descriptor allocation、prepare、zero、属性建立；回到连接锁，检查仍 open 且 generation 相同，再公布 handle。close 已经开始时将新对象转交回收并退回尚未消费的 reservation。这样 client close 和锁外 allocation 的竞争不会写入已经释放的 `entries_`。

cache hit 摘出对象后不立即对外公布。必要的清零或 CPU map 重建在该对象独占状态下完成；成功才交还 live，失败则转回收/quarantine。metadata 页块分配同样在锁外，回锁后重新检查需求并处理并发多分配；所有多分配仍先获得预算。

当前慢点对应 `NativeAdrenoPublish.cpp:578-590` 全局锁内 dispatch，`NativeAdrenoUserClient.cpp:238-249` 锁内 allocation 与 WC 设置，`NativeAdrenoMemory.cpp:94` 的 `destroyOwnedStorage`。已测全局锁竞争很小；本改动直接防止慢 VM 操作阻塞其他 client，不把它当作 7 ms submit 的主要原因。

## 8. 完成通知与回收任务

每连接有单调 completion generation；内核完成处理更新已完成 fence 后发送合并通知，Turnip 的 waiter/reclaimer 等待通知并读取完成值。任意成功 fence wait 也可以促成同一 generation 更新。通知只负责唤醒，具体对象依据自己记录的 fence 判断，避免一个不相关的长 job 挡住所有 pending 对象。reset 进入新 epoch；旧 epoch 的对象必须等待明确的 quiescence，不能因为新序号变大就释放。

用户态每 device 一个回收任务，kernel 每服务一个允许阻塞的回收任务。触发来源是完成前进、超过高水位、内存压力、close 或 allocation retry。无任务时等待 condition/event；不依赖下一次 allocation 扫描，也不用周期 sleep 限速。

每次调度先在短锁内摘取最多 32 个已就绪对象，随后锁外执行。一个调度轮以 32 个对象、4 MiB 或 1 ms 任一达到为结束条件，至少允许当前对象完成；剩余工作重新排到正常调度队列尾部。单个 descriptor release 是不可拆分的 XNU 操作，可能超过 1 ms；该阈值限制下一对象的开始，不承诺抢占已经进入 XNU 的调用。大块清零可按 256 KiB 分段保存进度，不在完成回调执行。

回收顺序为：终止实际 CPU maps并释放 map 引用；在 VM lock 内移除 GPU mapping 和完成 TLB 同步；移出 VM；在锁外把合格 backing 转入全局缓存，或恢复属性并 complete/release；收到最终释放结果后归还 IOVA、native handle ID 和各项 credit。析构错误保存原对象和已完成步骤，避免重复 unmap 或重复 complete。

BoFree 的异步版本需要明确返回“内核已取得最终回收所有权”，并以完成序号确认 handle/VA 可重用。当前 ABI 的 BoFree success 含义保持不变；过渡期可以先在用户态 worker 调用现有同步 BoFree，把慢工作移出 allocation/完成回调，然后再实现 kernel 的异步接管。不能让用户态在仅入队成功后就重用仍可能被 GPU 访问的 IOVA。

## 9. Command pool 的所有权和保留

`metal2vulkan-runtime/src/spares.rs:150` 的 `give_command_pool` 在 pool 超过 64 command buffers 或队列族已有 8 个 idle pools 时，调用 `release_in_section`。`command_buffer/mod.rs:2728` 在完成处理阶段调用它。Mesa `tu_cmd_buffer.cc:4175` 的 destroy 逐个释放七组 command streams，`tu_cs.cc:81` 再遍历其 BO 数组。 [Metal 固定版本](https://github.com/steelbrain/metal2vulkan/blob/720ef1ce9ae2a2068999a8675717418b9416283e/crates/metal2vulkan-runtime/src/spares.rs)

pool 保留按实际 command BO capacity bytes、command buffer metadata bytes 和所属 queue family 记账。Mesa command stream 新建/销毁 BO 时维护 pool 的字节数，通过 Darwin runtime 的内部接口读取；不把 command buffer 个数当作等价内存。该统计只计算 pool 持有的 command storage，不重复计入引用的 textures。

完成处理将 pool 所有权交给 spare manager，合格者变 idle。超预算者带着现成记录进入 worker 队列，随后正常执行 completion handlers。取池先按 family 匹配并取得独占所有权，在锁外执行合法的 reset。retained pools、待销毁 pools 和其 BO 都参与同一物理预算，逻辑 pool bytes 只用于选择淘汰对象。

已被 worker 摘出的 pool 不再允许重用；只在 pool 全部 command buffer 没有 pending job、无录制者时销毁。worker 持有 device 的底层 Vulkan 所有权到最后一次销毁，device teardown 先停止新任务，再 drain worker，最后销毁 VkDevice，避免 `Arc` 环形保留。临界压力可以加快调度，完成处理线程仍只移交记录。

## 10. CPU map、清零与关闭

V2 CPU map 由 driver 创建并持有 `IOMemoryMap`，返回实际 VA 和 map_id；只有实际 unmap 成功后才解除该引用。完整 BO cache 内保留 map_id，且 task、连接 generation、cache mode、capacity 和放置地址仍匹配时可直接复用 CPU map。申请新的 placed address 时，先安全删除旧 map，再创建新 map。CPU alias 属性变化由 submit/coherency 方案的 generation 记录处理。

同一连接内复用完整私有 BO不跨保护边界，Vulkan 未初始化内容语义允许保留旧 bytes；具体调用者要求零值时清其明确范围。kernel backing 进入全局可供其他 client 使用的队列前必须完成全 capacity 清零，包含向上分档后的尾部，清零完成前仅在 scrub 队列中。压力下准备真正销毁的 backing 可以直接还给 XNU；它不会成为另一个 client 的可见对象。

新 `IOBufferMemoryDescriptor` 已经通过 XNU `KMA_ZERO` 和自动 prepare 建立 storage；现有 driver 再 `prepare` 并 `bzero` 的路径应按实际工厂契约消除重复工作。新 descriptor、cache 命中、导入各自记录 prepared 所有权，严格平衡 complete。导入内存保持所属 IOSurface/VM object 的引用与内容，不入全局清零 cache。

close 先禁止新申请/提交，等待已发布 job 的对象 fence，删除该 client 的 CPU mappings，再把对象移交服务回收。全局 backing cache 归设备服务所有，单 client close 不再调用全池 trim。当前入口为 `NativeAdrenoUserClient.cpp:147` `close`，其中 `:152` 无条件调用 `trimBackingCache`；应由明确 close reason 采样先量化影响。

正常 unmap/属性恢复/complete 失败时，保留该对象的剩余所有权、VA 和预算到恢复流程。硬件 reset 只有在停止访问旧地址并完成 SMMU 状态恢复后，才可解除旧 epoch 的 GPU 引用。quarantine 中的对象不参与 bucket 查找、pressure 可释放量或成功释放统计。

## 11. 实施边界与有界验收计数

本轮 kernel 诊断源码已加入 `NativeAdrenoMemory.h/.cpp`：18 个尺寸区间、4 类 lookup miss、13 类 insertion reject、5 类 eviction 的次数/失败/bytes，以及 64 条至少 8 ms 的慢事件。慢事件记录进程名、序号、采样时刻和 storage 属性；查询通过 `NativeAdrenoMemory::copyDiagnostics` 在 resource gate 内拷贝后输出。`trimBackingCache` 的 pressure/retry 调用已传入明确原因，close 保留默认原因。构建、部署及实机结果由协调记录，本文件不把源码完成视作设备结果。

当前轮只补两层缓存原因与慢事件，明确 close/capacity/fence/尺寸分布后，再按上述判据选择第一组修改。V2 resource refs、CPU map 所有权、每 BO fence 与异步释放必须共同维护同一对象存活条件；不能只实现新的 cache admission。

后续保留以下固定计数即可观察效果：

- 每层每 bucket 的 eligible hit rate、miss 原因、淘汰原因、保留 bytes/objects 和 metadata bytes。
- free 请求到实际 fence 完成、完成到 reusable、reclaim ready 到实际 release 的固定对数延迟桶，以及最大事件的尺寸/类型/来源。
- worker 每轮对象数、bytes、耗时及单个不可拆操作超预算次数；allocation/完成处理线程内执行实际 descriptor release 次数应为 0。
- active/pending/cache/reclaiming/quarantine 的总 bytes、credit 拒绝次数、pressure generation、实际回收 bytes，确认队列增长受既有已计费对象约束。
- command pool 的创建、复用、reset、交给 worker、实际销毁次数及各项实际 capacity bytes。
- 真实最后 fence 已完成但仍因全局最后 fence 等待的次数；V2 路径应为 0。map_id 无效仍命中、跨 client 未清零入池和未 quiescent 重用次数应为 0。

本设计没有增加测试或开展额外实验。收益以正常交互时保存的有界记录判断，所有阈值和预算更改均记录对应构建版本。

## 12. 55 的缓存原因与 64 MiB block 生命周期

### 12.1 已保存的运行证据

55 使用 `system-gpu-bo-profile-55`，固定输入记录在 `logs/gpu-bo-55-source-revisions.json`：设备 `4942c40e57d091c8533935d0bad52ad691c5c265`，core `65a37862ac62d7763a42ef6567ed0c2a3d02c708`，Adreno6xx `a7cc54003bb0a680fffd7dc94f38440d07572f95`；Mesa 和 Metal 基准仍是第 3 节所列提交。原始记录为 `logs/t733-usb-live/t733-20261026T183119.124446Z.raw`，首次完整诊断从该文件 4775 行附近开始。该轮部分长文本相邻输出被拼接，累计字段及保留下来的慢记录仍提供以下事实。

| 观察窗口 | 实际记录 |
| --- | --- |
| 首次 kernel 汇总 | object-limit eviction 13,343，client-close 16；峰值 64 个对象、约 2.62 MiB。小 backing 的淘汰主要由对象数上限触发。 |
| 首次 backboardd Turnip 汇总 | request 19,814，hit 3,802；miss empty 5,578、size 9,346、flags 810、compatible-pending 278。 |
| 同一汇总的申请来源 | `cmd cs` 5,603、`draw sub cs` 5,500、`draw cs` 5,276、`tile store cs` 2,437，合计占 request 的 94.96%。这些是命令存储，和 runtime 的资源内存 block 分开分析。 |
| 同一汇总的 64 MiB 档 | request 23，hit 10，miss 13，compatible-pending 0；requested bytes 为 1,543,503,872。该数是多次申请尺寸之和，包含缓存命中，不能当作同时驻留量。 |
| kernel 慢记录 | 64 MiB user-owned allocation 的若干样本为 40-46 ms，owned descriptor release 为 19-26 ms；记录标出 40-bit、16,384 spans 和调用进程。另有其他尺寸慢样本，不能把全部释放长尾都归到 64 MiB。 |
| 后续 170.469 秒增量 | request 6,246，hit 1,050，miss 5,196，命中率 16.81%；miss size 4,061、flags 733、empty 323、pending 79。kernel object-limit eviction 累计升至 17,961，close 仍为 16。 |

这两个窗口确认小 BO 的低复用持续存在，且相邻观察间 object-limit eviction 仍增加。Turnip 的 size miss 表示缓存中没有相同尺寸的匹配候选；它本身没有记录相邻尺寸的空闲容量，因而尚不能量化改用分档后能增加多少命中。64 MiB 档已经有 10 次完整 BO 命中，说明这一层能够实际复用大块；该档 pending 计数为 0，当前窗口的大块 miss 不由已有兼容对象等待 fence 解释。

### 12.2 runtime 如何建立和释放 64 MiB block

以下行号均指 `sources/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/memory.rs`。 [对应基准源码](https://github.com/steelbrain/metal2vulkan/blob/720ef1ce9ae2a2068999a8675717418b9416283e/crates/metal2vulkan-runtime/src/memory.rs)

- `BLOCK_BYTES:52` 取 64 MiB，`Memory::new:692` 再受 device 最大 allocation size 限制。`place:1376-1377` 确定需要新建时的尺寸：普通小于等于半块的资源采用这一整块尺寸，超过半块或 `Fit::Apart` 采用资源自身尺寸。`Fit::Pooled` 在真正新建前仍会尝试已有池块，包含超过半块但仍能放入已有空间的申请。一个 64 MiB BO 因此可能是池块，也可能是恰好该尺寸的 dedicated resource，现有 native BO 大小不能单独区分它们。
- `Memory::pool_at:751` 按 `(Holds::Buffers 或 Holds::Images, memory_type_index)` 分池。`Memory::kept:844-874` 将 Private/WriteCombined/Shared 的 buffer 类型去重，另取一个 image 类型；三个 memory class 并不必然产生三份 buffer 池。`keep_room` 对每个选中池要求的是至少 32 MiB 的连续可放置空间，和“保留一个完全空块”是两个不同条件。
- `Pool::place:292-303` 按块顺序执行 first fit；`has_room:333-335` 使用这个池曾见过的最大 alignment，检查每块最长可用区间。底层 `range.rs:29` 分配对齐区间，`:95` 合并相邻 free，`:126` 求最长对齐后区间。总 free bytes 足够而最长区间小于 32 MiB 时，`keep_room` 仍会申请新块。
- `BlockMaker::make:1133-1208` 建立整块 `VkDeviceMemory`，host-visible 类型整块 map；需要设备地址时还建立覆盖该块的 buffer 并绑定。这里的 64 MiB 建立成本由一个小 placement 触发也会完整发生。
- `Pool::release:354-370` 已经保留每个池的一块完全空闲的非 dedicated block；第二块变空时把该块取出释放。dedicated block 在最后 placement 释放后取出。现有实现不是“每块一空就释放”，不能把增加最基本的一块保留作为尚未实现的修复。
- `Memory::release_range:949-960` 直接调用 `Block::release:458-464`，后者释放地址 buffer 后调用 `vkFreeMemory`。`buffer.rs:449,631` 与 `texture.rs:1129` 等调用者仍持 `shared.memory` 的 mutex，所以这里同步进入 Vulkan 的释放工作会延长其他 resource placement 等待时间。Turnip 随后可以缓存或退休这个 BO；实际 kernel descriptor release 发生在最终淘汰时，runtime 的 `vkFreeMemory` 与 kernel 物理释放不是同一个事件。

`Device::keep_room` 在 `device/mod.rs:634-639` 调用上述函数。`buffer.rs:987` 的 new_buffer、`texture.rs:1413` 的 new_texture、`queue.rs:226`、`heap.rs:468`、`pipeline/render.rs:1500` 等创建路径都会调用它；因此大块增长可在创建小 buffer、texture 或 pipeline 时发生。源码明确给出了这些入口，55 尚未为每次 block 建立保存对应入口。

### 12.3 并发增长与立即丢弃：代码已经证明的行为

`keep_room:1409-1418` 在 mutex 内生成缺少 32 MiB 空间的池列表，随后释放 mutex；`:1422` 在锁外创建 block；`:1425-1432` 回锁检查池是否已有足够空间。若已有，释放 mutex 后立即释放刚建好的、从未放置资源的 block。下面是现有操作允许的具体顺序，不是本次已观测的线程记录：

1. 两个创建线程同时看到同一个 buffer 池缺少 32 MiB 连续空间。
2. 两者各自在锁外创建一个 64 MiB block，代码没有给该池登记“已有线程正在增长”。
3. 第一个线程把 block 加入池。第二个线程回锁发现空间已经足够，将自己刚建好的 block 释放。

同样的“创建后释放”也可能由并发普通 `place` 补足空间，或现有资源 free 合并出足够空间触发。`keep_room` 的缺少空间列表还可能包含多个池，排在后面的池在真正调用 make 前也不会重新检查一次。因此即便同一线程只在处理一份旧列表，列表中的较后池也可能已经被其他线程补足。

`place:1381-1392` 的行为不同：先尝试池内 placement，失败后锁外 make，成功回锁后直接 `add_block` 并放入本次资源，没有再次尝试已有块。同一池的两个并发 miss 因而可以各自建立 64 MiB，随后都留在池内供各自 resource 使用。它本身没有“回锁发现重复就立即释放新块”的分支；多建块随后变空时，`Pool::release` 的一块空闲保留规则才可能释放其中一块。

另一个确定的统计边界是 `Memory::allocated` 只在 `add_block:819` 增加。`keep_room` 丢弃新品的分支在 `:1432`，发生于 `:1435` 的 add 之前；这种已经执行过 `vkAllocateMemory` 的重复建立不会进入 `Memory::allocated`，也不会出现在只统计仍存活块的 `census`。旧的 allocated/census 不能用来排除这条路径。

### 12.4 当前能归因到哪里

直接证据有三层：55 记录到 64 MiB BO 的创建/复用以及最终物理释放耗时；源码确认 runtime 默认池块为 64 MiB；源码确认 keep_room 存在创建后立即释放路径、place 存在同池并发重复增长路径。现有记录没有 block ID、pool 类型、增长原因、创建时正在增长的线程数或 never-used release 原因，因此 13 次大块 miss 中有多少属于各路径仍需对应字段。

除并发增长外，顺序执行的资源数量增长、资源释放后再次达到更高使用量、32 MiB 连续空间要求与分散 free、完全空块只留一份，以及专用大资源，都可以产生新的 64 MiB 申请。64 MiB backing 超过 kernel 单对象缓存上限 16 MiB，是当前源码的确定行为：Turnip 一旦最终淘汰它，kernel backing cache 不会继续保留，后续缺少完整 BO 命中时要重新分配。小命令 BO 和大块共用 Turnip 完整 BO LRU；大块成为最旧项且对象数达到限制时，新归还的小 BO 也能将它挤出。55 尚未记录“被哪一类新对象挤出”的归因。

因此小命令 BO 的数量问题与 runtime 大块的增长/保留问题需要分别处理。把小 BO 缓存加大可以降低命令存储周转；它不能消除 runtime 已经走到的重复 `vkAllocateMemory`。大块在 runtime 中继续存活，可以同时保留 placement 管理、CPU map、设备地址 buffer 和下层 native BO。

### 12.5 选定的成熟机制与本项目接入方式

Vulkan Memory Allocator v3.3.0 提供直接参照：`VmaBlockVector::Allocate` 在该池的锁内查找并执行 `AllocatePage`/建块；`Free` 在锁内摘除空块，锁外销毁，结合预算与最低块数决定是否保留空块。`VmaPoolCreateInfo` 同时提供 blockSize、minBlockCount 和 maxBlockCount。 [v3.3.0 实现，Allocate 10387、Free 10622 行](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/v3.3.0/include/vk_mem_alloc.h#L10387)，[池配置](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/struct_vma_pool_create_info.html)。

本机选定沿用当前 range allocator，接入上述“同池增长有唯一执行者、明确保留量、锁外销毁”的所有权关系，不在此轮替换整个 Vulkan allocator。具体结构如下：

1. 每个 `(Holds, memory_type_index)` 维护增长状态、完成 generation 和全局已预留 bytes。普通 placement 先在短 metadata 锁内查找；需新 block 时，在不持 `Memory` mutex 的状态下取得该池增长权，取得后重新检查现有空间。`keep_room` 遇到该池已有增长者直接结束本池预留工作，避免重复建立；真正需要资源的 placement 等待增长完成事件，醒来后重新查找。其他池和能够命中已有空间的申请继续运行。
2. 增长者先取得容量预算，再锁外执行 Vulkan 建块，回短锁发布新块、归还 reservation 并通知等待者；失败路径同样结束增长状态和通知。预留增长 bytes 和真正 live bytes 一起参加预算。这个做法保留了 runtime 把慢 driver call 移出全局 mutex 的要求；“短 metadata 锁加每池增长状态”是本项目的适配设计，VMA 的具体实现使用池锁覆盖增长。
3. 正常 free 在锁内归还 range，并决定保留或摘除 block；摘下的 block 交给独立回收任务，锁外释放。保留一空块的策略已存在，首先保留这一基线。只有按池记录证明反复释放后短期重新增长，才为该热池增加有预算的保留量；每个池的下限、最大块数和全设备 bytes 一起生效。VMA 的 minBlockCount 约束总块数，不能把它等同于“始终额外保持这么多空块”。
4. `keep_room` 的 32 MiB 目标与 block 保留量共用一次容量决定。多个创建入口只声明所需连续空间，不能各自创建同一份预留。仍因并发 free 使新建块成为多余时，按剩余 idle budget 保留或交给回收任务；不在 creation caller 内做慢销毁。发生 pressure 后主动降低预留，停止没有实际需求的增长。
5. 本文第 6 节全局 idle budget 必须包含 runtime 的完全空 block。一个 buffer 空块和一个 image 空块已经占 128 MiB，不能另外无条件叠加 Turnip 与 kernel 的完整预算。部分使用 block 仍按 committed bytes 计费，其 free ranges 单列供碎片分析。runtime 持有空块时，下层完整 BO cache 尚未持有它，避免重复物理计数。

现阶段不凭 23 次累计申请改变 64 MiB block 大小。若后续按池记录显示长期只用少量 bytes，则依据真实 placement 尺寸和峰值选择较小 block 或有上限的增长档位；资源需要的 alignment、buffer/image 分池和 dedicated 条件仍保持明确。

### 12.6 归因所需的最小有界记录

在现有慢记录之外，runtime 只需固定计数加一个短环记录 block 变化：`pool_kind/type_index`、单调 block ID、`reason`（普通 placement、keep_room、dedicated）、requested size/alignment、block capacity、增长开始/完成时刻、当时 longest free/total free、live/empty/growing blocks、是否成功发布、从未被 placement 使用即释放、释放原因。计数单列 keep_room 已有增长者、建好后已有空间、place 建好时已有可用空间，以及空块释放后再次增长。

这些记录不需要按每个 resource 输出，也不需要继续拆分 XNU allocation。它们把已确认的 40-46 ms 冷分配和 19-26 ms 物理释放，关联到具体的 runtime 操作；实现与记录在后续统一源码变更中完成。本补充只完成源码研究和设计，没有修改运行时代码、执行测试或提交。
