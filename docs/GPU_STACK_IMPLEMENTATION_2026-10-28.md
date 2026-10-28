# GPU 提交、系统格式和 Metal 接口实施

日期：2026-10-28。设备：SM-T733 / SM7325 / Adreno 642L，iPadOS 17.7.11 21H461。

分支归属：`unixnu-sm-t733` 保留本文的内核任务、timeline、IOSurface/DPU 交接、Metal 同步优化和系统 BGRA8 sRGB 格式选择；`unixnu-sm-t733-apple3` 另含深度 resolve、Metal IO、采样位置及 Apple3 功能。下文完整实施记录对应 62j，两分支使用相同的系统 SDR 格式策略。

本轮根据[提交研究](GPU_METAL_SUBMISSION_RESEARCH_2026-10-28.md)和[Metal family 研究](A642L_METAL_FAMILY_RESEARCH_2026-10-28.md)修改源码。[62j 构建记录](GPU_STACK_62J_BUILD_2026-10-28.md)保存九阶段成功结果、固定源码和待刷入状态。DVFS 和 Apple4 起的 tile/imageblock/ROG 完整实现仍按既定范围延后。

## 内核任务与 timeline

`QueryAsync` 在原有 ABI 之外声明异步扩展。内核保存最多 64 个已接受任务，每个任务保留 IB、BO 范围、输入 timeline 点和输出 signal；硬件 ring 同时容纳最多 8 个任务。每个软件 queue 保持提交次序，调度器在有空间时选择最早就绪的 queue 头部。

同一 ring 中已经发出的生产者可以满足后续任务的调度依赖。尚未满足的 host signal、未来 timeline value 和 DPU 读占用留在软件队列，由完成事件重新触发调度。空提交也在依赖及此前 queue 工作后写入完成 fence。CPU 等待使用所需完成点上的睡眠等待。

GPU 中断确认 RBBM 状态并唤醒完成 worker。worker 读取 CP 写出的 fence，退役任务、更新 timeline 并唤醒等待者；10 ms 服务轮询用于故障检查，硬件任务保留 500 ms 超时。错误先终结等待对象，在 CP 和 GMU 的 DMA 停止后回收硬件引用。

有界 `NativeAdrenoAsync` 记录关联接受、发出和完成时间，以及软件完成序号和硬件 fence。BO 的映射建立和解除更新持续存在的 backing 关系；提交处理已保存的访问范围，统计实际范围数、别名查询和依赖数。相同物理内存的 IOSurface 和 GPU BO 按这些关系交接。

## 系统创建 SDR 表面

设备 ADT 声明 sRGB。21H461 的启动期 text fixup 固定 QuartzCore 的显示能力，并让 RenderBox 普通 RGB8 自动格式选择进入现有 BGRA8 分支。MobileGestalt 缓存按策略版本失效，插件只读记录框架版本和指令状态。地址、原始指令和缓存流程见[显示格式实施](DISPLAY_FORMAT_POLICY_2026-10-28.md)。

## IOSurface 生产者与 DPU 扫描

`NativeLegacyDisplay` 为当前 IOSurface 的 `IOMemoryDescriptor` 向 Adreno 调度器申请读占用。调度器匹配持久物理 backing，记录已存在的写入任务并阻止后续 GPU 写入。没有 GPU 写入者的 CPU 表面立即就绪；GPU 写入者完成后回调只保存结果并唤醒显示 work loop。

`t733_dpu_surface_begin` 成功接收 IOSurfaceFence 和 GPU 读占用。VIG0 开始扫描后，驱动保留表面、descriptor、显示 SMMU 映射和读占用。下一张表面完成 CTL 切换时，才释放上一张表面的这些资源。

SM7325 DTS 的 MDSS 地址为 `0x0ae00000`，长度 `0x84000`，中断为 SPI 83 / GIC 115。`NativeDpuInterrupt` 使用 MDP 中断区 `+0x1000` 的 INTF1 vsync 位 27，记录 INTF1 `+0x36000` 帧号和 CTL0 `+0x16000` flush。硬中断只确认状态并通知显示 work loop。正常完成和 vsync 消息由这一通知驱动；缺少中断资源时保留定时观察路径，启用中断后的定时器用于有界故障等待。

依据为原厂 `yupik-sde-common.dtsi`、`sde_hw_interrupts.c` 和 `sde_hwio.h`。本轮不改变显示时钟、扫描时序或 DSI 配置。

## Metal 编码与提交

Metal 前端向 runtime 传递资源使用、render/compute memory barrier 和 fence 的实际阶段。资源创建时保存 CPU 可访问性，命令编码时累加本批访问阶段，提交时合并已有摘要。Private 资源之间的可见性交接继续经过 Vulkan 命令与 semaphore 的依赖；HOST 出口只覆盖本批写入的 CPU 可访问资源。

普通 draw 的固定附件顺序和串行 compute 顺序沿现有 Vulkan/Turnip 路径执行。显式 barrier、跨 encoder/queue 和外部读写保留各自要求的依赖。统计保存在有界汇总中，不逐 draw、逐资源打印。

## 深度 resolve

缺少原生 Min/Max resolve mode 时，shader 从多重采样深度中选出样本。具有已记录精确 FragDepth 写回行为的驱动沿原路径写回；其他驱动将 D32 位模式或 D16 整数码写入 R32_UINT/R16_UINT 颜色附件，再经过 GPU transfer buffer 写入目标深度 aspect。

深度与 stencil 使用不同 resolve 纹理时，每个需要 shader 选样的 aspect 使用自己的中间附件，完成后只拷贝该 aspect。深度和 stencil 的其他内容按原 store/resolve 规则保留。多重采样深度附件创建时加入 sampled usage；memoryless 深度使用普通设备内存保存当前 pass 的样本，Metal 的 memoryless API 限制继续生效。

实现保存在 AdrenoMetal 的 `0012-resolve-depth-through-integer-attachments.patch`。该改动未增加 Turnip 的 FragDepth 行为假定。

## 系统 Metal IO 队列

21H461 的 `_MTLDevice` 已提供 `newIOCommandQueueWithDescriptor:error:`、普通文件和压缩文件 handle。桥接设备以该类为基类，原生构造函数创建 `MTLResourceListPool`，原生 IO worker 读取或解压文件后调用桥接已有的 command buffer、blit encoder 和 shared event。

本轮补入 `maxIOCommandsInFlight=16`，供原生队列限制文件读取并发；桥接资源和 command buffer 基类保存稳定 `globalTraceObjectID`，供原生 IO 完成函数调用。runtime 的 `IoCommandQueues` 缺口只说明 Rust runtime 未提供该对象，前端使用上述系统实现。

静态依据：`_MTLDevice init` 为 `0x187acf7c8`，队列创建为 `0x187b5807c`，普通/压缩文件创建为 `0x187b580c4` / `0x187b5819c`，IO 队列初始化为 `0x187b84170`，读取及 blit 调度为 `0x187b838cc`，完成函数为 `0x187ae1274`。保存的反汇编在工作区 `logs/gpu-stack-policy-research-20261028/metal-native-io-*.txt`。本轮尚未执行设备上的 IO 操作。

## 默认采样位置

`getDefaultSamplePositions:count:` 经过桥接检查 runtime 支持的 texture sample count，并读取实际 Vulkan 设备的 `standardSampleLocations`。满足条件时返回 1、2、4 个样本的标准坐标，顺序与 sample index 一致；非法输出或不支持的数量产生参数错误。

坐标依据为 [Vulkan Standard Sample Locations](https://docs.vulkan.org/spec/latest/chapters/primsrast.html#primsrast-multisampling)。pass 内自定义 sample positions 继续通过 render pass 描述符传给 runtime。

## 实施状态

系统格式、Metal 同步、DPU 交接、深度 resolve、异步 job/timeline、Apple3 前端扩展、256M texture buffer、PVRTC 和 XR 格式路径已合入固定源码，62j 完成统一构建。设备当前未连接 MSC，刷入和本包运行结果待补。
