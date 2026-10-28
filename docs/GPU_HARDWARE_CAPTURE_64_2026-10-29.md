# A642L GPU 硬件采集 64

2026-10-29。本轮调查 GPU 执行、shader 活动、缓存与 pipeline 等待、GMEM 搬运；CPU 耗时分析和 DVFS 调整暂缓。

## 交付

`system-gpu-hardware-64` 九阶段构建均退出 0。MSC 已完整备份旧 FAT，再写入 536870912 字节并完成 sync，返回 `write-fat completed`。Data 保留。当前等待新包启动和操作期采样，尚未取得本包的有效硬件计数。

| 构建输入 | 固定版本 |
| --- | --- |
| 设备 | `4ea506234cc3abc15828f50375833fd497a7964f` |
| core | `1aa8c23b8100ee766e9de540ac9b3045130b1e1e` |
| SM7325 | `f67942745987ab94b36e1bb6d6a34f4395e89cdc` |
| Adreno6xx | `b762d6999d1abbf335ebad8f0abac1f3dae1554f` |
| AdrenoMetal | `cc4c26f3ba1091ecb7576d3644fdb5169db3efca` |
| 固定构建 tag | `build/system-gpu-hardware-64-source-2` |

`source-1` 是编译前的中间源码标记，未用于此镜像。实际输入和命令保存在 `build/releases/system-gpu-hardware-64/source-revisions.json`，阶段结果为同目录 `stages.tsv`。工作区部署记录在 `logs/msc-system-gpu-hardware-64/`，旧 FAT 在 `backups/msc-system-gpu-hardware-64/payload-before.img`。

```sh
source tools/activate.sh
bash tools/build-t733-recovery.sh system-gpu-hardware-64 --system --payload-only \
  --local-module soc:sm7325=/Users/rigoligo/expm/unixnu-sm-t733/sources/unixnu-sm7325-common
```

## 操作窗口

USB 命令 `gpu-profile-start` 开始固定 20 秒窗口，硬件计数每 20 ms 保存一次。`gpu-profile-stop` 提前结束，`gpu-profile` 查询状态并在停止后分批导出。主机发送 start 后每 2 秒自动读取，收到 end 后停止；180 秒未完成会记录超时。运行中只在固定容量内存保存数据，停止后每次最多导出 64 个硬件样本和 128 个 job。

```sh
.venv/bin/python tools/t733-usb-console.py --output logs/t733-usb-live --background
.venv/bin/python tools/t733-usb-console.py --output logs/t733-usb-live --command gpu-profile-start
```

新记录前缀为 `NativeAdrenoGpuProfile`。主机在每个 raw 文件旁保存 `.gpu-hardware.jsonl` 和 `.gpu-hardware.summary.json`。有效测量必须有齐全的 begin/definition/sample/job/end，且实际计数随有效样本增长；ready 只说明初始化完成。

29 路计数保留 selector、event、单位、64 位原始值和有效位。相邻有效样本才计算差值，SP 多单元累计值不直接换算成单核利用率。shader 活动与指令计数通过 SP 事件观察；本包没有指令 PC 采样。

窗口内每笔任务增加 9 个 PM4 dword：CP_REG_TO_MEM 写 CP 到达时间，RB_DONE_TS 写渲染完成时间，无新增 WFI。入队时间来自写 WPTR 前读取的同一 GPU 时钟。job 区间包含 GPU 队列和流水线重叠，分别报告入队至 CP 到达、CP 到达至 RB 完成。RB 时间只取低 32 位，按 19.2 MHz 和模差计算并限制有效范围。窗口关闭时不添加时间戳命令或周期读取。

## GPU 开销候选与采样边界

M2V 为每个 encoder 创建主 command buffer，并为入口及 HOST 收尾记录独立 barrier buffer。固定 Turnip 在多个主 buffer 的结束处清理 CCU，按访问与阶段需求发出缓存命令和等待。63c2 累计记录中，显式 Metal barrier/fence 为 0，而 922 批入口和 HOST 收尾均执行；这说明应优先计量翻译层生成的命令。实际耗时仍需硬件数据。

设备未强制 sysmem；GMEM 选择沿用固定 Mesa。多 render pass、512 KiB GMEM 的 tile load/store、少量 draw 的 sysmem 选择及慢速 store 路径均需按实际 GPU 记录区分。

部分 u_trace 点会因时间戳复制额外插入 WFI。本包没有开启这些点。可用的有限点集、FD_RD_DUMP 抓取与解码步骤保存在工作区 `logs/gpu-hardware-64-prep/command-overhead-findings.md`。
