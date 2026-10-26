# SM-T733 pipeline 创建、缓存与预热设计

日期：2026-10-27。本文是架构方案，未修改实现、构建或部署。

选定方案：在 Turnip 上使用 Mesa 原生 shader 对象缓存及磁盘缓存；由 metal2vulkan 的设备对象保存有界的完整 pipeline、合并相同创建请求，并调度编译；由 Metal 前端实现真正异步的 completion-handler 入口。预热重放实际使用的 descriptor 和 draw key。同步 Metal 创建继续返回完成的 pipeline 或明确错误。

## 1. 输入、已知问题与目标

54 的实测为 122 次 Metal pipeline 创建合计 11,635 ms、最大 3,155 ms；嵌套 Vulkan 创建合计 8,141 ms、最大 2,173 ms。最大值来自开机至查询的累计记录，不能把两个最大值相减当成某一次翻译耗时，也不能据此把某个手势卡顿归因于编译。完整来源见 [54 采样及源码调查](../../../notes/GPU_BO_MAP_COST_AUDIT_2026-10-27.md#source-investigation-of-the-remaining-stalls)。

54 的实际用户态源码是 `sources/UniXNU/.cache/gpu-reference/metal2vulkan` 与 `sources/UniXNU/.cache/mesa-work`。前者固定 `720ef1ce9ae2a2068999a8675717418b9416283e`，后者基点为 `85fda3b36405bcaea8bffc64aa4f2651dc90770d`，另应用本地补丁。本文所有本地行号来自这组源码；`out/` 中旧 Mesa 副本不作为此包的依据。

| 已定位流程 | 对应源码 | 本设计解决的工作 |
| --- | --- | --- |
| 同步创建与 completion-handler 形式都在调用线程编译 | `NativeBridgeMetalFrontend.mm:1440-1472` | 两种 API 共用创建结果，只有同步形式等待；异步形式入队并返回 |
| 创建收集 neutral、预测首次 draw、depth copy pipelines，最终等待全部任务 | `pipeline/render.rs:1378-1382,2321-2383` | 完整保留同步成功/错误契约；真实预热、复用和去重减少需要执行的创建 |
| 每个 Vulkan making 读文件、创建临时 VkPipelineCache、创建 pipeline、导出缓存、写文件、销毁缓存 | `shader_cache/driver.rs:266-356` | Turnip 改用设备内部缓存与 Mesa 按对象持久化，去掉每次 making 的整个外层缓存循环 |
| Mesa shader disk cache 在构建中关闭 | `scripts/build-turnip-darwin.py:43-47` | 启用已经实现的 Mesa 持久化组件，并接入可写目录、身份和有界保存队列 |
| 磁盘命中仍会反序列化并上传 shader | `tu_shader.cc:3156-3200` | 设备内命中复用现有 `tu_shader`；完整 pipeline 命中连 pipeline 状态分配一起省去 |
| 当前预热以默认常量和 BGRA8 推测配对，并用 path/size/mtime 标记完成 | `PrimeShaders.h:116-134,173-176` | 记录真实请求及版本身份，只标记实际成功创建并持久化的具体 key |

`V cache hit120` 是翻译产物命中。`ShaderCacheStatistics` 已把它与 `driver_pipeline_hits` 分开，见 `shader_cache/mod.rs:138-186`。采用内部 Mesa 缓存后，Vulkan 的 application-cache-hit 标志不能代表全部命中来源；需要单列内存 shader 命中、磁盘命中、实际编译和完整 pipeline 命中。

## 2. 已读的成熟实现及采用边界

下面 Mesa 链接固定到上述基点，方便实施者逐行取得相同版本。源码通过本地 checkout 和该 checkout 配置的 Chromium Mesa 镜像读取；没有切换或修改源码。固定版本的比较不是对官方 main 当前 HEAD 的声明。

| 组件与精确入口 | 实际行为 | 本机采用方式 |
| --- | --- | --- |
| [Mesa Vulkan cache](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/vulkan/runtime/vk_pipeline_cache.c#L290)，290-422 | 表锁内查找并取得引用；磁盘读取、反序列化在表锁外；插入时合并重复对象 | 直接保留查找、引用、插入和序列化格式 |
| 同文件 187-206、425-451、725-804 | weak-owner 的最后引用释放时移出表；新增对象目前先在创建线程 serialize，再向 disk_cache 排队；GetPipelineCacheData 持表锁遍历全部对象 | 复用引用规则；新增有界的后台对象序列化接入；生产创建不导出整份 VkPipelineCache |
| [RADV device](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/amd/vulkan/radv_device.c#L531)，531-540 | 内部 `mem_cache` 以 `weak_ref=true` 创建 | 将同一已有选项用于 Turnip 内部设备缓存，避免该缓存永久强持全部已编译 shader |
| [RADV pipeline cache](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/amd/vulkan/radv_pipeline_cache.c#L380)，380-481 | 从共享缓存取 shader/pipeline 对象；pipeline 持引用；同 key 插入复用既存对象 | 采用 pipeline 持有编译对象、设备表索引它们的所有权关系 |
| [ANV pipeline cache](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/intel/vulkan/anv_pipeline_cache.c#L238)，238-315 | 未传应用缓存时使用设备 `mem_cache`；kernel/NIR 经 Mesa 通用缓存取得和保存 | 直接沿用该入口形态；ANV 的 Intel kernel heap 不移植到 Adreno |
| [Turnip pipeline](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/freedreno/vulkan/tu_pipeline.cc)，`tu_pipeline_builder_compile_shaders`、`tu_CreateGraphicsPipelines`、`tu_CreateComputePipelines` | shader key 包含 shader、布局、编译选项和相应状态；未传应用缓存时选择 `dev->mem_cache` | 直接使用现有 shader key、IR3 编译、上传和 GPU state 生成。54 本地相应行为在 1951-2052、4847、4921 |
| [Mesa disk cache](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/util/disk_cache.c#L565)，565-633；[磁盘写入](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/util/disk_cache_os.c#L771)，771-875 | 写任务拥有复制的或转移来的字节；压缩与文件写入在 worker；同目录临时文件加非阻塞文件锁，完成后 rename；读取仍是同步 | 直接复用格式、压缩、原子写入和多进程写竞争处理；队列接纳、序列化位置和目录由本机适配 |
| [Mesa util_queue](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/util/u_queue.c#L285)，285-301、545-632 | 锁内取任务，锁外执行、signal、cleanup；满队列允许扩张至其字节条件，否则等待 | 采用 worker、fence 和 cleanup 程序；为可选缓存保存增加非阻塞接纳，不能直接沿用满队列等待 |
| [Zink 编译](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/gallium/drivers/zink/zink_program.c#L930)，930-994、1424-1552、1833-1857 | 已有可用 pipeline 时后台编译优化版本；完成后替换；销毁前等待编译 fence；不支持当前变体时仍同步等待 | 采用任务引用、完成条件和安全发布。这里不引入另一套可改变输出的占位 pipeline |
| [Zink 保存](https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/gallium/drivers/zink/zink_screen.c#L380)，380-423、1610-1621 | worker 取 pipeline cache 数据并转移给 disk_cache；退出先 finish 队列 | 采用保存独立于创建返回、按所有者停机的关系。Turnip 选择按 shader 对象保存，省去整个 cache 的遍历 |
| [metal2vulkan libraries](https://github.com/steelbrain/metal2vulkan/blob/720ef1ce9ae2a2068999a8675717418b9416283e/crates/metal2vulkan-runtime/src/pipeline/libraries.rs#L91)，91-175 | `Waiting/Making/Made`、Condvar、失败释放 claim、pipeline 与 command buffer 持引用 | 扩展现有去重和所有权实现到设备级完整创建请求，保持现有 GPL 分片去重 |

Mesa 的 `disk_cache.c:90-93` 当前配置 32 个初始槽和 4 个 worker，并允许扩张；`u_queue.c:580-613` 在满载时仍可能阻塞调用线程。`vk_pipeline_cache.c:442` 的 serialize 也发生在入队前。因此只把 `shader-cache=disabled` 改成 enabled 不能完成本方案。

## 3. 完整调用与所有权

```text
Metal API
  -> immutable request and exact descriptor key
  -> DeviceShared pending/ready table
       -> existing ready result: create caller's Metal wrapper
       -> existing pending result: join that result
       -> new result: process compile scheduler
            -> translator artifacts
            -> shared GPL libraries and exact draw pipelines
            -> vkCreate*Pipelines(NULL cache) on Turnip
                 -> device weak shader cache
                 -> Mesa disk cache on memory miss
                 -> IR3 compile and shader upload on disk miss
                 -> bounded optional object-save work
  -> synchronous return OR completion callback

command buffer -> exact pipeline -> shader/library references
ready LRU      -> optional retained pipeline references
save task      -> referenced immutable shader + live device ownership
```

`DeviceShared` 拥有 pending/ready 表、pipeline 保留预算、GPL libraries 和 Vulkan 设备。进程编译调度器只负责安排任务与并发额度，任务持有设备、不可变请求和完成状态；它不拥有另一个独立的永久 pipeline 缓存。Turnip 的内部 `mem_cache` 保存弱索引，实际 shader 由 pipeline、已接纳的保存任务及其他使用者持有。

同一 runtime 还会运行于其他 Vulkan 驱动。通过已识别的 Native Turnip 后端能力选择此策略，并把策略版本放入设备身份；其他后端继续走其现有 cache 实现。不能仅用 `target_vendor=apple` 选择，这个条件目前也控制面向 MoltenVK 的四并发预算。

### 3.1 创建契约与请求快照

[Metal 同步创建](https://developer.apple.com/documentation/metal/mtldevice/makerenderpipelinestate(descriptor:)) 返回已创建对象或错误；[completion-handler 入口](https://developer.apple.com/documentation/metal/mtldevice/makerenderpipelinestate(descriptor:completionhandler:)) 在完成后回调。两者调用同一个结果产生程序。

同步入口在未命中时等待该请求，成功才返回 state。此阶段保留 `make_neutrals`、`make_at_creation`、`make_stand_in_copies` 的完成范围以及 full join。调整预测变体集合应是独立的、具有等价编译语义的工作；不能先删掉 join，再把原来的编译和失败移到第一帧。

异步入口先做有界的 descriptor 快照和参数检查，保留函数、常量字节、动态库、binary archive 及设备引用，复制 completion block，再向同一调度器提交请求。返回之后 worker 执行创建，完成队列调用 handler 恰好一次。即使命中 ready cache，handler 也走完成队列，避免命中与未命中产生重入差异。调用 handler 时不持缓存锁、编译额度或设备调度锁；每个 worker 使用自己的 autorelease pool。

Objective-C descriptor 的可变子对象需要复制成 runtime 的 owned 字段，不能只 retain descriptor 后在 worker 读取。`NmbPipelineOptions` 中 archive 指针数组、descriptor 内的函数句柄以及 TLS `nmb_last_error()` 内容必须分别保留或复制；错误由工作线程在返回桥接层时立即转成该任务的 owned error。reflection 和 Metal wrapper 的 label 也属于各请求的结果。相同底层 pipeline 可共享，但不能让两个可变 Metal wrapper 共用 label 属性。

现有 `pipeline/content_keyed.rs:30-42,48-55` 只允许 runtime 认定的 program-call 更新共享模块和 library 表。异步入口的 worker 必须进入与原同步入口相同的受管 API 调用作用域，再安排内部编译任务；不能让整次 Metal API 直接被标为 pool piece，导致每个 worker 都创建仅自己可见的模块和 library。所有表的变更仍遵守现有锁与 owned-object 销毁程序。

### 3.2 相同请求只执行一次

设备表的完整 key 包含：策略/格式版本，库与函数的稳定身份，函数常量的类型及原始字节，完整 vertex descriptor，color/depth/stencil 格式与 blend/write mask，sample count/alpha-to-coverage/rasterization/topology，tessellation 或 mesh 参数，buffer mutability，linked functions，支持的创建选项及 reflection 要求，以及影响编译的 profile、feature、layer settings。GPU draw 变体另使用 runtime 现有的完整 draw key。不得用 label、函数名或文件 path 单独代替内容身份。

key 按显式字段和版本化编码生成；内存表在索引命中后比较完整规范化内容。binary archive 的身份、内容 generation 及 fail-on-archive-miss 选项也进入创建请求身份，确保缓存复用仍返回该请求应有的成功或错误。现有 `Making::text` 使用 Debug 输出命名磁盘记录，可保留为旧记录格式；新的持久化 descriptor 不使用 Debug 文本充当长期稳定格式。

表项状态为 Queued、Running、Ready、Failed。第一位请求者安装任务，后续请求者共享完成状态；同步等待者加入后提升尚未开始任务的优先级，异步请求只追加自己的 callback。翻译函数、完整 pipeline、GPL part 各自按对应 key 去重，避免只去重某一个 Metal state 内的 draw。

完成时在短表锁内发布 result 并摘下等待者，锁外唤醒或回调。确定性参数/编译错误可保留在本进程的有界失败表中；OOM、设备丢失、线程启动失败等瞬时错误不写磁盘成功记录。已加入同一次任务的请求者收到同一次结果；下一次请求是否重试由失败类别和退避时间决定，不能让每个被唤醒的 waiter 立刻重新编译。

Turnip 通用 cache 的插入去重发生在编译之后。若不同完整 pipeline 同时使用相同 shader 编译输入，仍可能各编译一次。为 `tu_pipeline_builder_compile_shaders` 的完整 shader 组合 key 增加设备级的进行中记录：拥有者执行整组查找/编译/插入，等待者醒来后重查各 stage。此记录只覆盖当前函数的一次非递归编译，不在持锁期间调用编译，不跨入另一个尚未运行的 pipeline 任务。失败发布同一 VkResult，并从进行中表退场。使用完整组合 key，避免分别锁多个 stage 后彼此等待。

## 4. 有界调度、保存与内存

以下数值是 SM-T733 首轮实现的配置预算，不是已测最优值。配置应集中定义并进入日志；运行时不能悄悄扩成无限任务队列。

| 资源 | 初始限制 | 到达限制后的操作 |
| --- | --- | --- |
| 进程内实际编译 | 2 个任务；预热最多占 1 个 | 请求按已有 urgency 顺序等待；已有同步请求优先于纯预热 |
| 等待编译的唯一请求 | 64 项、合计 16 MiB owned 请求元数据 | 丢弃尚未开始的纯预热；同步请求等待接纳；异步请求无容量时完成为明确资源不足错误 |
| completion 等待者 | 每进程 256 个受管 callback 槽 | 新请求不占无限队列；资源不足在本次调用报告 completion 错误，正常已接纳请求仍只从完成队列回调 |
| runtime 额外保留的完整 pipeline/GPL/模块 | 合计 64 MiB，完整 pipeline 最多 256 项 | 移除无人使用的 LRU 保留引用；新结果太大时只交给当前调用者 |
| 保存任务保留的 shader、字节与压缩空间 | 合计 16 MiB，最多 32 项；单条落盘数据最多 4 MiB | 可选保存跳过并计数；创建成功照常返回，下一次遇到该对象可再申请保存 |
| 磁盘 shader 数据 | 同一用户同一身份目录 256 MiB | Mesa 原有逐项回收；后台限制过期身份目录总量 |
| descriptor 预热清单 | 2048 项、16 MiB | 保留近期成功且确实被 draw/dispatch 使用的项目 |

进程额度覆盖所有 `VkDevice`，也覆盖入口创建、现有 `pipeline_pool`、draw 变体和 helper 创建。不能为异步前端再开一组四线程，并让它们继续各调用四线程 pool。任务执行到内部依赖尚未完成时，把 continuation 留在依赖完成状态上，释放 worker 的执行资格；不得在全部 worker 内等待只能由同一批 worker 执行的子任务。已有 `budget.rs` 的 urgency、提升优先级和不持额度等待其他 making 的规则继续使用。

预热在后台 QoS 执行，交互创建在适当的用户发起 QoS 执行；现有 pool 会把 macOS 工作线程提升到 user-initiated，接入时必须让任务携带 urgency 并选择实际 QoS。两并发是本机起始配置，不复用 `APPLE_TURNS=4` 对 MoltenVK 的历史测量理由。

额外保留预算需要真实对象成本。pipeline 状态的 `tu_pipeline::bo/cs`、各 `tu_shader::bo`、private-memory BO、CPU variant/NIR 及 runtime owned 字节都必须纳入。通过本机 Turnip 与 runtime 的版本化私有接口返回 pipeline 的保留成本；同一底层对象出现在多个 pipeline 时允许重复计费，使预算偏大。suballocator 按使其存活的完整 backing BO 计费，不能只算 shader 指令大小。其他后端无法提供成本时不加入这项可选长期保留。序列化文件长度不能宣称为实际常驻内存。

应用持有的 pipeline 和未完成 command buffer 使用的对象按正确性所需寿命存活。上表限制的是缓存与后台工作额外持有的引用；应用主动保留的大量对象要在统计中单列。编译器执行中的临时内存受任务并发和分配失败处理约束，不把它混入已计算的缓存容量。内存压力到来后取消未开始预热、释放可选 LRU、停止保存接纳；当前必要编译在成功或真实 OOM 后完成其请求。

### 4.1 Mesa 保存队列的具体接入

Turnip 配置启用 `shader-cache`，继续使用 Mesa `vk_pipeline_cache_object_ops::serialize/deserialize`、disk cache key、压缩和多文件原子写入。新增接入放在对象成功插入之后：

1. 在设备仍活着时为不可变 shader 对象保留引用，取得其已维护的 owned 字节数及保守序列化大小上界，尝试一次性保留队列预算。上界超过单项限制时直接跳过保存。
2. 队列接纳只持短锁；满载立即返回跳过结果。不能在 pipeline 创建线程等待 disk queue 空位，也不能在这里实际 serialize 来计算大小。
3. 后台 worker 在不持 pipeline-cache 表锁的情况下 serialize 到具有固定容量上限的 blob。Turnip 编译完成后的 variant、const state 及其保存字段视为不可变；只允许明确满足这一约束的 object ops 使用此通路。
4. 所有权从 shader 引用转成序列化字节后才释放前者；字节经 `disk_cache_put_nocopy` 的等价所有权转移进入写队列。两个阶段共用接纳预算和在途计数，不能在两个队列间短暂漏计。
5. disk writer 使用现有压缩、临时文件锁与 rename 程序。压缩输入、输出及写任务都包含在预算中；任何分配、压缩、文件锁或写入失败释放全部任务资源，记录原因，保留已成功创建的 pipeline。

为此给 util_queue/disk_cache 增加受预算约束的非阻塞接纳入口，并让已有调用保持原行为；为 Vulkan 对象保存增加持有对象直到 serialize 完成的任务类型。worker 框架、序列化格式和文件程序是直接复用，接纳与生命周期是本机需要补齐的适配。任务失败后不设置永久 saved 标志；同 key 同时只有一份保存任务，完成状态区分 Pending、Persisted、RetryableFailure。

逻辑设备必须拥有其序列化在途计数。物理设备拥有共享磁盘 writer，但逻辑设备销毁前等待自己持有 Vulkan 对象的任务退出；转成纯 owned bytes 的写任务可随磁盘 owner 继续完成。不能让 disk_cache 的物理设备寿命掩盖已经销毁的 VkDevice。

### 4.2 异步入口的容量边界

任意数量的调用者可以持续提交异步请求，而 handler 也可能永久不返回；有限内存无法同时保证无限接纳和所有调用都立即返回。实现必须把上表接纳边界写成正式行为：正常接纳的请求异步完成；达到 callback/元数据容量时，本次调用不保留新任务并用明确资源不足错误调用其 handler。该拒绝路径只构造错误并交付，不做编译。统计单列这类拒绝；正常交互样本的验收要求为零。线程创建失败采用同一错误完成程序，不把整个编译退回 GUI 调用线程。

## 5. Vulkan 同步、对象销毁与失效

本方案在 Native Turnip 生产创建中传 `VK_NULL_HANDLE` 的应用缓存，进入驱动 `mem_cache`。该内部缓存使用已有 mutex 和弱引用操作。对应用显式创建的 VkPipelineCache 或其他后端，继续遵守其原有外部同步约定。

如果某条兼容路径仍持有应用 VkPipelineCache，默认不设置 `EXTERNALLY_SYNCHRONIZED_BIT`，以保留实现的同步。设置该位的对象由唯一 owner 串行执行所有修改操作。[缓存标志定义](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheCreateFlagBits.html)。`vkMergePipelineCaches` 的目标缓存需要外部同步，除非确实创建了其专用的内部 merge 同步标志；不能假定普通内部锁就免除了该要求。[merge 规则](https://docs.vulkan.org/refpages/latest/refpages/source/vkMergePipelineCaches.html)。

`vkGetPipelineCacheData` 的两次长度/内容读取之间，内容可能变化，返回 `VK_INCOMPLETE` 是有效结果。[数据读取规则](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPipelineCacheData.html)。兼容导出在后台执行，cache 对象有独立的访问寿命锁；限制缓冲容量和重试次数，不在编译线程反复扩容。通用 Vulkan blob 不手工拼接，多进程也不互相共享 VkPipeline/VkPipelineCache 句柄。

完整 pipeline 只有在所有已提交的引用它的命令完成后才能销毁，而且 host 对其销毁访问需要外部同步。[销毁规则](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyPipeline.html)。继续由 command buffer 持有 pipeline 引用至完成；LRU eviction 只放弃缓存的一份引用。真正销毁通过原有 release/retirement owner，不能在 LRU 表锁里析构一批 pipeline。GPL library、模块、layout 和 sampler 也必须存活到最后一个使用者退出。

停机顺序为：停止新接纳，取消未开始的可选工作，发布已取消请求的完成结果，等待正在执行的编译和持 Vulkan 对象的保存任务结束，处理正常设备关闭所需的 command-buffer 完成及引用释放，释放 ready/LRU 和 libraries，最后销毁 VkDevice。callback 不在上述 owner 锁内运行，回调重入关闭时由外部完成状态安排收尾。设备丢失时停止新编译和预热，给等待请求发布同一个设备错误，阻止把未完成对象写为成功缓存；后续新设备创建新表和新的设备 generation。

## 6. 跨进程、身份与缓存目录

每个进程拥有自己的设备和活对象表。磁盘文件可在相同用户、相同编译身份下复用。采用 Mesa 按对象文件和原子 rename，自然避免多个进程覆写一个总 VkPipelineCache 文件丢掉彼此新增内容。同一 key 的非阻塞文件锁冲突允许当前 writer 放弃，该次编译仍成功。

driver 身份包括 Vulkan vendorID、deviceID、pipelineCacheUUID、驱动版本、Mesa/IR3 固定 revision、本地编译相关补丁与构建选项的版本号、实际启用的编译特性。`tu_device_get_cache_uuid` 已包含 build identity、chip id 与 driver flags，见 `tu_device.cc:60-105`。本机 Mach-O build-id 支持来自 `ir3-darwin-build-id.patch`，实施时需保证其输入在实际驱动编译改变时更新。记录使用正常源码 revision、不可移动构建 tag 和已有 build identity，不新增通过 hash 文件确认构建的流程。

runtime 翻译产物、driver shader 文件、预热清单分别保存格式版本。翻译与预热身份另包括 translator/runtime revision、Metal 语义转换版本、profile/layer settings、OS build/只读系统映像版本、库身份和 descriptor 编码版本。常量、specialization、dynamic-linking 内容改变会形成不同 key。无稳定磁盘身份的内存库可以在本进程复用；只有完整 owned 输入可被安全持久化且在预算内时才纳入跨进程预热。

路径由当前进程可写的用户缓存根提供，在建立 device 前固定；遵守已存在的 iOS 用户会话和 sandbox 可访问范围。不能用 root 的临时全局目录替代 mobile 用户目录。共享目录的文件权限只允许相同授权身份写入；不同 UID 默认隔离。目录不可写时在首次失败记录原因后使用内存缓存，创建仍可执行；不为每个 pipeline 重复路径探测和错误输出。

读取文件先限制长度、检查格式和身份，再反序列化。保留 Mesa 已有完整性校验与边界检查；拒绝的条目计数并在所属目录后台清理。进程崩溃留下的临时文件由启动时有界清理或后台整理处理。更新软件后只进入新身份目录，后台逐步删除不再使用的旧身份，不能把整棵缓存删除塞进第一次创建。

## 7. 按真实请求预热

现有 `journalRenderPipeline` 已记录常量、attachment、blend 和 vertex 信息，但它同步追加文本文件，且不是版本化完整重放格式。新入口从实际 canonical request 生成有界二进制记录，内存聚合 use count、首次实际 draw、最后使用时间、编译成本和是否成功；后台合并清单。函数名匹配和单 BGRA8 模板不再决定成功覆盖率。

预热有两种实际动作：首次取得相关 MTLLibrary 后，按同身份清单创建其曾用的完整 Metal descriptor；在 runtime 内按保存的具体 draw key 创建确实出现过的变体。遇到不能重放的外部资源、缺少 linked library 或不支持的 descriptor 字段，记录该项原因并继续其他项，不能换成默认值后标记原 key 完成。

初始每次开机最多读取 16 MiB 清单、选择 128 个近期项目、后台运行 30 秒累计 CPU 时间预算。调度器最多运行一个预热编译；下一项目只在没有等待的交互创建且设备没有内存压力时开始。单次 Vulkan 编译不可抢占，超出剩余预算后只完成当前项目，再暂停后续项目。每个项目的历史成本用于入队预算，不以固定 sleep 20 ms 代表预算。

标记分为本进程 Ready 和 Persisted。只有缓存 writer 完成对应文件后才记录 Persisted；重启只凭“曾经预热成功”也仍要允许缓存条目已被回收。清单中的成功记录以完整身份和完整 key 命名，不再以库 path/size/mtime 表示“该库全部完成”。编译中的进程退出只记录 Interrupted，不能像现有 `prime-current` 那样把任何退出都永久判成坏函数；确定性错误才按相同身份和 key 限期跳过。

## 8. 实施顺序及每阶段完成条件

1. 完成一组有界分段计时与 identity/capability 配置：请求快照、翻译内存/磁盘、队列等待、依赖等待、driver shader 内存/磁盘查找、IR3、shader upload、pipeline state 分配、serialize、write。把 Metal API 形式和当前 descriptor key id 关联到同一次事件。此阶段提供证据，不声明已经修复卡顿。
2. 原子接入 Turnip 缓存策略：启用 Mesa disk_cache；内建 mem_cache 使用 RADV 弱引用；runtime 增加有成本上限的完整 pipeline 保留；Native Turnip 停止每 making 的临时 VkPipelineCache 读写；完成保存任务所有权与非阻塞接纳。弱缓存和完整 pipeline 保留一起交付，避免中间版本刚释放 pipeline 就丢掉全部上传结果。
3. 原子接入设备级请求去重与进程级任务预算：连接翻译、GPL、draw、完整创建的所有 call sites，补齐 Turnip 相同 shader 组合的进行中去重、失败唤醒和 teardown。保留同步创建 full join，消除相同请求的并发重复工作。
4. 修改所有相关 Metal completion-handler 入口，接入同一调度器和 owned request。render、compute、函数常量 specialization 的异步形式要一起清点；同步入口使用同一结果状态。完成错误传播、反射结果、回调重入和压力接纳行为。
5. 接入真实 descriptor/draw-key 采集与预算预热，替换当前基于名字配对和全库 done 标记的程序。最终开启策略前提供每层命中与重放覆盖率。

源码修改需同步更新设备固定 revision、构建输入与补丁记录，全部提交并保持工作树干净后由协调者统一构建和部署。本次文档工作没有执行这些步骤。

## 9. 有界观测与验收指标

默认只保存每层计数、固定桶直方图、最大值附属 key id，以及最近 256 个完整创建事件。每项携带 pipeline 类型、同步/异步/预热来源、线程身份和内存压力状态；枚举 key 名称和 descriptor 只在显式导出时进行。正常绘制不逐帧打印，不持续追加无限日志。

以下是实施后的验收目标，由协调者安排正常实机操作采样；本文没有执行测试或逻辑检查，也没有宣称已达到目标。

| 目标 | 可直接判断的记录 |
| --- | --- |
| 相同请求并发合并 | 每个设备 generation 和完整 key 同时只有一个实际编译 owner；joined waiters 单列 |
| 完整 pipeline 命中省掉全部 driver 创建 | 命中事件的 vkCreate 次数为 0；额外 shader upload 次数为 0 |
| 驱动内存命中复用已上传 shader | 命中 shader 的 IR3 与 shader upload 次数均为 0；仍需的新 pipeline state 时间单列 |
| 磁盘命中省掉编译 | 对应 key 的 disk-hit、deserialize、upload 有明确计数，IR3 次数为 0 |
| 保存离开创建路径 | 创建线程的 serialize、GetPipelineCacheData、文件 write/rename 时间均为 0；保存排队满载也不增加创建等待 |
| 异步调用尽快返回 | 正常 descriptor 快照及接纳 p95 低于 1 ms、最大低于 4 ms；本机实际成本超标则按字段复制、锁或分配分别处理 |
| 已保留的同步创建命中 | p95 低于 1 ms、最大低于 4 ms；UI 正常重开期间不出现同 key 重编译 |
| 预算生效 | 唯一请求数、owned bytes、callback 数、保存输入/输出峰值始终在配置内；超限操作有原因计数；正常交互资源拒绝为 0 |
| 真实预热覆盖 | 对清单内同身份的已持久化 key，下一次创建的 driver 编译次数为 0；首次实际 draw 缺少预期变体的次数单列 |
| 退出和失败完成 | 已接纳请求全部进入成功或失败终态；无重复 callback、丢失 waiter、设备销毁后 Vulkan 调用 |

冷启动和暖启动分别统计，至少区分进程内首次使用、磁盘命中首次使用、完整 pipeline 命中以及完全冷编译。线程 wall time、CPU time 和 GPU 等待分开；这组 CPU 软件计时不声称提供 GPU shader 利用率。若后续要把残余时间归因于 GPU 执行，再使用项目已有的硬件 counters 与 shader profiling 采集要求。
