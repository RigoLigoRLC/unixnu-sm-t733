# XNU 补丁来源与诊断选择

本文保留 2026-10-24 的选择器与缓存处理记录。当前 format 2 使用 board 显式 series 和统一 `UNIXNU_PATCH_LIST`，操作说明见 [现行构建说明](BUILDING.md#4-补丁与编译输入)，48f 实际构建、写入和启动记录见 [format 2 记录](archive/FORMAT2_2026-10-25.md)。

2026-10-24 源码整理。此次未构建、未执行补丁应用检查、未部署。

`scripts/build-xnu-4k-baseline.sh` 在公共构建树内读取官方
`patches/xnu/*.patch`，同名文件优先采用设备 `patches/t733-xnu/` 中的版本。
设备新增补丁由 `patches/t733-xnu/additions.list` 明确列出。两者合并后按
文件名排序应用，随后应用 `kits/NativePrelink/patches/*.patch`。
原来设备复制的 48 个未改公共补丁已移除，公共目录由官方模块提供。

设备保留的五个公共补丁替换为：

- `offline-component-load-probes.patch`
- `offline-owner-systemos-mount.patch`
- `zzy-native-gpu-addon-mount.patch`
- `zzy-staged-system-apps.patch`
- `zzz-boot-progress.patch`

官方 `1a01c2a` 的签名缓存分块读取、union 下层目录判断、lookup 记录和
sandbox set-properties 补丁直接从公共目录读取。GPU addon 的同名替换已合入
上游创建 `/private/var/ngpu` 本身的修改，设备 Data 持久化处理保留。
`prepare-xnu-source.py` 仍只恢复缺失头文件；它已合入官方 GitHub token 请求，
保留 raw 下载失败后的 API 回退与 `UNIXNU_RECORD_HASHES=0` 支持。

## 诊断配置

`CONFIG_XNU_EARLY_ENTRY_MARKER=y` 对应构建变量 `XNU_EARLY_ENTRY_MARKER=1`，
加入 `early-entry.list` 中七个补丁，并向受影响编译单元传入
`kits/EarlyEntry` 头文件路径。入口彩条、MMU、早期 C、VM、异常、额外
framebuffer 页表与早期 panic 格式化均在这一组。系统与平台 defconfig 都保持
`n`，正常生成的 XNU 工作源码不应用这一组。

`CONFIG_XNU_NATIVE_USB_DEBUG=y` 同时选入 `diagnostics.list`：进程异常及用户栈、
用户页表属性记录、SMP 入口阶段统计。两个 defconfig 都保持 `y`，USB 日志和
`tasks` 调用栈采样继续保留。`native-process-logs` 启动参数仍控制进程诊断输出。

`native-smp-cpu-started.patch` 单独修正 `cpu_machine_init` 的局部 `started`，
不依赖统计组。`native-t733-managed-rt-memory.patch` 单独处理 managed RT DRAM
的 Normal NC 属性，页表读取输出位于诊断组。现有 DC ZVA 用户空间兼容修补、
停服务项和 Data 配置由板级配置维护，本次补丁拆分保留这些设置。

## 工作源码更新

构建脚本记录所选补丁的相对路径、字节数、修改时间与 XNU 基础提交，
不计算文件确认哈希。首次切换至此目录布局、补丁内容元数据变化或诊断组变化时，
脚本把原 `.cache/xnu-work` 完整保存为
`.cache/xnu-work-before-patch-update-TIMESTAMP`，然后从 `refer/xnu` 创建工作副本。
补丁全部应用成功后才写入选择记录；选择未变时复用工作副本及其本地修改。
保存目录可用于取回尚未整理进补丁的源码修改。

本次仅更新这一构建行为，没有执行工作副本移动、清缓存或编译。
