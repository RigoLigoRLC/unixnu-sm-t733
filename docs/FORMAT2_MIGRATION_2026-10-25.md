# SM-T733 format 2 迁移

日期：2026-10-25。官方基线为 `fb5e8ba88fcce2526e33fe8c4348d5e7aeb585d8`，公共核心固定提交为 `1d9fed8`；48f 构建使用相同运行实现，随后补齐文档检查的 SoC 参数与私有 defconfig 输出命名。本记录说明公共构建和 ADT 兼容机制；仓库固定版本以 `support/UniXNU.rev`、`support/UniXNU.upstream.rev` 和 `support/repositories.json` 为准。

`system-format2-48f` 已完成统一系统构建；536870912 字节 FAT 已完整写入并完成 sync，用户确认启动。USB 已记录八核 online=1/fail=0、UFS completed=300788/failed=0/超时=0/峰并发14、显示 ready=1/awake=1 和触点事件。产物位于 `build/releases/system-format2-48f/`，构建阶段结果见该目录的 `stages.tsv`。本轮沿用 `memory-input-display-46` BOOT/recovery 和现有 Data 布局。

## 配置和组件来源

`board.toml` 和 `board.platform.toml` 使用 `schema_version = 2`，官方配置解析器依次合并 ARM64、SM7325、`ipados-17.7.11-21H461` 和设备配置。设备包装器读取官方 `status --json` 的 `output`，将内核、菜单、payload 和交付复制都绑定到解析得到的配置输出目录。

公共 `components.catalog` 按公共、ARM64、SoC、OS、设备顺序读取各自的 `components.toml`。后层的同名条目完整替换前层元数据，模块文件仍由官方 feeds 的优先级选择。本机 `NativeUfs`、`NativeRamBlock`、`NativePrelink` 和 `NativeMobileDisplay` 保留社区仓库身份；SoC 目录的 `patches/xnu` 由 series 直接读取，组装树的 `patches/xnu` 继续指向官方公共补丁模块。

`scripts/porting-record.py check` 复用组件表读取函数，可通过 `--soc DIR` 读取已获取 SoC 的组件表，再合并 OS 和设备组件表。CI 使用固定公共核心和官方解析器取得 SoC 路径，再检查结构化记录中的本机组件 ID。

## 最终补丁列表和源码准备

两份 board 用官方允许的完整 `kernel.patch_series` 数组指定顺序：

1. `device:patches/arch/arm64/series` 引用官方公共补丁，并在原位置引用设备替换。该清单保留 `native-smp-secondary.patch` 和 `sheng-platform-power.patch`。
2. `soc:patches/xnu/series` 选择 SM7325 时钟、CPU、内存属性和调度拓扑补丁。
3. `device:patches/os/ipados-17.7.11-21H461/series` 选择本机 boot-progress 和 `NativePrelink` 两项 OS 补丁。
4. `device:patches/t733-xnu/series` 选择设备功能补丁。
5. `device:patches/t733-xnu/diagnostics.list` 选择当前配置保留的 USB 诊断。当前 early 配置为 `n`，数组未包含 `early-entry.list`。

公共 `patches.plan` 按上述 series 产生一份最终有序列表。`patches.prepare` 用这份列表应用 XNU 补丁，并将列表路径放入 `UNIXNU_PATCH_LIST`。设备 `build-xnu-4k-baseline.sh` 直接消费该变量，不再在 format 2 路径中另行扫描公共补丁和追加设备补丁。

`UNIXNU_RECORD_HASHES=0` 时，两端使用相同的 XNU 基础提交及每项补丁的绝对路径、大小、修改时间记录输入。补丁应用完成后写入 `.cache/xnu-work/.unixnu-patch-selection.json`；外部报告为 `.cache/xnu-patch-selection.json` 和 `.cache/xnu-patches.list`。公共准备步骤还保存配置输出目录的 `xnu-inputs.json` 和 `xnu-patches.list`。设备 baseline 读取同一记录，已准备的源码不会重复应用补丁。

输入变化时，源码和 `build/xnu/ios-obj` 分别保存到带时间戳的目录，旧头文件完成标记同时失效。公共 kernel 步骤先执行 baseline bootstrap，产生当前源码对应的头文件、编译记录和 `link.filelist`，随后构建组件归档并完成内核链接和 prelink。

分层后 OS 的 boot-progress 补丁先于设备诊断应用。`native-process-diagnostics.patch` 的退出日志 hunk 已以现有 `boot_progress_exit` 调用作为上下文，保留退出种类、详细退出原因和 boot-progress 调用的执行顺序。

## ADT 和启动交付

| 输入 | 公共转换器产生的 ADT 数据及消费者 |
| --- | --- |
| `payload.adt.owned_memory_size = "auto"` | `darwinboot-memory-range` 为两个小端 u64，下限 1 GiB，上限取 DRAM 大小与 8 GiB 的较小值并按 2 MiB 向下对齐；DarwinBoot 在 UEFI conventional memory 中分配实际连续窗口。 |
| `native-ufs-smmu-reg` | DTS 的四个 32 位 cell 编为物理地址和范围大小两个小端 u64，供 NativeUfs 使用。 |
| `native-ufs-stream-id`、`native-ufs-interrupt` | 各自编码为小端 u32，保留本机 UFS SID 和中断 ABI。 |
| CPU 的 `cluster-type`、`die-cluster-id`、`cluster-core-id` | 各自编码为小端 u32，交给本机调度拓扑实现。 |
| 硬件 `reg` | 保留官方非零 `arm-io` 物理窗口与子节点偏移，NativeUfs 通过总线窗口恢复物理地址。 |

公共核心继续承接已使用的菜单滚动、Samsung 电源键确认、Recovery 状态显示、额外 XNU/EFI/action 菜单项和指定 payload 文件复制。官方新增加的 Tools 菜单路径保留。

IPSW 提取、kernel prelink、payload 报告和 skip-setup 种子缓存均遵循 `UNIXNU_RECORD_HASHES=0`；种子缓存使用输入路径、大小、修改时间、工具和参数。SHNGROOT 磁盘格式所需的摘要仍由格式写入过程产生。此次迁移未执行额外测试或文件确认哈希。
