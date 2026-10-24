# SM-T733 驱动状态与后续清单

更新日期：2026-10-24。当前实机基线为 `system-graphics-memory-38a`，
配套 UEFI 为 `msc-recovery-28`。本页按实际可用功能记录完成状态；
后续项目完成后使用 `- [x] ~~项目~~`，补充日期及对应源码。

## 当前可用功能

| 功能 | 当前状态 | 设备仓库源码 |
| --- | --- | --- |
| CPU、中断与时钟 | 八核在线，每核 timer 和 IPI 已有实际计数 | [ArmCpu](../drivers/ArmCpu/ArmCpu.cpp) |
| 系统存储 | 原生 UFS 读取系统卷，96 GiB Data 可写并跨重启恢复，已有 FUA 和 flush | [NativeUfs](../drivers/NativeUfs/NativeUfs.cpp)、[Data 持久化](../drivers/NativeRamBlock/PersistOverlay.h)、[固定布局](../package/data-layout.toml) |
| 桌面与显示 | CPU 软件合成和帧复制，38a 图库返回桌面正常 | [NativeLegacyDisplay](../drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp)、[显示注册](../package/T733Display/DisplayRegistration.cpp) |
| 触屏 | FT8203 固件上传、真实触点和 HID 已接通，可操作设置及桌面 | [触屏 worker](../package/T733Touch.cpp)、[HID](../package/T733TouchHid.cpp)、[FT8203](../drivers/NativeFocaltechTouch/Fts8203.c)、[GENI SPI](../drivers/NativeGeniSpi/GeniSpi.c) |
| 电池 | SM5714 电量、电压、电流及充满状态；充电插拔图标立即更新 | [电池服务](../package/T733Battery.cpp)、[电量计](../package/T733FuelGauge.cpp) |
| USB 调试 | CDC 日志、命令、指定进程线程栈采样；内存日志与启动日志保存 | [CDC](../drivers/NativeUsbDebug/native_usb_debug.c)、[日志](../drivers/NativeUsbDebug/native_usb_log.c)、[线程采样](../drivers/NativeUsbDebug/native_usb_thread_snapshot.c)、[BootLog](../package/BootLog.c) |
| 平台启动 | PMGR 服务满足当前启动依赖，系统已通过 Setup 进入桌面 | [平台电源服务](../package/ShengPlatformPower.cpp)、[watchdog 启动策略](../package/BootWatchdogPolicy.c) |

38a 中设置首页和关于本机最终均能打开，仍有加载延迟。当前软件绘制省略
部分 CoreImage 形状滤镜和 blendOver 效果。GPU 加速、完整电源管理和所有
外设尚未接通；设置延迟按实际用户线程采样继续定位。

UEFI MSC 已能更新启动 FAT。MSC 实现和错误恢复属于独立 UEFI 仓库；
日常驱动更新只替换 FAT，保留系统卷和现有 Data，写入及同步完成后完整
重启固件。仍有端点错误恢复缺口，不调用 eject。

## 1. 公共 GPIO 中断、物理键和触屏

- [ ] 将 TLMM INTID240 从电池驱动移到公共 GPIO 中断控制器，提供按 pin 注册、触发、屏蔽、应答和重新使能。
- [ ] 将电池 GPIO54 迁到公共接口，保留 worker 读清 SM5714 状态后重新使能的顺序。
- [ ] 将触屏 GPIO81 的 1 ms 锁存轮询改为中断唤醒，保留 SPI 读取、触点释放和 HID 提交。
- [ ] 实现 SPMI PMIC arbiter 事务、版本识别、PPID/APID 与所有权处理。
- [ ] 接入 PMK8350 PON 电源键/音量下、PM7325 GPIO6 音量上，完成去抖、按下/释放、HID 和 PDC INTID513 路径。

当前电池 ISR 会屏蔽其他 enabled、APPS、pending 的 GPIO 来源。GPIO81
尚未开启 summary-enable；接触屏中断必须同时完成公共控制器和电池迁移。
UEFI 菜单已有物理键输入，XNU 中的按键实现仍属本批工作。

## 2. 亮度、熄屏和显示/触控恢复

- [ ] 补齐当前 DSI 视频链的命令发送，按本机面板 DCS `0x51/0x53` 控制亮度。
- [ ] 管理面板 GPIO44、buck GPIO96/97 和 PMIC 电源引用，协调触控共享供电。
- [ ] 接通显示 PM 状态与背光开关，完成触控睡眠、恢复、必要的固件重载和全触点释放。
- [ ] 接通亮度滑条、电源键锁屏后的真实熄灯及唤醒画面/触控。

本批先处理显示与触控的局部电源循环，整机休眠依赖后续存储、USB 和 CPU 恢复。

## 3. UFS 与 USB 持续运行

- [ ] UFS 从单 slot 轮询转为中断完成和有界队列，明确取消、错误及完成回调。
- [ ] 补齐 UFS abort、任务管理、链路/控制器复位和时钟、PHY、HCE 独立初始化。
- [ ] 接通正常关机与驱动停止时的写入、FUA/flush 顺序，保持当前卷和 Data 布局。
- [ ] 独立 UEFI MSC 完成 BOT reset、失败传输和端点重新配置，处理旧完成事件。
- [ ] XNU CDC 完善拔插重连、传输取消和完整 panic backtrace 的发送收尾。
- [ ] 在 SM5714 PD/CC、VBUS 和 PHY 控制就绪后接 USB host，明确停止 DMA、切换角色及恢复 device 日志的顺序。

UFS 与 USB 可以分别开发，合并后统一构建。USB host 与 CDC 使用同一 DWC3
控制器，需要明确所有权和角色切换。

## 4. 温度、充电和 watchdog

- [ ] 接 PMK8350 ADC7 电芯、充电及板温通道和换算表，并接 SoC TSENS。
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

- [ ] 将当前单一同类集群描述接到本机 4+3+1 性能/频率域，补调度容量和工作分配。
- [ ] 温度管理就绪后接时钟/电源调节、idle 和 CPU 上下线，完善 SM7325 PMGR 后端。
- [ ] 显示、触控、UFS 和 USB 各自恢复就绪后，接系统 suspend/resume、PDC 唤醒及驱动恢复顺序。

## 8. GPU 和其余外设

- [ ] Adreno 642L：GMU、固件、供电/时钟、SMMU、命令队列和 fence，再接 Turnip/NativeMetal 与系统合成。
- [ ] 摄像头和其他外设按实际使用需求分别实施。

当前优先完成公共中断及物理键，再接亮度和恢复；UFS/USB 错误恢复可以独立
推进。保留 38a 为可用回退基线，普通更新复用固定 Data 种子、容量和位置。
