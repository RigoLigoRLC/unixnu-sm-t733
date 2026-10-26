# SM-T733 驱动状态与后续清单

更新日期：2026-10-27。最新部署为 `system-gpu-bo-54`，已启动并实测 BO map 改善，
配套 UEFI 为 `memory-input-display-46`。本页按实际可用功能记录完成状态；
后续项目完成后使用 `- [x] ~~项目~~`，补充日期及对应源码。

## 当前可用功能

| 功能 | 当前状态 | 设备仓库源码 |
| --- | --- | --- |
| CPU、中断与时钟 | 八核在线，每核 timer 和 IPI 已有实际计数 | [ArmCpu](../drivers/ArmCpu/ArmCpu.cpp) |
| 内存 | 46b 原生 profile 记录总物理内存 4548722688 字节，约 4.236 GiB | [DarwinBoot](../drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c)、[ADT](../scripts/make-gts7fewifi-adt.py) |
| 物理按键 | SPMI v5/PDC 中断与三键 HID，已记录完整 DOWN/UP | [SPMI](../drivers/NativeQcomSpmi/NativeQcomSpmi.cpp)、[按键](../drivers/NativePmicKeys/NativePmicKeys.cpp)、[板级配置](../package/T733PmicKeys.cpp) |
| CPU 调频与异构调度 | 45e 已实测 E0/P1/prime P2 八核上线、perfcontrol 通知和 27 路有效温度，45f 保留并修复 UFS 入队超时 | [调频 worker](../package/T733CpuPower.cpp)、[调度回调](../package/T733CpuPowerStartup.c)、[温度限制](../package/T733Thermal.cpp) |
| 系统存储 | 原生 UFS 读取系统卷，96 GiB Data 可写并跨重启恢复，已有 FUA 和 flush | [NativeUfs](../drivers/NativeUfs/NativeUfs.cpp)、[Data 持久化](../drivers/NativeRamBlock/PersistOverlay.h)、[固定布局](../package/data-layout.toml) |
| 桌面与显示 | 系统Metal合成与DPU持续直接扫描；54用户确认明显流畅，首次打开界面和重复手势仍有小卡顿，map平均56.4 us | [NativeLegacyDisplay](../drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp)、[DPU扫描](../drivers/NativeMobileDisplay/NativeDpuSurface.cpp)、[54实测](GPU_BO_54.md) |
| GPU 命令与离屏渲染 | 50c内核和用户态CP回读成功、faults=0；Turnip填充1024字、计算1024字、三角形512像素，VkResult=0、退出0 | [Adreno6xx](../drivers/Adreno6xx/README.md)、[硬件启动](../drivers/Adreno6xx/NativeAdrenoService.cpp)、[GPU实施证据](../../../notes/GPU_IMPLEMENTATION.md) |
| 触屏 | FT8203 固件上传、真实触点和 HID 已接通，可操作设置及桌面 | [触屏 worker](../package/T733Touch.cpp)、[HID](../package/T733TouchHid.cpp)、[FT8203](../drivers/NativeFocaltechTouch/Fts8203.c)、[GENI SPI](../drivers/NativeGeniSpi/GeniSpi.c) |
| 电池 | SM5714 电量、电压、电流及充满状态；充电插拔图标立即更新 | [电池服务](../package/T733Battery.cpp)、[电量计](../package/T733FuelGauge.cpp) |
| USB 调试 | CDC 日志、命令、指定进程线程栈采样；内存日志与启动日志保存 | [CDC](../drivers/NativeUsbDebug/native_usb_debug.c)、[日志](../drivers/NativeUsbDebug/native_usb_log.c)、[线程采样](../drivers/NativeUsbDebug/native_usb_thread_snapshot.c)、[BootLog](../package/BootLog.c) |
| 平台启动 | PMGR 服务满足当前启动依赖，系统已通过 Setup 进入桌面 | [平台电源服务](../package/ShengPlatformPower.cpp)、[watchdog 启动策略](../package/BootWatchdogPolicy.c) |

38a 中设置首页和关于本机最终均能打开，仍有加载延迟。当前软件绘制省略
部分 CoreImage 形状滤镜和 blendOver 效果。GPU命令执行、Turnip Vulkan离屏渲染、
系统Metal合成和DPU扫描已接通；显示性能、正常关机和完整电源管理继续记录。
设置延迟按实际用户线程采样继续定位。

UEFI MSC 已能更新启动 FAT。MSC 实现和错误恢复属于独立 UEFI 仓库；
日常驱动更新只替换 FAT，保留系统卷和现有 Data，写入及同步完成后完整
重启固件。仍有端点错误恢复缺口，不调用 eject。

## 1. 公共 GPIO 中断、物理键和触屏

- [x] ~~将 TLMM INTID240 移到公共 GPIO 中断控制器，提供按 pin 注册、触发、屏蔽、应答和重新使能。~~ 2026-10-24，`package/T733Tlmm.cpp`，39b 已构建部署。
- [x] ~~将电池 GPIO54 迁到公共接口，保留 worker 读清 SM5714 状态后重新使能的顺序。~~ 2026-10-24，39b 初始化与采样成功。
- [x] ~~将触屏 GPIO81 的 1 ms 锁存轮询改为中断唤醒，保留 SPI 读取、触点释放和 HID 提交。~~ 2026-10-24，39b 已记录 GPIO81 交付和 HID DOWN/UP。
- [x] ~~记录触控与本轮充电插拔同时工作时的实际结果。~~ 2026-10-24，用户确认都正常；GPIO54 delivered=1、GPIO81 delivered=60。
- [x] ~~实现 SPMI PMIC arbiter 事务、版本识别、PPID/APID 与所有权处理。~~ 2026-10-24，46b 实际 v5、321 次事务零错误，NativeQcomSpmi。
- [x] ~~接入 PMK8350 PON 电源键/音量下、PM7325 GPIO6 音量上，完成去抖、按下/释放、HID 和 PDC INTID513 路径。~~ 2026-10-24，46b 记录 20 条完整按下/抬起报告，NativePmicKeys 与 T733PmicKeys。

公共 ISR 按 pin 屏蔽和交付，worker 完成设备事务后重新使能。GPIO81
保留屏蔽期间的边沿锁存，GPIO54 在低电平清源后重新使能。
XNU 三键已交付 HID，电源键对应的真实面板熄屏按第二批继续。

## 2. 亮度、熄屏和显示/触控恢复

- [x] ~~实现 DSI FIFO、MAX77816 I2C、系统 brightness-nits 与原始 swap 亮度入口、触屏睡眠及恢复源码并构建部署。~~ 2026-10-24，46b/46c；NativeQcomDsi、NativeGeniI2c、T733Display 和 NativeLegacyDisplay。
- [ ] 补齐当前 DSI 视频链的命令发送，按本机面板 DCS `0x51/0x53` 控制亮度。
- [ ] 管理面板 GPIO44、buck GPIO96/97 和 PMIC 电源引用，协调触控共享供电。
- [ ] 接通显示 PM 状态与背光开关，完成触控睡眠、恢复、必要的固件重载和全触点释放。
- [ ] 接通亮度滑条、电源键锁屏后的真实熄灯及唤醒画面/触控。

本批先处理显示与触控的局部电源循环，整机休眠依赖后续存储、USB 和 CPU 恢复。

## 3. UFS 与 USB 持续运行

- [x] ~~UFS 接入 INTID297、有界队列、每槽 DMA 和超时任务 abort。~~ 2026-10-24，39b 实机峰值在途 2、13841 次完成且失败 0。默认 31 个普通槽，slot 0 留给 panic；超时失败隔离仍可能 DMA 的内存。
- [x] ~~Data 非重叠页范围并发，保留数据 FUA、位图提交及 flush 屏障。~~ 2026-10-24，39b 实机 Data 峰值并发 3。
- [ ] 补齐链路/控制器复位和时钟、PHY、HCE 独立初始化，记录实际恢复结果。
- [x] ~~接通 UFS PM/HIBERN8、Data/BootLog 停写排空、flush/SSU 与 PSCI 前完成检查。~~ 2026-10-24，40c 已构建部署，`NativeUfs/PowerLifecycle.h`、`package/ShengPlatformPower.cpp`。
- [ ] 记录正常关机、休眠/恢复和 Data 保留的实际结果。
- [ ] 独立 UEFI MSC 完成 BOT reset、失败传输和端点重新配置，处理旧完成事件。
- [x] ~~实现 CDC 端点停止、控制器停止/复位和 worker 重连。~~ 2026-10-24，40c 已构建部署并从新启动取得日志。
- [ ] 记录 CDC 拔插恢复，补完整 panic backtrace 的发送收尾。
- [x] ~~实现 CDC ECM 和 NativeUsbNetwork ifnet，接通 USB 网络。~~ 2026-10-24，41a 起部署；Mac 已枚举 en9，设备 unet0 attach=0、link=1；44b 已建立真实 LLDB 会话。
- [x] ~~接通 addon 安装、helper 监听和连接 FD 直接 exec debugserver。~~ 2026-10-24，44b 实际收到 vAttachOrWait，随后系统进程 task-port 授权被 AMFI 拒绝。
- [x] ~~实现本地 lockdown 开关设置及开发签名 debugserver。~~ 2026-10-24，44d 已统一构建并完整写入 512 MiB FAT、sync 成功。开发副本保留原权限，仅加 task_for_pid-allow，并使用独立 category 5 trust-cache 条目；输入与签名过程见 [BUILDING.md](BUILDING.md)。
- [x] ~~记录 44d 正常 lockdown API 开关读回，以及实际附加系统进程、全线程栈和 detach。~~ 2026-10-24，helper 第三次 confirmed=1；LLDB attach SpringBoard PID27，thread backtrace all 含原始库与 ObjC 方法名，detach 成功、quit 退出0。工作区证据为 logs/t733-lldb-system-native-debug-44d.txt，本次符号目录为 out/t733-lldb-symbols。
- [x] ~~取得原生 lockdown 未配对状态和首次配对拒绝的具体原因。~~ 2026-10-24，44d 返回 paired=false、17.7.11、iPad7,1；普通 DevicePublicKey 请求即返回 GetProhibitedError。原始 21H461 的普通 Pair 和 PairCU 最终共同要求 type1/type5，现有 ECM TCP 为 type3。EnableWifiConnections 已完成网络接入，完整配对需另外接入真实 USB mux 或 RSD 传输。
- [ ] 完成真实 USB mux 或 RSD 的首次配对授权，接通受配对保护的系统日志、崩溃报告及后续 Xcode 服务。原生 debugserver/LLDB 已完成，CPU profiling 继续沿该入口实施。
- [ ] 在 SM5714 PD/CC、VBUS 和 PHY 控制就绪后接 USB host，明确停止 DMA、切换角色及恢复 device 日志的顺序。

UFS 与 USB 可以分别开发，合并后统一构建。USB host 与 CDC 使用同一 DWC3
控制器，需要明确所有权和角色切换。

## 4. 温度、充电和 watchdog

- [x] ~~接 SoC TSENS 温度采样和 CPU 温度限制。~~ 2026-10-24，45e 已取得 27 路有效温度并解除初始限制，实际过温触发和冷却解除待记录，见第 7 节。
- [ ] 接 PMK8350 ADC7 电芯、充电及板温通道和换算表。
- [ ] 依据温度、供电源和原厂限制控制 SM5714 输入限流、充电电流/电压及故障处理。
- [ ] 完善 I2C 串行化、超时与恢复，新增 GENI I2C 时保留现有电池读数和插拔事件。
- [ ] 实现 SM7325 硬件 watchdog 和 IOWatchdog 用户态接口，完成启动、停止、超时与喂狗后启用 watchdogd。
- [ ] 接通系统关机、重启和 UFS 同步后的平台复位。

当前电量显示可用，充电策略尚未接管。当前 PMGR 服务和暂停 watchdogd
解决了启动依赖，硬件电源后端与 watchdog 驱动仍按本清单推进。

## 5. Wi-Fi 与蓝牙

- [ ] WCN6750 Wi-Fi：电源/RPMh、WPSS 固件、SMEM/SMP2P、QMI/QRTR、SMMU/MSI 和数据队列。
- [ ] 接 XNU 网络数据路径和 WLAN 扫描、连接、认证及状态接口，保留本机校准输入。
- [ ] WCN6750 Bluetooth：明确本机 HCI 传输与启动顺序，接总线、流控、复位、睡眠/唤醒及系统接口。
- [ ] 协调 Wi-Fi 与蓝牙共用的供电和控制信号。

本机 Android 已识别 WCN6750，Wi-Fi 使用 ICNSS2 路径。无线需要固件服务与
网络接口共同实现，工作量大于现有 SPI 触屏的中断迁移。

## 6. S Pen、传感器与音频

- [ ] Wacom W9021：GENI SE13 I2C、地址 `0x56`、固件和中断，接坐标、压力、倾斜及笔事件。
- [ ] 从本机 registry、总线及 DSP 配置确定实际传感器，优先加速度和旋转。
- [ ] 双 CS35L45 扬声器：控制总线、功放固件/校准、LPASS/TDM、所需 ADSP/APR 服务与系统 PCM 输出。
- [ ] 按本机实际 codec 和录音路径接麦克风。

传感器资料仍包含多个候选型号；共用设备树中 disabled codec 节点不代表本机
已绑定的器件。板级输入入口为 [stock-hardware.toml](../package/stock-hardware.toml)
和 [firmware-inputs.toml](../package/firmware-inputs.toml)。

## 7. 异构调度、调频与整机休眠

- [x] ~~接三域 EPSS LUT、SDPM 投票、LMH 限制和 200 ms 在线 CPU 负载调频。~~ 2026-10-24，40c，实际频率和硬件周期计数一致。
- [x] ~~接 XNU perfcontrol 的每核 QoS、运行状态和调度延迟，由单一 EPSS worker 调整频率。~~ 2026-10-24，45c 构建、部署完成；[回调](../package/T733CpuPowerStartup.c)注册 v6 `oncore`、`csw` 和 `state_update`，只记录原子数据。[worker](../package/T733CpuPower.cpp)每 10 ms 消费需求，保留每 200 ms 更新一次的负载目标，等待本轮启动后的频率响应实测。
- [x] ~~修改 ADT、ArmCpu 和 XNU 集群配置，使用 Edge 建立 E0(CPU0..3)、P1(CPU4..6)、P2 prime(CPU7) 三调度集。~~ 2026-10-24，45c 构建、部署完成，三个 EPSS 域保持独立；实际线程放置与迁移待本轮启动采集。
- [x] ~~接 TSENS 温度限制并与硬件 LMH 上限合并。~~ 2026-10-24，[NativeQcomTsens](../drivers/NativeQcomTsens/NativeQcomTsens.cpp)与[T733Thermal](../package/T733Thermal.cpp)随 45c 部署，温度限制触发与解除待实测。
- [ ] 接 idle 和 CPU 上下线，完善 SM7325 PMGR 后端。
- [ ] 显示、触控、UFS 和 USB 各自恢复就绪后，接系统 suspend/resume、PDC 唤醒及驱动恢复顺序。

调频 worker 按实际运行 CPU 所属的 EPSS 域汇总需求，短峰保留约 30 ms，普通降频等待约 300 ms。
负载目标只在 200 ms 采样时重新计算；每次 10 ms 周期将它与调度需求取较高值，再应用 LMH 和温度上限。
`dvfs` 输出每核 class、QoS、延迟与域请求，同时输出 TSENS 温度和限制状态。

温度 worker 每 200 ms 采样。原厂 CPU trip 为 E 域 110 C、P 域 108 C，解除温度为 100 C；
本移植在达到 trip 时请求最低合法普通 LUT 档。初始化缺少有效温度、连续 1 秒无完整传感器覆盖时也请求最低档，
解除限制要求相关传感器全部有效且达到解除温度。共享 APC1 温度限制同时作用于 P1 和 prime 域。

45c 首次启动暴露 Edge 在目标调度簇尚未注册时访问空指针，45e 加入启动簇回退后进入用户空间。
八核已按 4+3+1 注册，perfcontrol 通知持续增加，两组 TSENS 有效掩码为 `7fff/0fff`。
该轮 CPU 分组温度最高 46.1/46.1/48.8 C，初始温度限制解除，实际过温尚未发生。
45e 随后出现软件入队期限混用导致的 UFS 超时；45f 已五阶段构建并完整写入 536870912 字节、sync 成功。
45f 实机累计完成 1213669 次 UFS 请求、失败和两类超时均为 0、峰值在途 16。
原生 debugserver 已完成 30 秒 CPU/线程/内存采集并正常 detach，产物为工作区 `out/t733-native-profile-45f/`。
运行证据见工作区 `notes/CPU_POWER_IMPLEMENTATION.md`、`notes/THERMAL_IMPLEMENTATION.md` 和 `notes/UFS_IMPLEMENTATION.md`。

## 8. GPU 和其余外设

- [x] ~~Adreno 642L供电/时钟、GMU/HFI和A660固件启动，GPU及LPAC独立SMMU context与启动页映射。~~ 2026-10-25，50a实机通过；Adreno6xx/NativeAdrenoA6xxPower.cpp、NativeAdrenoA6xxGmu.cpp、NativeAdrenoGpuContext.cpp。
- [x] ~~CP消费ME_INIT、PAS13认证、退出secure mode，以及内核scratch写入和fence回读。~~ 2026-10-25，50a记录RPTR12/12、14/14，scratch=0x6421cafe、fence1/1；NativeAdrenoService.cpp、NativeAdrenoA6xxZap.cpp。
- [x] ~~用户态经NativeAdrenoGpuPublished分配/映射BO、提交CP_MEM_WRITE、等待fence和回读。~~ 2026-10-25，50a四个值全部匹配、faults=0、t733-adreno退出0；tools/gpu/t733-adreno.cpp、NativeAdrenoUserClient.cpp。
- [x] ~~Turnip Vulkan离屏填充、计算着色器和三角形渲染。~~ 2026-10-25，50c记录fill_words=1024、compute_words=1024、triangle_pixels=512、other_pixels=0，stage=0、VkResult=0、compute_stage=0、render_stage=0、进程退出0；DeviceInfo flags=1、UBWC=3、swizzle=6、macrotile=0，见[GPU实施证据](../../../notes/GPU_IMPLEMENTATION.md)。
- [x] ~~接NativeMetal与系统Metal合成。~~ 2026-10-25，51e实际提交，51j采样1509批全部完成；Adreno6xx/NativeAdrenoAccelerator.cpp、公共AdrenoMetal插件。
- [x] ~~接DPU直接扫描IOSurface，完成fence、双缓冲和vblank切换。~~ 2026-10-25，51j持续flip1200，用户确认无闪烁；NativeMobileDisplay/NativeDpuSurface.cpp、NativeLegacyDisplay.cpp。
- [x] ~~重构 BO 地址与物理范围管理、增量更新页表并复用完成的 BO。~~ 2026-10-27，54实测13,294次map平均56.4 us，单页58.4 us，用户确认明显流畅；[实现和数据](GPU_BO_54.md)。
- [ ] 缩减每次提交的全体 WC 页检查和导入 BO 缓存维护，改善小 BO 复用、集中回收及首次 pipeline 创建。54最近256次提交准备平均7.26 ms，缓存相关两段占98.17%。
- [ ] 改善Metal提交和显示等待开销，记录稳定帧率及正常关机结果。
- [ ] 摄像头和其他外设按实际使用需求分别实施。

50c实机证据为工作区 `logs/t733-usb-live/t733-20261025T081340.968881Z.raw`；
逐轮结果见[GPU实施](../../../notes/GPU_IMPLEMENTATION.md)，LPAC与ZAP故障调查见
[首次取指复核](../../../notes/GPU_FIRST_FETCH_AUDIT_2026-10-25.md)。
普通更新复用固定Data种子、容量和位置。
