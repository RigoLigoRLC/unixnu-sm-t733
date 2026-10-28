# A642L GPU 硬件采集 64

2026-10-29。本轮调查 GPU 执行、shader 活动、缓存与 pipeline 等待、GMEM 搬运；CPU 耗时分析和 DVFS 调整暂缓。

## 交付

`system-gpu-hardware-64` 九阶段构建均退出 0。MSC 已完整备份旧 FAT，再写入 536870912 字节并完成 sync，返回 `write-fat completed`。Data 保留。启动后正常合成约 4 分钟，开启采样后发生黑屏复位，暂停该版采样。尚未取得导出的有效硬件计数。

## 首次窗口故障

上海时间 00:39:39.782 开始窗口；约 2 秒后的状态记录 97 个内存样本、33 个已收集 job、1 个 pending job，丢弃数 0。00:39:43.426 为最后收到的字节，00:39:49.723 USB 断开。用户报告滑动中黑屏死机复位，之后停在固件菜单或 MSC。尾部没有本次 panic 或 GPU fault。完整原始资料保存在工作区 `logs/gpu-hardware-64-crash-1/`。

源码审计找到两处具体访问协议错误：新增 CPU 时间读取用了 CP AO `0x980/0x981`；同机原厂 `adreno_a6xx.c:a6xx_read_alwayson` 在 GMU 启用时明确选择 GMU AO。周期性能计数读取还缺少原厂 `adreno_perfcntr_active_oob_get/put` 的 `oob_perfcntr` 所有权。提交前 CPU 时间读取位于提交锁内；若 Device 总线读取不返回，完成回收和后续超时检查会被挡住。具体阻塞访问尚无现场 PC 证据，正在按原厂访问协议修正。

独立 PM4 审计确认新增包字段、长度、时间戳槽范围及正常 fence 后回收与固定参考路径一致，暂未发现越界证据。修正版必须覆盖周期采样和提交前计时的全部 CPU 寄存器访问；GMU AO 与 CP 时间在没有同步依据时不得直接相减。

## 64a 修正源码

Adreno6xx `83a0453` 将两处 CPU 时间读取统一改为 GMU AO `0x1f888/0x1f889`。GPU PM4 继续读取 CP `0x980`。同机原厂 ringbuffer 的 CPU submit tick 经 `a6xx_read_alwayson` 取得 GMU AO；GPU end timestamp 由 CP 写入，dispatch 直接计算两者差值。本机沿用该已确认的计算关系，另用 `queue_valid` 和 `rb_valid` 分别标记两种区间。

USB start 只排入窗口请求。GPU worker 使用 A642L 实际 GMU 分支的 OOB 请求 bit28、ACK bit30、释放 bit29；100 ms 内取得 ACK 后才允许采集。窗口期间持续持有，停止、截止或容量达到上限后等 pending job 退休再释放。fault 先禁采，停止 CP 后清请求，GMU reset 成功后清剩余软件所有权。OOB 失败时禁止计数器访问并进入既有故障处理。运行期不再读取 selector。新增 PM4 仍为每任务 9 dword，无新增 WFI。

同机寄存器与地址换算已独立对照，构建和修正后的实机结果待记录。故障前内存记录计数不作为硬件计数采集成功证据。

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
