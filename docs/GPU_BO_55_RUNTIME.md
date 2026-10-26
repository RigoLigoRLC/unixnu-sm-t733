# 55 实机 BO 复用与慢操作

2026-10-27。用户确认已进入桌面并打开界面、重复滑动。实际启动标识为 `system-gpu-bo-profile-55`。保存原始输出及解析值：工作区 `logs/gpu-bo-55-runtime/interaction-query.raw` 与 `interaction-analysis.json`；持续原始记录为 `logs/t733-usb-live/t733-20261026T183119.124446Z.raw`。

## 已定位的复用障碍

第一份有效快照中，内核 backing 缓存由于64对象上限淘汰13,343个对象，client close淘汰16个；峰值64对象、2,744,320字节。后续累计分别为17,961与16。相邻快照之间的4,618次淘汰全部由对象上限触发，低占用下反复碰到数量上限已经实测成立。

Turnip完整BO缓存是独立层。backboardd首份有效快照为19,814次申请、3,802次命中；后续170.469054秒内的数据如下：

| 计数 | 增量 |
| --- | ---: |
| requests | 6,246 |
| hits | 1,050，16.81% |
| misses | 5,196 |
| size不匹配 | 4,061，未命中的78.16% |
| flags不匹配 | 733，14.11% |
| 空缓存 | 323，6.22% |
| 兼容BO仍在退休队列 | 79，1.52% |
| alignment/memory property不匹配 | 0 |

相邻用户态快照 sequence为54与138。后者累计按对象数淘汰19,765次、按字节数淘汰168次，IOVA不足、内核allocation失败、close均为0。计数排除了把此窗口主要归因于fence未完成或连接关闭的解释。

第一份有效快照的用途统计：`cmd cs` 5,603，`draw sub cs` 5,500，`draw cs` 5,276，`tile store cs` 2,437，合计占申请的94.96%；8 KiB和16 KiB所在两个尺寸桶占84.29%。`vkAllocateMemory()`只有26次，另有950次descriptor pool与少量pipeline/global对象。

对应代码在内核 `NativeAdrenoMemory.cpp` 的 `BackingCacheEntries`、`takeBacking`、`cacheBacking`，用户态正式输入为core的 `patches/mesa/turnip-darwin-kmd.patch` 中 `DARWIN_CACHED_BOS`、`darwin_take_cached`、`darwin_cache_completed_locked`：两层各保留64个对象、精确尺寸匹配；Turnip还要求完整flags相等。全局FIFO淘汰使不同用途/尺寸的批量BO互相挤出，低字节占用也会达到对象数限制。flags不匹配计数说明需要按实际分配语义分组，不能直接放宽GPU权限等属性匹配。

下一步选择[BO设计](GPU_BO_LIFETIME_DESIGN_2026-10-27.md)中的按大小分档、按属性分组、对象内缓存节点和字节/metadata预算，以及完成后的有界回收。单纯扩大64项数组不能解决批量销毁、全局FIFO与不同大小资源混用的问题。

## 超长操作的实际尺寸

内核累计39个至少8 ms事件中，有20个64 MiB分配，最长46.497 ms；13个64 MiB owned descriptor release，最长26.525 ms。另有16 KiB分配42.013 ms、256 KiB descriptor release45.194 ms。计时为调用经过的墙钟时间，包括可能发生的调度等待。

Turnip首份有效快照中64 MiB申请23次、命中10次、未命中13次；全部申请字节中64 MiB共1,543,503,872字节。用途计数指向 `vkAllocateMemory()`。完整用户态分配包含缓存回收、内核分配和GPU映射，保留样本中64 MiB调用达66.256 ms；iconservicesagent一次达80.478 ms。这些值与内核单段不能直接相加，外层范围包含内层。

runtime实际存在suballocator：`metal2vulkan-runtime/src/memory.rs:52` 的默认块为64 MiB，`:1376-1377` 为普通资源选择这个块，`Pool::release:354-370` 每个pool只保留一个空块。`keep_room:1409-1435` 锁外分配，回锁发现空间已被别的线程补足后立即释放新块；`place:1383-1392` 锁外增长后直接添加，不重查先前失败的可用空间。代码证明存在重复增长和即时释放路径，当前BO记录尚未记录触发这条路径的上层请求。具体并发增长与保留方案见[BO设计的64 MiB补充](GPU_BO_LIFETIME_DESIGN_2026-10-27.md)。

## 输出修订

55的长行受到约255字节输出限制，部分slow记录的用途、缓存/退休队列字段被截断。上述计数、尺寸、耗时采用已完整收到的字段；原始记录保留。55b把所有新增字段拆成可关联短行，计数和GPU行为保持55实现。55b实机已收到完整用途、对象、flags、fence和缓存/退休队列字段。

## 55b 操作后的增量与长尾

用户要求分析操作一段时间后的记录。保存的第二、第三次查询分别为工作区
`logs/gpu-bo-55b-runtime/after-user-interaction.raw/json` 和
`logs/gpu-bo-55b-runtime/late-user-interaction.raw/json`。
原始采集为 `logs/t733-usb-live/t733-20261026T184400.809479Z.raw`。
以下增量采用同一 backboardd 连接代数3、Vulkan device 1的报告67至187，
用户态单调时间168.828895至412.656203秒，共243.827308秒。

| 计数 | 本窗口增量 |
| --- | ---: |
| BO申请 | 23,364 |
| 完整BO缓存命中 | 3,648，15.61% |
| 未命中 | 19,716 |
| 尺寸不匹配 | 15,161，未命中的76.90% |
| flags不匹配 | 1,209，6.13% |
| 空缓存 | 3,120，15.82% |
| 兼容对象仍在退休队列 | 226，1.15% |
| 对象数量上限淘汰 | 19,761 |
| 字节上限淘汰 | 0 |
| 报告失败 | 0 |

64对象上限使缓存以很低的字节占用反复销毁对象，持续交互后仍然成立。
最后一次报告仅保留27个BO、1,794,048字节；峰值对象数64。
flags维持真实分配语义，改造需要将兼容的尺寸和属性分别组织，不能把不同权限合并。

第三次查询保留的最近64个慢事件覆盖sequence224至287。其中：

| 时间，秒 | 事件 | 尺寸 | 墙钟耗时 |
| --- | --- | ---: | ---: |
| 379.710783 | cmd cs 最后引用转交退休流程 | 256 KiB | 239.143 ms |
| 379.350340 | cmd cs 最后引用转交退休流程 | 16 KiB | 224.489 ms |
| 231.362515 | host import 最后引用转交退休流程 | 4 KiB | 202.400 ms |
| 231.362599 | cmd cs 最后引用转交退休流程 | 64 KiB | 196.540 ms |
| 219.386948 | draw sub cs 实际存储释放 | 8 KiB | 100.791 ms |
| 219.386950 | 包含上述释放的退休调用 | 8 KiB | 100.805 ms |
| 231.675593 | vkAllocateMemory() 完整申请 | 64 MiB | 73.419 ms |

退休调用的尺寸属于最后放引用的BO；它可以同步回收其他BO，因此不能据此说
“释放这个16 KiB对象本身耗费224 ms”。实际存储释放范围包括CPU unmap、lease释放、
BoFree、IOVA和ID回收。外层事件与内层事件不能相加。
`tu_knl_darwin.cc` 中 `darwin_bo_finish` 在 `retire->lock` 内调用
`darwin_reap_locked`，后者扫描完成队列，缓存满后通过
`darwin_evict_oldest_locked` 同步调用这些释放操作；这是明确的长调用传播路径。

最新内核256条提交按sequence排序为2789至3044，时间241.959169至421.042534秒，
包含用户停止操作的空档。CPU准备平均6.294 ms、p95 10.141 ms、最大15.476 ms；
WC属性扫描平均2.218 ms、导入内存发布平均3.982 ms，合计占98.51%。
提交至首次观察完成平均36.985 ms、p95 63.926 ms、最大111.534 ms，包含排队、
CPU准备与完成观察延迟。此处未采集GPU硬件执行计数。
输出按环形数组物理槽顺序排列，不能把文本最后一行当成最大sequence。

两次查询之间Metal pipeline累计由115次/5176 ms变为116次/5180 ms，
Vulkan pipeline累计由120次/687 ms变为121次/688 ms。
该窗口只新增一次约4 ms的Metal创建，内部Vulkan创建约1 ms；
开机累计最大值1931 ms和268 ms在此期间没有变化。
因此当前重复交互的长尾应优先处理提交和回收，不能用开机最大pipeline耗时解释。

用户看到的 compositor HUD 另有累计和峰值保持行为，`Frame Duration` 还依赖
显示驱动完成报告。[HUD计时与回调证据](GPU_HUD_TIMING_2026-10-27.md)
按21H461原始QuartzCore及IOMobileFramebuffer反汇编记录了字段来源。
