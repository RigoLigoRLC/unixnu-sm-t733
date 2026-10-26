# 55 BO 缓存原因与慢操作采样

本包保持 54 的分配、缓存容量与 GPU 提交行为，增加两层独立诊断。首次构建标签为 `system-gpu-bo-profile-55`，输出修订包为 `system-gpu-bo-profile-55b`。实际源码提交号和构建命令随产物记录在 `source-revisions.json`；本文的构建和部署结果以交付入口为准。

55已经完成构建、刷入及正常交互采样。实机输出发现长行在约255字节被截断，55b将字段拆为短行，完整保留用途和队列状态。用户态每行的 `s/g/d/q` 分别为连接slot、连接generation、Vulkan device ID、记录sequence；同一组字段按此四项合并。kernel记录按sequence合并。55已完整输出的计数和尺寸字段继续作为有效证据。

## 采样内容

内核 `NativeAdrenoMemory` 对 backing 请求按 4 KiB 起的二次幂区间累计请求数、字节数、lookup、hit、miss、bypass。一次未命中只记一个主要原因：空缓存、尺寸、地址位宽/连续性属性、隔离对象。插入拒绝单独统计。淘汰分为对象数量上限、字节上限、连接关闭、内存压力、分配失败重试，分别保留尝试、失败、完成数和字节数。

Turnip 的完整 BO 缓存另有请求、eligible、hit/miss、导入、固定地址/flags/尺寸 bypass，按尺寸与用途分类。未命中原因依次区分兼容对象仍在退休队列、空缓存、尺寸、alignment、flags、memory property。`pending` 表示兼容对象尚未被现有回收流程移入缓存，可能排在其他未完成对象之后；采样不额外查询这些对象的 fence。IOVA 不足、内核分配失败、容量上限和连接关闭分别计数。

用途名来自原始 BO 申请参数，独立保存在 BO 中，Mesa 关闭 BO debug table 时仍有效。15 个不同用途和一个汇总槽固定占用内存；名称最多31字节。导入独立累计，不计入可复用的私有 BO 命中率。

两层各保存最近64个至少8000 us的操作。普通操作只更新累计计数。内核记录完整 allocate/import 和实际 owned/imported descriptor release，包含尺寸、物理 spans、地址位宽、contiguous/WC/import/pool/quarantine 标志、请求复用结果、process、缓存占用与 available pages。Turnip 记录完整 allocate/import、实际释放、最后引用转交回收的耗时，包括返回状态、用途、尺寸、alignment、handle、IOVA、flags、memory property、连接退休 fence 和 pending/cache 占用。用户态 allocation 包含其调用中触发的旧对象回收及映射工作。

Turnip 使用每设备一个两秒周期的后台报告线程。它在 allocator mutex 内复制固定内存，释放锁后通过可选 ABI method 17 发送。线程保留原连接的 send right，退出时先发最后快照并 join，再关闭连接。没有逐 BO 的报告调用或热路径文本打印。每连接代数及 Vulkan device ID 独立编号；内核最多保留8个报告源，已关闭的源可以被替换，活跃源满时返回 NoResources，用户态计入 report_failures。

USB `gpu` 查询一次复制后在 GPU 锁外输出 `NativeAdrenoBO:`。每行带 sequence 与 sample time，慢事件重复查询保留，按连接代数/device/sequence 去重。计数是累计值，使用相邻两次查询的增量分析交互窗口；用户态和内核时间分别来自各自已有单调时钟。slow_total/slow_overwritten 表明有界历史的覆盖量。

## 慢事件枚举

| 用户态 operation | 含义 |
| --- | --- |
| 1 | 完整 BO 分配调用 |
| 2 | host import 调用 |
| 3 | CPU unmap、lease 释放、BoFree 和 IOVA/ID 回收 |
| 4 | 最后应用引用转交退休队列及该调用促成的回收 |

用户态 reason：0 hit，1 empty，2 size，3 alignment，4 flags，5 memory property，6 compatible pending，7 fixed address，8 excluded flags，9 oversized，10 import，11 failure，12 本操作未查询缓存。release/retire 使用12。`connection_fence` 沿用54的连接最后提交值。

内核 operation：0 allocation，1 owned descriptor release，2 imported descriptor release，3 import。source：0 unknown，1 kernel-owned，2 user-owned，3 imported。cache_result：0 not requested，1 hit，2 miss。flags 的 bit0..5 依次为 contiguous、WC、imported、pooled、quarantined、reuse requested。

## 后续修改依据

[BO 生命周期方案](GPU_BO_LIFETIME_DESIGN_2026-10-27.md)给出按采样结果选择大小分档、保留预算、完成通知和后台回收的条件；[提交与缓存一致性方案](GPU_SUBMIT_COHERENCY_DESIGN_2026-10-27.md)定义资源引用、实际 CPU map 所有权、逻辑提交和 fence；[pipeline 方案](GPU_PIPELINE_CACHE_DESIGN_2026-10-27.md)定义 Mesa 对象缓存、后台持久化、有界编译和真正异步的 Metal API。这些架构改动尚未进入55。
