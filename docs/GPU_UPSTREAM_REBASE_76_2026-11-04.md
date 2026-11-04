# GPU 上游迁移 76

2026-11-04，按用户要求再次同步上游 GPU 更新。`system-gpu-upstream-76b`
完成九阶段构建，并于 23:26 经 MSC 写入完整 536870912 字节启动 FAT，sync 成功，
部署返回 0。被动 USB 采集已运行，并已通知用户完整重启固件后选择 `Boot UniXNU`。
本次启动已发布 Metal 加速器并运行 SpringBoard/backboardd。保存的启动快照记录
1247 次显示完成，failed/errors 为 0，DPU flip 达到 1200；没有记录到 panic。
桌面颜色和交互结果等待用户观察。

## 源码

| 仓库 | 构建提交 | 官方基线 |
| --- | --- | --- |
| device | `48ce095b7b27f347f2b9c1191cf69ff97c295570` | 本机设备仓库 |
| core | `6a33f6eb5f4ea67a57a59b7bd631738f429a62a5` | `350ddc2a23dcaea7d74d8a469259c2efcec3e4a9` |
| SM7325 | `14d3ee4d62783e036b7bc9d6c3bfe88278384d40` | 本机 SoC 仓库 |
| Adreno6xx | `fa0622067e875828abcc1a4c43e7d1ff62227b31` | `6fa7bae2a5b3b523e790eb3e0348af4d98969e4d` |
| AdrenoMetal | `6b46ed460a1fdb0e1248cf656b4efa6e7a576300` | `243d63cf43305a85c8119b66b49cb7df4b30b019` |
| ArmSmmu4K | `b49b0ea8cebc0ce416ae225e124da95bb88dcb87` | `83969cd874503e65997ff7639f7dcf3d0a92cd17` |
| NativeMobileDisplay | `9e61131843529e3d5deb4466b26468c8e9628ce5` | 保留本机 DPU 实现 |

七仓固定 tag `build/system-gpu-upstream-76b-source-1` 已发布。core、Metal 和 GPU
使用 `rebase/unixnu-sm-t733-20261104`；原历史保存在备份分支及原构建 tag。
设备与 SoC 主分支更新了对应引用，Apple3 分支保持本轮开始时的状态。

## 实现

- core 重放设备构建和菜单适配，保留 74e 的懒分配资源登记时机。
  IPSW 缓存复用及 Metal 构建标识遵循 `UNIXNU_RECORD_HASHES=0`。
- Turnip 的正式 series 为 32 层，新增上游队列优先级、帧记录、GMEM transient
  attachment、线程渲染模式及 GMEM 内存侧通道交换更新。上游正式颜色修复替代 75
  独立补丁，并覆盖 linear texture source 的交换设置。本机继续使用空 `TU_DEBUG`。
- Metal runtime 为上游 30 层加本机 9 层，基于 metal2vulkan `624bab39`。
  合入共享 UID shader 缓存、队列优先级、异步退休释放及资源复用；保留硬件一致性、
  复制、memoryless、资源登记和采集支持。本机构建传 `--single-sample off`。
- A642L 保留单 ring、WB 一致性、315/450/550 MHz DVFS 及 GMU AO/OOB 访问规则。
  上游优先级修改作用于未派发任务，队列内部保持 FIFO。新增有界帧记录使用
  `0x7102` selector 和 120 字节记录，原 ABI 1..44 保持。
- 显示保留每张 surface 的 acquire/read 依赖、NotReady 重试和错误处理。
  validation、producer 等待、flip/latch、退休和 blank 事件接入共享记录；物理地址
  取自已 prepare 的 DPU 槽，不新增页面遍历或寄存器读取。

## 构建与部署

76 首次构建在 `NativeAdrenoSubmit.cpp` 调用上游 `fenceValue` 时失败。
GPU `fa06220` 改用本机 `observeFence` 非阻塞读取实际完成前缀。
76a 通过内核和 Turnip，Rust bridge 完成后在 Metal 插件遇到两个未使用函数的
`-Werror`。Metal `6b46ed4` 删除无调用方的状态包装及其专用存储，保留格式策略。
76b 的 configure、resolve、kernel、menu、turnip、metal、wallpaper、debug-tools、
payload 全部返回 0。各失败包的 tag 和日志保留。

构建前七仓源码全部提交，工作树干净，组件 revision 和 gitlink 同步；构建完成后
读取本轮 XNU 补丁列表。没有添加或运行独立测试，也没有计算文件确认 hash。

以下路径以工作区根目录为基准：

```sh
source tools/activate.sh
bash tools/build-t733-recovery.sh system-gpu-upstream-76b --system --payload-only \
  --local-module soc:sm7325=/Users/rigoligo/expm/unixnu-sm-t733/sources/unixnu-sm7325-common
bash tools/deploy-t733-msc.sh system-gpu-upstream-76b
```

- 产物及九阶段日志：`sources/UniXNU-gts7fewifi/build/releases/system-gpu-upstream-76b/`
- 完整源码和命令：产物目录 `source-revisions.json` 及 `logs/gpu-upstream-76b/source-revisions.json`
- 本轮 XNU 列表：`logs/gpu-upstream-76b/xnu-patches.list`
- 部署：`logs/msc-20261104-232614-system-gpu-upstream-76b/`
- 启动采集：`logs/t733-usb-live/watch-status.json`、`boot-events.jsonl` 和对应 raw/tail
- 本次启动快照：`logs/gpu-upstream-76b-boot/`，原始文件为 `t733-20261104T152726.111220Z.raw`

用户此前报告 74e 卡顿改善很小、壁纸及图标红蓝通道异常。75 的启动日志记录了
launchd、SpringBoard 和 backboardd，但没有用户确认的颜色结论；这两项实机结果
不能由本轮编译和烧录成功代替。此前入口状态完整保存在
[设备入口归档](archive/AGENTS_GPU_STATE_2026-11-04.md)。
