# 每帧生成与显示时间

2026-10-28。本轮把原生显示帧 ID、合成起点、Surface 就绪、GPU 写入提交和
DPU 切换观察保存在有界记录中，查询时输出。热路径不逐帧打印。

## 时间含义

所有输出时间统一为 continuous clock 的微秒值。

- `composition_start_us`：QuartzCore 完成 `begin_update` 时保存的时间，随后开始
  这一轮合成。更早的场景准备和该调用内的缓冲分配在这个时间点之前。
- `swap_issued_us`：原生 framebuffer 接收交换请求时记录的时间。
- `read_fence_active_us`：第一次观察到该 Surface 的原生读取 fence 已激活，
  表示前面的写入已经允许显示读取。包含主机观察延迟。
- `producer_ready_us`：现有全局 GPU 安全等待完成的观察时间，单独保留。
- `gpu_complete_observed_us`：直接写入该 Surface 的 GPU 提交完成观察时间。
  提交列表只列直接写入者，不把中间离屏纹理的所有依赖都认定为同一帧成员。
- `dpu_submit_us`：DPU CTL flush 写入前的时间。
- `display_lower_us`、`display_upper_us`：实际扫描切换发生的观察范围。

原始数据保留有效位、错误状态、记录缺失和截断信息。没有真实起点时不以交换
请求到达时间代替。每帧生成耗时从合成起点到 Surface 就绪观察计算，不累加
各笔提交含有重叠排队时间的延迟。

## 原生时间来源

QuartzCore `IOMFBDisplay::begin_update` 在 `0x189509858` 读取 absolute host
time，经 `CATimeWithHostTime` 后存入对象 `+0x66f0`。`finish_update` 在
`0x18950258c-0x1895025b4` 读取并通过 timestamp type 2 发送。
IOMFB 根据客户端时钟选择完成 continuous 转换。内核
`0xfffffff00678f75c-0xfffffff00678f768` 把 wire element 1 存入 request `+0x2e0`。
该位置已经是 continuous ticks，读取时不再重复转换。

request `+0x2d8` 在交换接收路径另外采样，是 issued time。
ABI 模板 `OriginalLegacyFramebufferABI.h.in` 只新增读取 accessor，保留原始
对象和完成报告布局。

## 关联容量与缺失处理

GPU 保留 4096 个写入事件、256 个提交完成记录，每帧最多输出 64 对提交序号和
fence。32 个来源描述符缓存按 BO 代次保持身份，每个来源最多关联 16 个别名。
首次匹配比较完整物理范围及长度；后续同一来源使用缓存，不逐帧重走页面。
导入只覆盖 plane 或子分配而长度不相等时，返回没有匹配，不把部分相交当成整帧。

BO 解除映射会释放对应描述符缓存。历史覆盖、未知访问、来源释放、输出截断和
缺失完成观察均有独立标志。主机导出不会把缺失值解释为零耗时。
`match_cache_hit`、`match_segments`、`match_us` 保留关联操作自身的开销。

## 查询与导出

使用现有 USB `gpu` 查询，读取 `T733 frame-profile:` 帧记录及
`T733 frame-write:` 写入提交记录。显示记录容量为 512 帧，重复查询按连接内
sequence 去重，捕获间隔须小于记录被覆盖所需时间。

```sh
.venv/bin/python tools/t733-usb-console.py --output logs/t733-usb-live --command gpu
python3 sources/UniXNU-gts7fewifi/tools/gpu/export-frame-profile.py logs/t733-usb-live/SESSION.raw --output logs/frame-profile-61
```

导出产生 `frames.csv`、`frames.jsonl` 和 `summary.json`。数据保持逐帧粒度，
显示间隔只在相邻完整记录之间计算。`generation_observed_us` 表示生成完成的
Surface fence 观察耗时；`generation_gpu_observed_us` 单独表示直接写入者全部
完成的观察耗时，仅在匹配完整且没有未知写入、历史缺失等标志时计算。
没有原生 Surface fence 的请求不伪造其激活时间。显示等待和显示间隔保留上下界。

现有 fence 等待的参考对照另见[等待审查](GPU_FENCE_WAIT_AUDIT_2026-10-28.md)。

## 构建与部署状态

`system-gpu-frame-profile-61` 已完成统一构建的九个阶段。源码为设备 `1727fdc`、
Adreno6xx `bdb86b8`、NativeMobileDisplay `b21111d`；core 保持 `7eefbce`、
SoC 保持 `1829984`。不可移动源码 tag 为
`build/system-gpu-frame-profile-61-source-1`。

产物位于 `build/releases/system-gpu-frame-profile-61/`，完整版本与执行命令见
`source-revisions.json`，各阶段结果见 `stages.tsv`。本轮没有新增 XNU 补丁或
更改 series，构建仍选中既有 `native-t733-managed-rt-memory.patch`。

2026-10-28，用户进入 MSC 后已刷入。服务完整写入 536870912 字节并完成 sync，
返回 `write-fat completed`，Data 保留。部署日志位于工作区
`logs/msc-20261028-030814-system-gpu-frame-profile-61/`，旧 FAT 位于
`backups/msc-20261028-030814-system-gpu-frame-profile-61/payload-before.img`。
被动日志采集已开启，等待完整重启；新增逐帧记录尚无设备采样结果。
没有运行额外测试。
