# 54：BO 管理和 GPU map 开销

2026-10-27，`system-gpu-bo-54` 已完成九阶段构建和完整 512 MiB FAT 刷入、
sync；Data、身份、配对、密钥、HUD、触点和现有固件保留。用户已启动并
确认明显流畅，首次打开界面和重复手势仍有小卡顿。

## 源码修改

Adreno6xx 使用 XNU `libkern/tree.h` 红黑树按 BO 保存 GPU 地址范围，
物理 spans 和排序索引由各 BO 自己持有。handle 使用 slot 和 generation，
空闲记录通过链表分配。固定的全局 span 数组已移除。

ArmSmmu4K 新增 `ArmSmmu4KLiveUpdateContextRanges`：初始 context 审计后，
只检查和写入本 BO 涉及的页表项，发布修改的页表页，unmap 继续执行
TLBIASID 和 TLBSYNC。原有完整 range 接口保留。

内核 backing 缓存按大小、物理地址宽度、连续性匹配，最多64项/64 MiB、
单项16 MiB；复用前恢复 WC 属性并清零，内存压力、分配失败和 close 回收。
Turnip 在 fence 完成后缓存完整私有 BO，最多64项/128 MiB、单项64 MiB，
保留 handle、CPU map、GPU VA 和页表；导入、共享与固定地址对象不入此缓存。
Mesa 本地索引由独立 allocator 分配，内核 handle 始终完整保留为64位。

设计说明和参考来源见组件 [BO memory management](../drivers/Adreno6xx/BO_MEMORY_MANAGEMENT.md)。
主机研究比较了 GEM/TTM、MSM/Freedreno/Turnip、AMDGPU/RADV/radeonsi、
i915/Xe/ANV、Nouveau/NVK、Panfrost/PanVK、V3D/VC4、Asahi/AGX 和 KGSL。

## map 耗时结论

52b 的约4 ms为一次 BO map 的平均值，计时已位于全局 GPU 锁内部。
CPU 用户映射和 WC 属性切换分别在其他入口。旧实现每次 map 多次遍历
约11万条驻留 span，插入时搬移数组，每64次还额外读取整张页表。
这解释了已有对象增多时管理工作随之增长。旧日志没有输出内部阶段计时，
54新增的 USB 累计数据已确认 map 平均降至56.4 us。

`gpu` 查询输出 `vm_map`、`vm_unmap`、`pte_map`、`pte_unmap`、
`memory_stage`、`backing_cache` 和 `map_pages one`。计数保存在有界内存中，
查询时复制后打印。本轮未新增或运行测试，已完成正式系统构建。

## 实机性能结果

2026-10-27，首次 `gpu` 查询记录13,294次 BO map，总耗时749,603 us；
444次单页 map 平均58.4 us，最大934 us。3,283次提交全部完成，GPU
faults=0、poisoned_jobs=0，BO allocate/map/free没有失败。

最近256次提交的准备时间平均7.26 ms，其中 WC 属性检查平均3.06 ms、
内存发布平均4.06 ms，两者占98.17%。当前提交遍历所有驻留 WC 页，
并对所有导入 BO 做整范围缓存维护；提交 ABI 只携带命令缓冲，需补充
实际使用的纹理、渲染目标等资源及访问范围才能缩减这些操作。

分配阶段单次最大42.105 ms，descriptor释放单次最大25.586 ms。
内核 backing 缓存命中率11.3%，淘汰9,873次，容量峰值只有3.14 MiB；
64对象上限限制了小对象复用。首次 pipeline 创建最大3.155秒。
这些结果解释了明显改善后仍有持续提交成本和偶发长尾。

工作区证据为 `logs/gpu-bo-54-runtime/first-query.raw` 和
`first-query-summary.json`，完整分析见
[54实测](../../../notes/GPU_BO_MAP_COST_AUDIT_2026-10-27.md#54-runtime-and-remaining-stutters)。
本次为软件路径计时，未采集 GPU 硬件计数。剩余卡顿的源码优化与部署尚未完成。

## 固定版本和产物

| 源码 | 构建提交 |
| --- | --- |
| 设备 | `cb7a993808b3e7bdce5d49c1e86970d5b35e398e` |
| core | `952c15dcb99f6af349525e9f02c37a32bdf1075a` |
| SoC | `c401d2d925ab23a68e3b29ff3df87385d801b84b` |
| Adreno6xx | `548b68101a0029d3b5cf5fc468e60fb2e48ec2a2` |
| ArmSmmu4K | `d92ebe7ee91e2412a40490f9f78a4a927876229a` |

以上仓库均用不可移动 tag `system-gpu-bo-54` 固定。完整命令、子模块和
失败尝试记录在 `build/releases/system-gpu-bo-54/source-revisions.json`。
最终 FAT 为该目录 `payload.img`，九阶段退出码均为0。
工作区部署记录为 `logs/msc-system-gpu-bo-54/write-fat.jsonl`，最后记录
536870912 字节和 `write-fat completed`；原 FAT 备份为
`backups/msc-system-gpu-bo-54/payload-before.img`。
