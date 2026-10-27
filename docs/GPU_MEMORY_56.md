# GPU memory 56

本包处理55b操作后的BO反复分配和同步回收，以及每次提交重新扫描owned WC页的成本。
实测依据见[55b增量与长尾](GPU_BO_55_RUNTIME.md#55b-操作后的增量与长尾)。
首次构建标签为 `system-gpu-memory-56`，XNU环境探测因沙箱拒绝sysctl退出，
未进入GPU编译。后续构建使用 `system-gpu-memory-56b`，构建、刷入和启动结果
由交付入口记录。

## 实现

- Turnip完整BO缓存按Freedreno的每倍增范围四档尺寸组织，再按实际flags、内存属性
  和alignment分桶。申请大小向对应档容量取整，复用时保持真实权限与完整映射。
  缓存保留预算128 MiB，每个内核handle预留退休元数据，数量上界4096。
- 最后一次放引用只转移已有对象。回收线程等待捕获的fence完成，再交给缓存；
  真实CPU unmap、lease释放、BoFree在allocator锁外执行。成功释放后才归还IOVA
  和用户态ID。回收积压达到64 MiB时，冷分配等待实际释放到32 MiB，等待有截止时间。
  错误保留handle及记录，直到连接关闭按内核原有流程处理。
- 每个Vulkan device拥有独立IOKit连接、fence状态和该连接实际GPU地址窗口。
  枚举使用短命连接；device退出先结束回收与报告线程，再关闭自己的连接。
  地址分配器和RMV记录都使用该device取得的实际窗口。
- 新建owned WC BO使用 `IOBufferMemoryDescriptor` 的持久WC memory entry。
  V2 map直接从这个descriptor建立并保留真实 `IOMemoryMap`，unmap成功后才释放
  独立mapping lease。普通和placed映射一起接入。旧协议继续属性修复；外部import
  保留原cache mode及完整维护。V2能力和budget记录通过独立capability发布。
- metal2vulkan每个buffer/image内存池各有增长mutex。已有块不足时，申请者取得
  该mutex后重查空间，再在内存表锁外创建块；可选reserve跳过正在增长的池。
  既有空间的分配和释放不等待另一个线程的 `vkAllocateMemory`。

## 诊断和剩余边界

55已有慢事件和命中原因保留。新增budget记录包含实际metadata占用、node数量上界、
缓存字节预算与回收积压高低水位。正常热路径只更新有界内存记录。

未知CPU写入来源的import仍按现有提交路径完整维护。后续缩小到实际资源及范围
需要[提交资源与所有权方案](GPU_SUBMIT_COHERENCY_DESIGN_2026-10-27.md)中的完整
BO引用列表和生产方交接，不能仅删除flush。IOUserClient原生IPC串行锁仍存在；
本包通过复用减少进入这些接口的次数，并将实际存储销毁移出完成回调线程。

HUD的计时来源、累计和峰值保持，以及显示frame-info缺口见
[HUD代码证据](GPU_HUD_TIMING_2026-10-27.md)。56不改变显示frame-info实现。
首次pipeline创建的完整设计见[pipeline方案](GPU_PIPELINE_CACHE_DESIGN_2026-10-27.md)。

本包不新增或运行测试。实际编译和部署结果记录在发布目录的 `stages.tsv`、
`source-revisions.json` 与MSC输出；性能变化由后续相同操作的实机增量记录衡量。

## 构建与部署

2026-10-27，56b九阶段全部返回0。构建前各源码仓库干净，固定设备 `bc90d20`、
core `e0a8997`、SoC `4de7850`、Adreno6xx `5b15108`、AdrenoMetal `093e075`。
不可移动tag为 `system-gpu-memory-56b` 与
`build/system-gpu-memory-56b-source-1`。完整提交号、命令、子模块和补丁列表
在 `build/releases/system-gpu-memory-56b/` 内；后续文档提交不改变此镜像来源。

MSC重新枚举后完整写入536870912字节并sync，返回 `write-fat completed`。
日志为工作区 `logs/msc-system-gpu-memory-56b/write-fat.jsonl`。Data保留。
USB被动collector已确认运行，已请用户完整重启固件并选择Boot UniXNU。

## 操作后的实测

用户已启动并操作，反馈没有明显的整体流畅度提升，Frame Duration仍为0。
本包没有修改FrameInfo，后者与实际交付范围一致。原始记录为工作区
`logs/gpu-memory-56b-runtime/interaction-later.raw`，按查询及记录类型分组的数据
为同名JSON；第三次查询前的系统壁纸崩溃另由57专题记录。

同一backboardd连接代数3、device 1，在BO快照q34（101.526948秒）到q83
（200.077998秒）之间，申请从4700增至5887，命中从2116增至3296：1187次
申请中1180次命中，命中率99.41%。只有7次大小不匹配，pending miss、字节
预算淘汰均未增加，对象数量淘汰为0。第三次q219（472.755133秒）的BO申请
和命中计数未变，不能把这一段空闲时间继续计为新的交互样本。

前两次查询各自最近256次提交的CPU准备平均3.468和3.283 ms，WC处理平均
0.116和0.139 ms，import发布平均3.221和3.016 ms。提交到fence完成观察仍
包含GPU排队和主机唤醒，不能据此报告GPU执行耗时。55b的CPU准备平均6.294 ms
来自不同动作窗口，说明软件路径成本降低，但不证明总体帧率提升。

热期新增慢事件66到70仅含4个4 KiB host import（22.256、23.000、32.206、
19.135 ms）和1个8 KiB host import释放（27.953 ms）。没有新增owned BO
分配或退休慢事件。kernel方法内部计时未覆盖IOKit外层IPC锁等待；57将该
连接的externalMethod改用原生IPC读锁，并补齐各独立状态的同步。

Metal pipeline计数从110/13536 ms增至111/13575 ms；Vulkan从115/7999 ms
增至116/8002 ms。后续壁纸崩溃窗口两者均未增加。开机累计编译时间不能
解释这一窗口的反复抖动。

IPC锁等待、实际连接配额及57对应实现详见[57接续修复](GPU_MEMORY_57.md)。
