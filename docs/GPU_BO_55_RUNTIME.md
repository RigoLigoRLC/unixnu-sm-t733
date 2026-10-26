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

55的长行受到约255字节输出限制，部分slow记录的用途、缓存/退休队列字段被截断。上述计数、尺寸、耗时采用已完整收到的字段；原始记录保留。55b把所有新增字段拆成可关联短行，计数和GPU行为保持55实现。实机尚需确认55b的完整行尾已收到。
