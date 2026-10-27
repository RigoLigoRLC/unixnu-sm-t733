# 57: IOUserClient 并发与连接生命周期

日期：2026-10-27。本文记录已保存的 56b 交互证据及 57 的 IPC、连接配额实现。
构建、部署和显示 FrameInfo 交付由本轮统一发布记录承载。

## 56b 热期证据

原始记录为工作区 `logs/t733-usb-live/t733-20261027T004806.720902Z.raw`。
首份记录在 `logs/gpu-memory-56b-runtime/interaction-first.raw/json`，后续完整查询
在 `interaction-later.raw/json`；后者按 query/kind 保存字段。

| Reporter | 时间，秒 | requests | hits | misses |
| --- | ---: | ---: | ---: | ---: |
| q34 | 101.526948 | 4700 | 2116 | 2584 |
| q83 | 200.077998 | 5887 | 3296 | 2591 |
| q219 | 472.755133 | 5887 | 3296 | 2591 |

q34 到 q83 新增 1187 次申请、1180 次命中，命中率 99.41%。仅新增 7 次
size miss；pending miss 保持 1244、byte eviction 保持 334、object eviction 为 0，
reclaim bytes 为 0。q83 到 q219 只新增 3 次 import。用户仍反馈重复小卡顿，
因此热期继续调查提交、import、fence 与完成回调。冷启动期间的 pending miss
和 64 MiB 块占用缓存预算有单独的记录，不能用于解释这一热期窗口。

首份最近 seq 915-1170 的 submit CPU 平均 3.468 ms，其中 cache 0.116 ms、
publish 3.221 ms，提交到完成平均 43.14 ms。q34 到 q83 新增四次 4 KiB
host import 慢记录，分别为 22.256、23.000、32.206、19.135 ms，以及一次
8 KiB imported BO actual release 27.953 ms。同期 kernel 查询新增 56 次
BoFree，总计 2.428 ms；complete 与 descriptor release 各新增 56 次，总计
0.616、0.943 ms。用户 reporter 与 kernel 查询的时刻不同，这些累计差说明
总体工作量，不能作为单条慢记录的内部耗时拆分。

## IPC 锁的确定依赖

所选 XNU 源码位于构建树
`out/t733-kernel/gts7fewifi/ipados-17.7.11-21H461/gts7fewifi/tree/.cache/xnu-work/`。
`iokit/Kernel/IOUserClient.cpp:6467` 的 `callExternalMethod` 在调用驱动前执行
`ipcEnter`，方法返回后才执行 `ipcExit`。DefaultLocking 为 true 时，
SingleThreadExternalMethod 决定该锁是写锁还是读锁。`ipcEnter:608` 使用
原生 IORWLock；正常 close 在 `is_io_connect_method` 以外的关闭入口取得写锁，
task death 的 `iokit_client_died:745` 同样取得写锁。

上游实现为 Apple 的
[IOUserClient.cpp](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IOUserClient.cpp)
及 [IOUserClient.h](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IOUserClient.h)；
上述行号指本轮实际选择的本地源码。

56b 的 `NativeAdrenoPublish.cpp` 同时设置 DefaultLocking=true 和
SingleThreadExternalMethod=true。`runCore` 的 WaitFence 每次只在短暂持有
`g.lock` 时读取并退休完成状态，随后解锁并 idle；但外层 IPC 写锁仍覆盖整个
方法。Turnip 回收线程和普通 fence waiter 的 1 ms WaitFence 因而能阻挡同一
connection 的 submit/import/map/free 进入驱动。q34 到 q83 对应两次 kernel
查询的 wait_outer 新增 1280 次、3.205420 秒、11657 次 idle pause；它包含所有
等待来源。现有记录没有单独测量 NativeUserClientIPC 等待，故这里确认的是源码
中实际存在的串行依赖，具体占每次卡顿的比例由新包交互结果决定。

## 57 的锁与引用安排

57 使用 XNU 原有并发约定：DefaultLocking 保持 true，
SingleThreadExternalMethod 改为 false。普通 external method 共持 IPC 读锁；
close、task death 与旧 IOConnectMap/Unmap 继续使用 IPC 写锁。

| 操作 | 状态保护和对象持有 |
| --- | --- |
| core dispatch、submit、队列、BO handle/VM | `g.lock` 保护全部核心状态和连接 live/closing 检查 |
| WaitFence | 每次 poll 在 `g.lock` 下，idle 在锁外；IPC 读锁保护连接直到返回 |
| host import | `g.lock` 覆盖导入与 handle 发布，task 来自当前调用线程 |
| V2 CPU map/unmap | `g.lock` 覆盖 map_id 表、真实 IOMemoryMap 和 controlled lease 的转移；失败保留引用和隔离状态 |
| legacy CPU map/unmap | per-client `legacyCpuLock_` 覆盖 lease 检查、cached descriptor teardown 与最后一次 decrement；核心访问再取得 `g.lock` |
| legacy clientMemoryForType | IPC 写锁覆盖真实 MIG 映射阶段；descriptor 查找和建立同时使用 legacyCpuLock_ 和 g.lock |
| BO profile | `g.lock` 保护 slot generation、device ID 和固定记录存储 |
| 可选 export | 独立 export_lock 串行写入持久日志分片，不持有 g.lock |
| 可选 presenter callback | callback 所有者保留 backing 并同步自己的状态，允许并发调用；等待不占 GPU 锁 |

当前启用的 NativeMobileDisplay 直接扫描路径不经过 integration.displayCall，
当前 source 检索也没有发现该 callback 表的注册者。NativeAdrenoIntegration.h
明确表在 GPU 发布前安装，displayMemory 返回已 retain 的 descriptor，callback
对象由注册方持有。此前兼容接口不承担本轮显示 FrameInfo 实现。

close 先在 g.lock 下标记 closing 并解除 mailbox owner，随后持 legacyCpuLock_
移除实际 CPU 映射和旧 descriptor，最后在 g.lock 下 releaseAll。只有成功完成
teardown 才归还 GPU slot；失败保留 BO、core 和 slot，避免仍被引用的地址复用。
free 仅在对象已有最终独占所有权时释放其 mutex；完成 CPU 映射解除前不释放
mapping 表。锁顺序为 IPC gate、legacyCpuLock_、g.lock；export 不取得 g.lock。

旧 mailbox poller 改为先在 g.lock 下取得 owner 引用、连接 generation 和请求快照，
释放 g.lock 后调用原生 `IOUserClient::callExternalMethod`。它因此遵守与用户方法
相同的 IPC gate。响应只在 owner、generation 和 request sequence 都仍匹配时写回，
owner 引用在锁外释放。此传输仅接受原有 Query 至 GetDeviceInfo 方法，不接受
依赖当前用户 task 的 host import 或 V2 map。实际驱动编译 recipe 已启用
XNU_KERNEL_PRIVATE，直接使用该原生入口，无需另加 gate 实现。

并发新增的诊断状态也有对应同步：core 活跃调用按方法计数并在 g.lock 下更新，
wait_outer 在该锁下汇总，external method 入口计数和 selector bits 使用原子更新。
原有 g.lock 等待计时包含 runCore 新入口和实际 dispatch 两次取锁。

## 打开次数与 Poster 首次失败

56b 中 AegirPoster 和 KaleidoscopePoster 均出现 opens=8、closes=8、slot=255；
其他用户槽为空，全局 opened=19、closed=18 只剩 backboardd。源码却用进程名
累计成功 open 的八次上限拒绝后续请求，已关闭的枚举和 device 连接也消耗额度，
导致后续 e00002d5 Busy。现有计数没有证明 slot 泄漏。

57 在 mount 的同一次 g.lock 临界区中，按四个实际占用 slot 的 owner 名称计算
同时存活数量、检查配置上限、选 slot、打开 core、设置 VA window，成功后才登记
owner 和 opens。实际所有权表固定四项，独立于只保留前八个进程名的诊断表。
close 成功才清除 owner 并增加 closes；失败保留占用。四个物理 slot 和各自 VA
window 未扩大，mount 将实际 NoMemory、Busy、NoSpace 或 scope 错误返回调用者。

Poster 最初八次打开的日志先出现 MACF 拒绝，随后驱动连接关闭，mount failures 为 0。
其签名包含 `com.apple.security.ts.opengl-or-metal`。本轮授权修复由 XNU open
路径按实际 owningTask 的签名 entitlement、精确 accelerator metaclass、Metal plugin
属性与连接类型授权，原系统进程列表仍保留。Publish 的 grants 计数使用相同
predicate；进度 marker 不建立连接，因而不计入连接 grants。授权源码和显示修复
由各自负责人完成，本文件的 IPC 修复不增加进程名白名单。

## 交付与剩余工作量

本专题实现修改 `drivers/Adreno6xx/NativeAdrenoPublish.cpp` 与
`NativeAdrenoIntegration.h`；Mesa 56 缓存、worker 和 device connection 代码保持
本轮已固定的来源。本轮没有新增或运行测试、文件 hash、独立构建或提交。

IPC 并发允许其他方法在 WaitFence idle 期间进入驱动。import 的 VM 工作、CPU
映射以及未知 CPU 写入来源的完整 publish 仍受其真实所有权和缓存维护要求约束。
实机比较使用相邻 query 的 requests/hits/misses、import/actual release 慢事件、
wait_outer 与同期 submit CPU/cache/publish 数据，并结合新显示完成时间，区分仍在
驱动内执行的工作和被本轮解除的外层串行等待。

## 统一构建与刷入

2026-10-27，`system-gpu-concurrency-57c` 九阶段全部返回0，MSC完整写入
536870912字节并sync成功，Data保留。用户启动前已确认被动USB collector运行。
刷入成功后记录本节；当前等待新启动及操作结果。

构建前源码均已提交且工作树干净，固定设备 `642750e`、core `b5ebcf6`、
SoC `6fa3518`、Adreno6xx `5ca9e5d`、NativeMobileDisplay `0090571`。
完整revision及命令在 `build/releases/system-gpu-concurrency-57c/source-revisions.json`，
同目录保存 `stages.tsv`、子模块清单及实际 `xnu-patches.list`。不可移动tag为
`system-gpu-concurrency-57c` 和 `build/system-gpu-concurrency-57c-source-1`。

57首次编译在显示通知处遇到protected接口访问错误，新增禁止实例化的
IOUserClient派生静态发送封装；57b在GPU的两处volatile自增遇到编译错误，
改为原有锁保护下的显式读写。每次修复均先提交并固定新tag，失败包的日志及
源码tag保留。57c才是本轮已刷入镜像，后续文档提交不改变该镜像来源。
