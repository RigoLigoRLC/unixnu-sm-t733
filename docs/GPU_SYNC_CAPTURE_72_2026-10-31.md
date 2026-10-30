# GPU 复制同步与受控采集

2026-10-31。72a 在 71 的 DVFS 和复制实现上合并内部复制依赖，并补充命令、pass、附件和 SYS_WFI 统计。源码位于 `unixnu-sm-t733`。

## 实现

Metal runtime 在内部 render pass 和 tile dispatch 之前，用已有的资源键、访问范围及主机内存别名记录组织 shadow copies。复制之间存在实际读写依赖时记录 barrier，独立复制连续记录，组末按实际 transfer/compute 写入阶段建立后续依赖。staged row compute 的访问也参加判断。已有附件读取和 feedback 分段保留；每次实际 render pass begin 记录 encoder、instance 和分段原因。

AdrenoMetal 的 `0026` 补丁实现上述操作，`0027` 修正首个 inline pass 初始化闭包丢弃统计返回值的调用点。类型 15 和 17 的格式见模块 `bridge/TRACE_METADATA.md`。

Turnip 从 `tu_cs_emit_pkt7` 及事件参数记录 WFI 和 EVENT_WRITE，用固定开放寻址表汇总来源、pass、command stream 和条件。提交时分别导出发包数、tile 重放估计和条件执行上限。无法取得执行倍率的命令保留未知值。附件记录区分描述中的动作和实际发出的 GMEM load/store/resolve/clear。格式见 core `patches/mesa/turnip-darwin-submitted-pass-profile.md`。

默认采集保留 render、binning、sysmem、copy 和 compute 的 GPU 时间，关闭逐 draw/tile、GMEM load/store 与 cache flush 的密集时间戳。`--detailed-tiles` 可显式开启详细模式。统计表、事件预算和队列均有界，溢出计入丢失数据。

## 采集接口

原 29 路硬件计数的记录布局保留。SYS_WFI 使用 CP4、事件 25，通过原有 OOB 所有权路径采样；附加定义和样本放在 sections 6/7。

新增方法 41 接收 32 字节控制参数，42 返回 32 字节采集选项。固定频率可选 315/450/550 MHz，并沿用对应 DDR7 vote 和温控上限。窗口结束后等待捕获任务退休，再恢复原调频策略。GMEM/sysmem 选择作用于新记录的 pass，仍经过原有硬件适用条件。

从工作区根执行：

```sh
.venv/bin/python sources/UniXNU-gts7fewifi/tools/gpu/gpu-trace.py capture \
  --seconds 60 --frequency 550 --render-mode auto \
  --output logs/gpu-perfetto-72a-auto-550
.venv/bin/python sources/UniXNU-gts7fewifi/tools/gpu/gpu-trace.py capture \
  --seconds 60 --frequency 550 --render-mode sysmem \
  --output logs/gpu-perfetto-72a-sysmem-550
.venv/bin/python sources/UniXNU-gts7fewifi/tools/gpu/analyze-gpu-overhead.py \
  logs/gpu-perfetto-72a-auto-550 --baseline logs/gpu-perfetto-71-runtime-1 \
  --output logs/gpu-perfetto-72a-auto-550/overhead-analysis.json
```

每次使用新目录，并在采集真正开始后通知用户操作。导出保存 `trace.pftrace`、`trace.metadata.jsonl`、附加计数器二进制和实际采集选项。有效的零计数增量也算已采集测量；命令数量、硬件周期和 GPU 墙钟时间分别统计。

## 构建与部署

72 的 Metal 阶段因新增返回类型遗漏一个调用点而失败，未部署。72a 九阶段构建全部退出 0，已通过 MSC 完整写入 536870912 字节 FAT 并 sync；部署日志在工作区 `logs/msc-20261031-021814-system-gpu-sync-72a/`。启动日志确认 SpringBoard、backboardd 和 GPU 初始化均已启动。

72a 的方法 42 查询返回 `0xe00002c7`：外层 IOKit 分发表漏登了方法 41/42，内部处理函数已经实现。Adreno6xx `86e0f1b` 补齐两项，72b 用此修正继续交付；72a 未启动采样。启动证据在工作区 `logs/t733-usb-live/t733-20261030T181937.194618Z.raw`。

固定 tag `build/system-gpu-sync-72a-source-1` 与源码均已推送。构建入口为 `bash tools/build-t733-recovery.sh system-gpu-sync-72a --system --payload-only --local-module soc:sm7325=/Users/rigoligo/expm/unixnu-sm-t733/sources/unixnu-sm7325-common`，完整清单在 release 的 `source-revisions.json`。

| 仓库 | 构建提交 |
| --- | --- |
| 设备 | `173dea8` |
| core | `0e40cfd` |
| SM7325 | `2f34576` |
| AdrenoMetal | `b9469e6` |
| Adreno6xx | `14d4900` |

72b 九阶段构建全部成功，完整 FAT 已写入并 sync，部署日志在工作区 `logs/msc-20261031-022507-system-gpu-sync-72b/`。固定 tag 为 `build/system-gpu-sync-72b-source-1`，设备 `5e5422c`、SM7325 `be109f1`、Adreno6xx `86e0f1b`；core 和 Metal 与上表相同。源码及 tag 已推送，等待启动与实机采集。

72b 已正常启动并完成两轮各60秒的550 MHz/DDR9采集，调档错误0，原29路计数及SYS_WFI均有有效样本，采集结束后OOB已释放。数据分别在工作区 `logs/gpu-perfetto-72b-auto-550/` 和 `logs/gpu-perfetto-72b-sysmem-550/`，更正附件GMEM布局标签的导出为 `trace-decoded.pftrace`。前者766 jobs/2707硬件样本，后者358 jobs/2776样本；producer分别丢18016/15104条，元数据表分别丢4017/1867条，主机序号、job、硬件样本和已导出阶段端点无缺失。

auto操作期保存的WFI中77.82%来自既有tile完成等待。GPU关联的320对相邻pass中319对为memoryless写入分段，316对夹有复制。类型17为0，实际桌面走inline/core复制路径；本轮没有证明0026的PassSteps复制合批带来收益。详细结果在auto目录 `auto-vs71-findings.txt`。

两轮在相同尺寸、样本数、draw数和附件动作下，全屏8draw的GMEM/sysmem均值为5.143/7.794 ms，33draw为3.802/1.816 ms，34draw为1.622/2.712 ms。shader和texture内容没有受控重放，结果支持继续按pass选择模式。匹配样本和损失记录在sysmem目录 `render-mode-comparison.json`。

72c进一步采用既有secondary记录路径处理带memoryless颜色附件的encoder，等绘制命令收集完再开始实际pass，避免提前inline开始后第一次写memoryless附件时被迫拆分。真实附件读写和feedback依赖继续由既有planner处理。该修改在AdrenoMetal `944699d`，等待构建和实机对照。

71 的基线和源码调查保存在工作区 `logs/gpu-perfetto-71-runtime-1/`。Apple3 和 CPU 性能分析继续延后。
