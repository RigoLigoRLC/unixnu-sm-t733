# 构建与装配

从设备仓库目录执行本文命令。主机需要 macOS、完整 Xcode 与 iPhoneOS SDK、Python 3.11 或更高版本、Git、make、LLVM/LLD、mtools、dtc、openssl 和 unifdef。按照 [UniXNU 构建文档](https://unixnu.com/building) 安装公共工具；固件另需配套 UEFI 的工具和输入。

2026-10-24 的整理保留 38a 的配置与固定 Data 输入，内核与 FAT 组装交由官方命令处理。本轮只修改源码和文档，未构建、测试或部署，设备上的 38a 没有更新。

## 1. 公共源码与本机身份

```sh
git clone --recurse-submodules --branch unixnu-sm-t733 https://github.com/RigoLigoRLC/unixnu-sm-t733.git
cd unixnu-sm-t733
# 已有设备仓库检出在更新后执行此命令。
git submodule update --init --recursive
export UNIXNU_RECORD_HASHES=0
bash scripts/bootstrap-base.sh ../UniXNU
export UNIXNU_BASE="$(cd ../UniXNU && pwd)"
cp local.toml.example local.toml
```

`bootstrap-base.sh` 仅接受尚不存在的目录：克隆 `RigoLigoRLC/unixnu-core`、检出 `support/UniXNU.rev` 固定的适配提交并初始化子模块。该提交已包含公共适配；`support/UniXNU.patch` 记录相对 `support/UniXNU.upstream.rev` 的差异供阅读。已有相同适配版本的工作区直接设置 `UNIXNU_BASE`。后续命令使用 `PYTHON` 指定解释器，省略时为 `python3`。

`local.toml` 的 `hardware.unique_chip_id` 和 `hardware.chip_id` 为十六进制软件身份种子，两项一起填写；本机 DRAM 包络为 `0x180000000`。该文件被 Git 忽略。

公共 `unixnu` 自动处理所需源码。也可显式执行：

```sh
python3 "$UNIXNU_BASE/unixnu" defconfig gts7fewifi_defconfig -D . --out build/work
python3 "$UNIXNU_BASE/unixnu" fetch -D . --out build/work
python3 "$UNIXNU_BASE/unixnu" ipsw fetch -D . --out build/work
```

`UNIXNU_IPSW` 可指定本地 IPSW；省略时使用 `board.toml` 中的 21H461 URL。生成的 `unixnu.lock` 应随发布源码保存；公共源自身还由 `support/UniXNU.rev` 固定。

## 2. 私有输入

系统构建读取如下文件：

| 路径 | 来源 |
| --- | --- |
| `.cache/inputs/kernelcache.decompressed` | 匹配 IPSW 的 KernelCache；缺少时构建入口自动提取 |
| `.cache/inputs/bindings/kernel.macho` 和 `kernel.json` | 平台内核的 IOStorage 地址布局；缺少时自动构建平台配置 |
| `.cache/inputs/policies/os/`、`policies/systemos/` | 各自的 `manifest.bin` 与 `trusted-key.bin` |
| `.cache/inputs/trust.bundle` | 原始 StaticTrustCache 与 SystemTrustCache 的 bundle |
| `.cache/inputs/data.dbr` | 固定 96 GiB 逻辑容量的 DBRDISK1 Data 种子 |
| `.cache/firmware/vendor/T733ZCS9DYE1/vendor/firmware/tsp_focaltech/ft8203_gts7xllite.bin` | 本机原厂触屏固件 |

已有输入用一个入口导入：

```sh
python3 scripts/import-build-inputs.py \
  --kernelcache /path/kernelcache.decompressed \
  --bindings /path/platform-kernel-directory \
  --policies /path/signed-policies \
  --trust-cache /path/original-static-system.bundle \
  --data-seed /path/fixed-data.dbr
```

各参数可单独使用；已有输入只有显式 `--replace` 才会覆盖。导入器只复制签名清单和公钥锚，不复制私钥。厂商文件通过 `scripts/import-vendor-inputs.py --extraction /path/extracted --inventory /path/inventory.json` 导入，或按 `proprietary-files.txt` 的路径放入缓存。

构建入口建立 `.cache/firmware/t733-inputs -> ../inputs`。官方组装过程把设备的 `.cache/firmware` 接入构建树，因此 archive 命令从树内的 `.cache/firmware/t733-inputs/` 读取私有文件。archive 命令采用脚本名和相对构建树路径，设备 C/C++ 源码从官方挂载的 `kits/gts7fewifi/` 读取。ADT 通过 `UNIXNU_HARDWARE_INPUT` 读取设备的 `local.toml`。

### 从原始 IPSW 准备输入

```sh
python3 "$UNIXNU_BASE/unixnu" build userdata -D . --out build/work
python3 "$UNIXNU_BASE/unixnu" ipsw fetch StaticTrustCache Cryptex1,SystemTrustCache -D . --out build/work
python3 scripts/prepare-system-trust.py \
  --static /path/StaticTrustCache.im4p --system /path/SystemTrustCache.im4p \
  --output build/trust
```

提取出的实际成员名见 `proprietary-files.txt`；`ipsw fetch` 输出实际路径。`build userdata` 生成 `build/work/gts7fewifi/userdata/userdata-root.img` 与 `userdata-root.json`。根卷窗口中保存原始 OS/SystemOS，后续签名策略必须基于同一镜像。

21H461 的两个卷 UUID 已保存在 `package/root-volumes.json`。用 VerifiedRoot 工具生成本地签名策略：

```sh
python3 scripts/prepare-root-policies.py \
  --root-image build/work/gts7fewifi/userdata/userdata-root.img \
  --layout build/work/gts7fewifi/userdata/userdata-root.json \
  --volumes package/root-volumes.json \
  --manifest-tool "$UNIXNU_BASE/kits/VerifiedRoot/tools/manifest.py" \
  --output build/policies
```

`--manifest-tool` 使用对应公共基线中的清单生成程序，见其 `kits/VerifiedRoot` 文档。签名私钥保存在所选本地输出目录，公开源码只保留生成程序。

Data 由原始 OS 的 `private/var` 初始化。将 OS 只读挂载到本地目录，再执行：

```sh
python3 scripts/prepare-owner-data-fixture.py --source-root /path/os-mount --output build/data-seed
python3 drivers/DarwinBoot/Test/packed_ramdisk.py build/data-seed/owner-data-32.cdr build/data-seed/base.dbr
python3 "$UNIXNU_BASE/scripts/seed-data-grow.py" --gib 96 build/data-seed/base.dbr build/data-seed/grown.dbr
python3 scripts/seed-data-skip-setup.py build/data-seed/grown.dbr build/data-seed/data.dbr --no-auto-dim
```

新建 APFS 种子产生新 UUID。将 `build/data-seed/seed.json` 的 `volume_uuid` 填入一份私有 defconfig 的 `CONFIG_XNU_OWNER_DATA_VOLUME_UUID`，构建时用 `--defconfig /absolute/path/private_defconfig`。当前默认 UUID 对应原有设备 Data，不能用新 UUID 的种子更新已有设备。日常更新持续使用同一 `data.dbr`，容量、UUID 与 `package/data-layout.toml` 保持一致。

## 3. 单一构建入口

```sh
# 完整系统，默认八核，生成日常更新用的 512 MiB FAT。
bash scripts/build.sh system-local-1

# 单核平台诊断，生成 64 MiB FAT，可用作 recovery 中的备用载荷。
bash scripts/build.sh platform-local-1 --platform
```

入口依次调用官方 `defconfig`、`build kernel`、`build package` 和 `build payload`。`build kernel` 内部完成编译与预链接；`build payload` 生成 ADT、复制 trust bundle 和固定 Data 种子，并完成菜单与 FAT。缺少绑定内核时先完成平台构建；该过程与系统构建使用不同输出目录。默认中间文件为 `build/work/`，可用 `--out /path/work` 选择。

系统构建使用设备根目录的 `board.toml`。平台构建在忽略的 `build/platform-device/` 写入完整的 `board.platform.toml`，以 `board.toml` 为文件名，并链接设备模块、scripts、configs、缓存和身份文件。官方命令通过 `-D build/platform-device` 选择该配置；平台配置没有 Data 安装菜单。

`[payload] files` 声明将菜单构建产物复制为 FAT 内的 `UpdatedMenu.efi`；`extra_entries` 声明菜单 EFI、1/2/4 核备用启动项和返回固件。CPU 项继承最终启动参数，再覆盖 `cpus` 与 `sheng-smp`。安装确认、重启和关机项由官方 payload 生成。系统配置显式保留 `sheng-data-persist=2` 和 `sheng-log-reserve=1024`，tag 通过官方 `--kernel-arg t733-build=TAG` 传入。

每次使用新 tag，输出为 `build/releases/TAG/`：

```text
kernel.macho
kernel.json
UniXNU.efi
payload.img
payload.json
logs/
stages.tsv
```

`stages.tsv` 记录已执行阶段和退出码，失败日志保留在该 tag 下。`payload.json` 保存官方组装记录；FAT 的临时目录由官方命令清理。构建入口设置 `UNIXNU_RECORD_HASHES=0`。原始系统签名和磁盘格式所需的密码学处理属于对应格式的生成过程。

## 4. 固件与首次安装包

Samsung UEFI 独立维护，按其 `notes/RELEASE_STRUCTURE.md` 准备 vendor 模块、原厂 DTB/DTBO、工具及匹配的 boot/recovery。

```sh
git clone --recurse-submodules --branch unixnu-sm-t733 https://github.com/RigoLigoRLC/unixnu-mu-silicium.git ../uefi-gts7fewifi
git -C ../uefi-gts7fewifi checkout --detach "$(cat support/Mu-Silicium.rev)"
git -C ../uefi-gts7fewifi submodule update --init --recursive
bash scripts/build.sh system-install-1 --install \
  --uefi ../uefi-gts7fewifi \
  --stock-recovery /path/stock/recovery.img \
  --stock-boot /path/stock/boot.img \
  --root-image build/work/gts7fewifi/userdata/userdata-root.img \
  --recovery-payload build/releases/platform-local-1/payload.img
```

`--install` 生成系统 FAT、固件、原厂 recovery 恢复包、含 FAT 的 userdata sparse 镜像及完整系统 tar。该包用于首次安装，会覆盖 userdata；日常更新只使用 FAT。只需重新打包固件时使用 `--firmware`，同时提供 `--uefi`、`--stock-recovery` 与 `--recovery-payload`。

本脚本不执行刷机。MSC 更新由主机部署工具重新识别平板 userdata，只在分区内 `9382658048` 字节偏移写入 `536870912` 字节 FAT，完成 dd 与 sync 后完整重启固件。保留 OS/SystemOS 与持久 Data；当前固件不使用 eject。Odin sparse 不能用作保留数据的 FAT-only 更新方式。

## 5. USB 日志

```sh
python3 -m pip install -r tools/requirements-usb-console.txt
python3 tools/t733-usb-console.py --output logs/usb
# 另一个终端通过同一采集器查询，无需重新打开串口。
python3 tools/t733-usb-console.py --output logs/usb --command tasks --process SpringBoard
python3 tools/t733-symbolicate-stacks.py logs/usb/capture.raw \
  --cache /path/systemos/System/Library/Caches/com.apple.dyld \
  --root /path/os --root /path/systemos --output logs/stacks.txt
```

调用栈符号化使用同版原始系统文件；实际 raw 文件名由采集器打印并记录在 `events.jsonl`。
