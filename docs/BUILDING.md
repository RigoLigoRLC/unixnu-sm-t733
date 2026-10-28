# 构建与装配

设备构建采用 format 2，按 ARM64、SM7325、iPadOS 21H461 和 SM-T733 四层解析。主机需要 macOS、完整 Xcode 与 iPhoneOS SDK、Python 3.11 或更高版本、Git、make、LLVM/LLD、mtools、dtc、openssl 和 unifdef。固件还需要配套 UEFI 的工具及原厂输入。

`system-format2-48f` 于 2026-10-25 完成七阶段构建、完整 536870912 字节 FAT 写入和 sync。启动后 USB 已记录到八核上线、UFS 无失败或超时、持续显示帧和触点事件，具体数值与原始记录位置见 [48f 交付记录](archive/FORMAT2_2026-10-25.md)。迁移前的输入与迭代事实见 [构建归档](archive/BUILDING_2026-10-24.md)。

## 1. 固定源码与私有身份

从设备仓库的父目录开始：

```sh
git clone --recurse-submodules --branch unixnu-sm-t733 https://github.com/RigoLigoRLC/unixnu-sm-t733.git
cd unixnu-sm-t733
git submodule update --init --recursive
export UNIXNU_RECORD_HASHES=0
bash scripts/bootstrap-base.sh ../unixnu-core
export UNIXNU_BASE="$(cd ../unixnu-core && pwd)"
cp local.toml.example local.toml
```

`bootstrap-base.sh` 将公共 fork 检出到 `support/UniXNU.rev` 的固定提交，再初始化子模块。目标目录必须尚不存在。已有同版本公共源码时直接设置 `UNIXNU_BASE`。公共官方基线由 `support/UniXNU.upstream.rev` 记录，配套固件由 `support/Mu-Silicium.rev` 固定。

`feeds.toml` 固定公开的 [unixnu-sm7325-common](https://github.com/RigoLigoRLC/unixnu-sm7325-common) 提交，公共驱动及 Apple OSS 由对应 core feed 提供。设备有 13 个 driver 子模块和 1 个 kit 子模块；`components.toml` 保留社区 fork 身份及本机 recipe。克隆和更新设备仓库后都执行递归子模块初始化，gitlink 与组件 revision 一起更新。

54 在 51s 恢复基线上加入 BO 管理重构，身份、配对、软件加密、HUD 与触点设置保留，具体版本及范围见 [54 实施](GPU_BO_54.md)。此前恢复范围见 [GPU 回滚记录](GPU_51S_RESTORE.md)。每包使用提交后的源码和固定引用，原始 51s FAT 仍作为独立的已测产物保留。

公开 `board.toml` 与 `board.platform.toml` 的 `[payload.adt] chip_id = "733"` 是十六进制软件身份输入。每台设备的 `hardware.unique_chip_id` 填在忽略的 `local.toml`：

```toml
[hardware]
unique_chip_id = "YOUR_HEXADECIMAL_SEED"
```

示例中的字符串须替换为本机非零 64 位十六进制种子。`scripts/build.py` 读取该值并传入官方 `UNIXNU_UNIQUE_CHIP_ID`；已有同名环境变量优先。共用 DTS 转换器从此环境变量和公开 board 的 `chip_id` 生成 ADT 身份。板级 DTS 提供实际 DRAM 地址跨度，`owned_memory_size = "auto"` 继续让 DarwinBoot 按实时 UEFI conventional memory 分配连续窗口。

本地 SoC 开发可显式选择已检出的仓库，参数会传给每次官方命令：

```sh
bash scripts/build.sh system-local-1 \
  --local-module soc:sm7325=/absolute/path/unixnu-sm7325-common
```

## 2. 私有输入

系统构建读取以下忽略文件：

| 路径 | 输入 |
| --- | --- |
| `.cache/inputs/kernelcache.decompressed` | 同版 21H461 KernelCache；缺少时入口自动提取 |
| `.cache/inputs/bindings/kernel.macho`、`kernel.json` | 平台内核 IOStorage 布局；缺少时入口自动构建平台配置 |
| `.cache/inputs/policies/os/`、`policies/systemos/` | 各卷的 `manifest.bin`、`trusted-key.bin` |
| `.cache/inputs/trust.bundle` | 原始 StaticTrustCache 与 SystemTrustCache bundle |
| `.cache/inputs/data.dbr` | 固定 96 GiB、固定 UUID 的 DBRDISK1 Data 种子 |
| `.cache/inputs/debugserver` | 同版原始 OS 的 `/usr/libexec/debugserver`，作为开发副本签名输入 |
| `.cache/inputs/os-root` | 同版只读 OS 挂载目录或链接，提供 J120 静态壁纸 |
| `.cache/firmware/vendor/T733ZCS9DYE1/vendor/firmware/tsp_focaltech/ft8203_gts7xllite.bin` | 本机 FT8203 触屏固件 |
| `.cache/firmware/vendor/T733ZCS9DYE1/vendor/firmware/a660_gmu.bin`、`a660_sqe.fw` | A642L 的 A660 GMU 和 SQE 固件 |
| `.cache/firmware/vendor/T733ZCS9DYE1/apnhlos/image/a660_zap.mdt`、`a660_zap.b02` | PAS 13 认证元数据与 ZAP shader |

已有输入可通过统一导入器复制：

```sh
python3 scripts/import-build-inputs.py \
  --kernelcache /path/kernelcache.decompressed \
  --bindings /path/platform-kernel-directory \
  --policies /path/signed-policies \
  --trust-cache /path/original-static-system.bundle \
  --data-seed /path/fixed-data.dbr
mkdir -p .cache/inputs
cp /path/os-mount/usr/libexec/debugserver .cache/inputs/debugserver
ln -s /path/os-mount .cache/inputs/os-root
```

导入参数可单独使用；显式 `--replace` 才覆盖已有输入。导入器只复制签名清单及公钥锚。厂商文件使用 `scripts/import-vendor-inputs.py --extraction /path/extracted --inventory /path/inventory.json`，完整输入清单见 [proprietary-files.txt](../proprietary-files.txt)。原始 IPSW 提取、签名策略和首次 Data 种子的具体生成过程保留在 [输入准备归档](archive/BUILDING_2026-10-24.md#从原始-ipsw-准备输入)。

官方组装树将设备 `.cache/firmware` 共享进去；`.cache/firmware/t733-inputs -> ../inputs` 让 archive recipe 读取相同私有输入。设备 `package/` 在树中为 `kits/gts7fewifi/`。普通更新使用现有 `data.dbr`、Data UUID `09A342FC-CB72-44DE-A10E-B89F14508869` 及 [data-layout.toml](../package/data-layout.toml)；首次创建新种子产生的新 UUID 要与私有 defconfig 一起使用。

## 3. 统一构建与输出

每轮先提交全部参与构建的源码。先提交驱动和 kit，再同时更新父仓库 gitlink 与组件 revision，最后固定 core、SoC 和设备提交。工作树有未提交修改或未跟踪源码时不开始编译。交付记录保存所有选中提交号、完整命令和不可移动的构建 tag；私有输入与密钥继续保留在忽略目录。旧包没有源码记录时，不得事后用当前提交补填。

```sh
bash scripts/build.sh system-local-1
bash scripts/build.sh platform-local-1 --platform
```

系统入口依次执行 configure、resolve、kernel、menu、turnip、metal、wallpaper、debug-tools、payload，共九阶段。kernel 内部完成源码准备、头文件导出、编译记录、按序 archive、最终链接和预链接；turnip 编译 ARM64/iOS Vulkan 库，metal 编译并签名系统 Metal 插件及 Rust bridge；debug-tools 将插件、CP/Vulkan 离屏程序和调试工具加入 addon 与 trust bundle；payload 调用共用 DTS 转换器生成 ADT，再组装菜单、固定种子及 FAT。构建命令只生成产物。

turnip 阶段通过 `tools/gpu/bootstrap-turnip.py` 在公共 core 的 `.cache` 内准备固定 Mesa、Python 依赖、glslang 和 bison，再调用官方 `build-turnip-darwin.py`。也可提前执行 `python3 tools/gpu/bootstrap-turnip.py --core "$UNIXNU_BASE"` 准备依赖，保留 `UNIXNU_RECORD_HASHES=0`。

metal 阶段调用 `tools/gpu/bootstrap-metal.py --core <core> --abi-directory <resolved tree>/drivers/Adreno6xx`。主机须已有 `rustup` 和 LLVM 21 或更高版本的 `llvm-as`；脚本在 core 的 `.cache/gpu-reference/metal2vulkan` 获取固定提交 `720ef1ce9ae2a2068999a8675717418b9416283e`，优化主分支由 bridge 构建器按显式列表应用 `0001` 至 `0011`，Apple3 分支应用至 `0018`。每个源码文件只属于一份补丁，共享修改合并回该文件所属补丁；构建器把完整源码差异与登记内容比较后继续编译。Rust 1.98.0 及 `aarch64-apple-ios` target 安装在 `.cache/metal-rustup`，Cargo 依赖保存在 `.cache/metal-cargo`；首次准备需要网络。

bootstrap先查询本地工具链及iOS标准库；固定版本已安装时直接使用，缺失时才执行rustup安装。

本板调用公共 `build-adreno-metal-plugin.py` 时传入 `--gpu-abi-dir <resolved tree>/drivers/Adreno6xx`，并选择 `--scanout-copy off --quartzcore-display-state off --turnip-replay off --turnip-debug '' --turnip-smoke-debug ''`。shader 编译器使用 Turnip 已准备的 `.cache/mesa-tools/bin/glslangValidator`，通过 `--shader-compiler` 指定并传入 `--skip-shader-validation`。插件产物为 core 的 `build/adreno-metal-plugin/NativeAdrenoMetal.bundle`；设备 addon 打包器负责发布标记、插件文件和签名信任输入，固定 `data.dbr` 继续作为普通系统更新的 Data 输入。

默认 `--out` 为 `build/work`。format 2 的中间目录为：

```text
build/work/gts7fewifi/ipados-17.7.11-21H461/gts7fewifi/
  tree/
  kernel/
  package/
  payload/
  xnu-patches.list
```

平台配置最后一级是 `gts7fewifi_platform/`。`scripts/build.py` 在 defconfig 后读取官方 `status --json` 的 `output`，从该目录复制内核、菜单和 FAT。`--out /path/work` 改变输出根；自动绑定内核使用独立 `build/bindings/` 输出根及相同 format 2 层级。平台板通过忽略的 `build/platform-device/board.toml` 传给官方 `-D`，该目录同时链接 DTS、组件表、feed、模块和私有身份。

每次使用新 TAG，交付统一保存在：

```text
build/releases/TAG/
  kernel.macho
  kernel.json
  UniXNU.efi
  payload.img
  payload.json
  logs/
  stages.tsv
```

`stages.tsv` 记录实际阶段和退出码，每个日志旁保存命令 JSON。系统 FAT 为 512 MiB，平台 FAT 为 64 MiB。额外菜单项与安装确认由 board 声明；构建 TAG 通过官方 `--kernel-arg t733-build=TAG` 传入。构建始终保持 `UNIXNU_RECORD_HASHES=0`。

## 4. 补丁与编译输入

两份 board 的 `kernel.patch_series` 显式选择设备 ARM64 series、SM7325 series、设备 OS series、设备功能 series 和诊断清单。普通数组替换继承值，因此完整 components、fragments、series 和 archives 都写在 board 中。社区 NativeUfs、NativeRamBlock、NativeMobileDisplay 与 NativePrelink 保持各自来源身份。

设备 ARM64 series 直接引用公共补丁和本机逐文件替换，保留 `native-smp-secondary.patch` 对 SM7325 三簇调度的前置关系；SoC 仓库提供时钟、CPU startup、managed RT memory、调度 topology 与虚拟定时器补丁；OS series 引用本机启动进度替换及 NativePrelink 两项补丁。设备 `diagnostics.list` 由 board 明确列入；当前两个 defconfig 的 early marker 均为 `n`。新增功能补丁同时维护功能清单与实际选中的 series。

共用构建器生成最终有序 `UNIXNU_PATCH_LIST`，源码准备和设备 baseline 使用同一列表。选择记录包含 XNU 提交、补丁路径、大小与修改时间。输入改变时保存旧 `.cache/xnu-work` 和 `build/xnu/ios-obj`，清除配套头文件完成标记，再准备源码、导出头文件并生成编译记录。源码准备成功后才构建依赖 archive。失败日志及旧目录保留在所选输出根。

交付前读取本轮 `tree/.cache/xnu-patches.list` 或 `tree/.cache/xnu-patch-selection.json`，确认要求的补丁进入当前内核。源码、头文件、编译记录和最终补丁清单共同属于这一轮构建。

## 5. 固件、首次安装与更新

固件变化时用 `--firmware` 或 `--install`，按配套 UEFI 的输入要求提供 `--uefi`、`--stock-recovery`、`--recovery-payload` 等参数。首次完整安装的原始命令和 Samsung 封装关系保存在 [安装归档](archive/BUILDING_2026-10-24.md#4-固件与首次安装包)；其中旧 `build/work/gts7fewifi/` 路径须改成当前解析所得的 format 2 输出目录。

日常更新部署 `build/releases/TAG/payload.img`。工作区 MSC 入口每次重新枚举 userdata，在相对 `9382658048` 字节处写入完整 `536870912` 字节 FAT 并 sync，随后完整重启固件，选择 `Boot UniXNU`。OS/SystemOS、固定 Data 种子、UUID、96 GiB 容量和尾部位置保持当前布局。`Install / erase Data` 执行明确的 Data 重建。

USB 采集入口为 `tools/t733-usb-console.py`；原生调试入口为 `tools/t733-lldb.py` 和 `tools/t733-ios-diagnostics.py`。原生附加前在主机准备符号，暂停期间完成短读取后立即 continue/detach。采集命令及原始输出路径记录到对应 build TAG 的专题记录。
