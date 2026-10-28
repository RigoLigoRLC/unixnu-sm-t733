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

63c2 已通过 MSC 完整写入 536870912 字节并完成 sync，返回 `write-fat completed`，退出 0，Data 保留。旧 FAT 完整备份为工作区 `backups/msc-system-gpu-opt-63c2/payload-before.img`，读写记录在 `logs/msc-system-gpu-opt-63c2/`。被动 USB 采集器已就绪，当前等待首次启动确认 GPU 合成恢复。
