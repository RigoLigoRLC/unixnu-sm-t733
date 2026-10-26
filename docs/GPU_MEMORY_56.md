# GPU memory 56

本包处理55b操作后的BO反复分配和同步回收，以及每次提交重新扫描owned WC页的成本。
实测依据见[55b增量与长尾](GPU_BO_55_RUNTIME.md#55b-操作后的增量与长尾)。
构建标签为 `system-gpu-memory-56`，构建、刷入和启动结果由交付入口记录。

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
