# 63b Metal 设备创建失败与 63c2 修复

2026-10-28，SM-T733 / A642L。63b 刷入后用户报告 GPU 未工作。启动日志确认内核 GPU 初始化通过，Metal 设备创建期间的两次任务均执行成功，随后用户态等待请求被拒绝，设备创建返回 `DeviceLost`。

## 证据与原因

- USB 原始日志：工作区 `logs/t733-usb-live/t733-20261028T151345.340688Z.raw`。
- 第 2201 至 2202 行：Metal accelerator 已发布，CP scratch 自检通过。
- 第 3449 行：`report2=PUB no device: device: DeviceLost`。
- 第 3503 至 3507 行：`faults=0`、`submissions=2 completed=2 timeouts=0`，两次异步完成状态均为 0。显示帧记录的 `submissions=0`。
- `logs/t733-lldb/gpu63b-backboardd-load.txt` 确认 backboardd 已加载 NativeAdrenoMetal 和 libvulkan_freedreno。读取后已 detach。另一次 OSLog 查询被 SIGSTOP 中断，已自动 detach；未取得该查询的日志内容。

M2V 的 `Device::assemble` 调用 `prime::prime`，提交初始化任务后执行 `wait_semaphores`。Turnip 的 `vk_darwin_async_wait_many` 传入完整 `NativeAdrenoAbiSyncWait`，大小为 32 + 128 * 16 = 2080 字节。`dw_call` 的转发输入容量仍是 `sizeof(NativeAdrenoAbiSubmit)`，即 416 字节，因此在进入内核前返回 `kIOReturnBadArgument`，随后被映射为 Vulkan `DeviceLost`。

内核外层接受最多 262144 + 8 字节的 descriptor 输入，转交后由 UserClient 按完整 SyncWait 结构接收。本次修复将用户态输入容量设为 Submit 和 SyncWait 两种结构大小的较大值，保留有界缓冲区和调用者传入的输出容量。

## 提交与构建

| 内容 | 固定版本 |
| --- | --- |
| core 优化主分支 | `1aa8c23b8100ee766e9de540ac9b3045130b1e1e` |
| core Apple3 分支 | `5f62655d3b0f15d708d00a2716ce6ae3904301c6` |
| 设备构建提交 | `30869b1919dce462d35f66290db9dfbd444ff7ac` |
| 设备 Apple3 固定引用 | `7f4080f` |
| 构建标签 | `build/system-gpu-opt-63c2-source-1` |

修复文件为 core 的 `patches/mesa/turnip-darwin-async-submit.patch`，构建使用的 `.cache/mesa-work` 同步应用该修改。Metal、GPU 内核、显示、SoC 与 SDR 格式策略沿用 63b，组件提交完整记录在 `build/releases/system-gpu-opt-63c2/source-revisions.json`。

```sh
source tools/activate.sh
bash tools/build-t733-recovery.sh system-gpu-opt-63c2 --system --payload-only \
  --local-module soc:sm7325=/Users/rigoligo/expm/unixnu-sm-t733/sources/unixnu-sm7325-common
```

63c 的内核构建因主机沙箱阻止 sysctl 查询 CPU/内存参数而退出，原日志保留在 `build/releases/system-gpu-opt-63c/`。63c2 使用相同源码在允许该查询的环境构建，九阶段均退出 0，产物为 `build/releases/system-gpu-opt-63c2/payload.img`。

63c2 已通过 MSC 完整写入 536870912 字节并完成 sync，返回 `write-fat completed`，退出 0，Data 保留。旧 FAT 完整备份为工作区 `backups/msc-system-gpu-opt-63c2/payload-before.img`，读写记录在 `logs/msc-system-gpu-opt-63c2/`。随后启动及用户滑动记录确认 GPU 合成恢复。

## 63c2 操作期结果

本次 USB 日志为工作区 `logs/t733-usb-live/t733-20261028T152909.990732Z.raw`。GPU 查询完整输出位于第 3945 至 11094 行。累计提交 929、完成 929，faults、timeouts 和 poisoned_jobs 均为 0；显示完成 1099 帧，failed、missing、unmatched 均为 0。

保留的 256 笔提交为 sequence 674 至 929，时间为开机后 153.026 至 206.125 秒；512 帧为 sequence 588 至 1099，时间为 135.358 至 206.134 秒。帧缓存已覆盖较早的 587 帧。导出数据在工作区 `logs/gpu-opt-63c2-runtime/`：`frames.jsonl`、`frames.csv`、`summary.json`、`operation-summary.json`、`bo-operation-intervals.json`。

| 指标 | 操作期结果 |
| --- | --- |
| 硬件提交函数 CPU 耗时 | 平均 24.695 us，中位 16 us，p95 51 us；范围不含调度器接收前的资源遍历、分配和用户态编码 |
| 接受任务到下发 | 平均 40.469 us，中位 30.5 us |
| 下发到观察到完成 | 平均 34.078 ms，中位 31.293 ms，p95 65.919 ms；包含硬件队列及完成观察延迟 |
| 开始合成到显示完成上界 | 中位 63.399 ms，p95 125.286 ms，最大 702.318 ms |
| 最后连续 114 帧 | 帧间隔中位 33.663 ms；合成至显示中位 63.399 ms，p95 111.406 ms |
| 每次提交的软件缓存维护 | 保留的 256 笔均为 0 us |
| 两次提交间记录到的映射/释放/分配 | 173/241/12 次，累计 1014.733/377.337/456.436 ms；这些时间在提交函数计时范围之外 |

最慢的第 914 帧耗时 702.318 ms，其中 657.684 ms 发生在该帧最终输出任务首次提交之前。该段第 741 笔提交前记录到 38 次映射共 141.954 ms；第 744 笔提交前另有一次分配耗时 239.367 ms。持续慢帧和提交前长停顿需要分别定位。

从设备创建累计至查询，backboardd 的 BO 复用请求为 9605 次，命中 7665 次，命中率 79.80%；资源处理的 `clean_bytes=0 acquire_bytes=0 fallback=0`。这些累计值包含启动初始化。实际普通纹理格式列表包含 80，即 BGRA8Unorm；IOSurface 纹理创建成功 27 次、失败 0 次，平面请求计数 0。该计数直接读取传入源 IOSurface 的 `IOSurfaceGetPlaneCount()`，因此这 27 次创建调用的源表面均为非平面布局，未进入双平面导入路径。记录未提供成功 IOSurface/视图的 Metal 格式号，尚未取得 81 即 BGRA8Unorm_sRGB 的直接记录。

本轮读取已保存的软件时序记录，没有新录制硬件计数或 shader 采样，不能据此把提交后延迟全部归为 shader 执行时间。
