# 57c 两帧反复闪烁现场

2026-10-27。用户在设置应用操作时报告：新内容停止绘制，每次更新像是在两幅旧画面之间切换。
现场保持运行；本次只请求了两次 `gpu`、一次 `display` 和一次 `tasks backboardd`，没有重启、复位 GPU 或修改运行参数。

## 已取得的证据

原始连续日志是工作区 `logs/t733-usb-live/t733-20261027T133202.266092Z.raw`。
固定副本和两次状态查询位于 `logs/gpu-concurrency-57c-runtime/two-frame-flicker/`：

- `initial.raw`、`watch-status-initial.json`、`boot-events-initial.jsonl` 保存收到报告时的已有记录。
- `after-state-queries.raw`、`watch-status-after-queries.json`、`boot-events-after-queries.jsonl` 保存上述查询后记录。

| 现场字段 | 值和含义 |
| --- | --- |
| Metal 错误行 | `submit failed: 0xe00002c2 (VK_ERROR_DEVICE_LOST)` |
| 随后的 Metal 行 | `commandBuffer refused: command buffer: DeviceLost` |
| 内核 Submit 方法 | calls=2615、failures=1、status=e00002c2 |
| 已接受的提交 | submissions=2614、completed=2614 |
| 硬件完成序号 | submitted_fence=2615、completed_fence=2615 |
| 任务与引用 | jobs=0、held=23128、retired=23128 |
| 故障状态 | faults=0、suspends=0、poisoned_vm=0、poisoned_submit=0、poisoned_ring=0 |
| BO 映射错误 | method 3 共 50 次失败，最后状态 e00002be |
| BO 诊断旧值 | backboardd 4071 个，iconservicesagent 21 个；仅普通分配更新，释放和导入不更新，不能当作当前值相加 |
| 当前 GPU VM mapping | objects=4070，所有连接共享最多 4096 个 mapping |

两次 `gpu` 查询的提交、完成、错误和硬件序号相同。显示查询返回 ready=1、awake=1；
FrameInfo 仍有完成报告。后一事实表示显示交换路径仍工作，不证明被显示的像素有更新。

`e00002c2` 在所选 XNU 的 `iokit/IOKit/IOReturn.h` 中为 `kIOReturnBadArgument`；
`e00002be` 为 `kIOReturnNoResources`。Darwin Turnip 的 `darwin_submit_chunk()` 将非 Busy
的提交错误转换为 `vk_device_set_lost()`，随后 Metal runtime 拒绝新的 command buffer。

因此已确认的故障链是：一次提交被内核以参数错误拒绝，用户态 Vulkan device 进入 lost，
后续 Metal 绘制不再接受；此前已经接受的 GPU 工作全部完成。现场没有持续未完成的 GPU job。
这与用户看到两幅旧画面交替相符。导致该次参数错误的具体 IB、handle、范围或映射状态仍在调查。

## 当前调查边界

故障子代理独立追踪 `NativeAdrenoUserClient::submit()`、`NativeAdrenoSubmit::submit()`
与 Darwin BO 回收、命令资源引用。协调代理负责现场采集和汇总。

全局 GPU VM 目前最多 4096 个 mapping，各 Darwin device 的 BO 预算也各为 4096。
这提供了映射资源耗尽的候选原因。已有源码表明正常 BoMap 失败路径会 BoFree 并归还
IOVA 和用户态 ID，因此不能直接把 50 次映射失败解释成失效 handle 被提交。

进一步读取现场统计：backboardd 的缓存保留 3365 个 BO、71188480 字节，
`evict_objects=0`、`evict_allocation=0`。`darwin_bo_init_recorded()` 和 host import
路径在 BoAlloc/BoImport 失败时会回收缓存并重试，在 BoMap 失败时却只撤销本次分配、
返回错误。按字节限制缓存且缺少共享 VM 条目压力处理，确实会留下大量可回收 mapping。
这是已确认的错误处理缺口；最后一次 BadArgument 与该缺口之间的具体传播链尚未确认。

源码还存在能连接这两个错误的传播路径：Mesa `tu_cs.h:312` 的 `tu_cs_reserve()`
取得 `tu_cs_reserve_space()` 的失败结果后仅 assert 成功；行 278 的 `tu_cs_emit()`
以及数组写入仍可继续写入并推进旧指针。`tu_cs.cc:239` 据此产生 IB 范围，Darwin
提交后由 `NativeAdrenoSubmit.cpp:342` 检查是否超出 BO。实际构建配置见公共 core
`.cache/mesa-tu-ios-reconfigure.log:330`，为 `b_ndebug=true`、release。

这条源码通路支持“mapping 资源不足，命令扩容失败被忽略，产生超范围 IB，提交被
BadArgument 拒绝，用户态 device lost”的高优先级解释。本次没有保存失败 IB 的参数，
因此不把特定 IB 越界写成已直接观测到的事实。现有 queue `record_result` 检查不能
拦住未被写入该状态的扩容错误。

对应完整改法已纳入[资源提交设计](GPU_RESOURCE_SUBMISSION_DESIGN_57C_2026-10-27.md)：
统一共享 VM mapping 预算；BoMap 资源不足时回收已完成缓存并有界重试；命令流保存
第一次错误，全部 reserve/emit/直接写指针/子流/IB 生成调用点停止写入并传播原错误。
不能只把 assert 换成忽略错误的 if，或仅提高对象数量上限。

现有提交统计没有保存第一次拒绝的具体检查分支和对应 IB。后续提交接口需要在拒绝时
保存有界错误记录：请求序号、失败步骤、条目索引、handle/generation、IB offset/length、
mapping 状态、容量及返回码。该记录用于识别错误，不能替代资源寿命修复。

本现场没有改变 57c 源码或镜像，故障尚未修复。确定的资源管理与错误传播缺陷已纳入
设计；具体失败 IB 需要下一包的有界拒绝记录补齐。
