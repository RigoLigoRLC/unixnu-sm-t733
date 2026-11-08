# 删除旧工作区后重新构建 SM-T733

本教程恢复 `system-gpu-upstream-76b` 的设备、GPU 和系统配置。主机为 Apple Silicon
Mac，系统输入固定为 iPad7,1 / j120ap / iPadOS 17.7.11 / 21H461，Samsung 输入固定为
SM-T733 / CHN / T733ZCS9DYE1。76b 的实机构建和部署记录见
[76 迁移记录](GPU_UPSTREAM_REBASE_76_2026-11-04.md)。

发布状态（2026-11-08）：教程及恢复修改已在本地准备，两个加密模块的 fork 和新的
`rebuild/76b-20261108` tag 等待发布授权。完成发布前保留旧工作区。

## 1. 只保留不能重新生成的内容

私有目录与源码仓库分开放置，例如
`/Users/rigoligo/expm/unixnu-sm-t733-preserved-20261108`。权限为目录 0700、私有文件 0600。

| 私有文件 | 用途 |
| --- | --- |
| `local.toml` | 原设备身份、序列号、MAC 和软件钥匙串根密钥 |
| `data.dbr` | 原始固定 UUID 的 Data 种子，实际约 1.3 MB，逻辑容量 96 GiB |
| `owner.pem` | 原根卷签名私钥；公钥、读取策略和签名由它重新生成 |
| `device-original/` | 原厂设备专有分区及当时采集的硬件状态；具体文件见私有目录清单 |

Data UUID 为 `09A342FC-CB72-44DE-A10E-B89F14508869`，布局由
`package/data-layout.toml` 固定。重新创建 APFS 种子会产生另一 UUID，必须保留这份种子。
它是初始 Data 内容；平板当前安装的应用和用户文件保存在平板 userdata 内。

已发布源码重新克隆；IPSW、Samsung 包、提取的固件、kernelcache、绑定内核、已签策略、
trust bundle、debugserver、工具链缓存和最终镜像全部重新取得或生成。

## 2. 安装工具并取得固定源码

安装完整 Xcode 并完成首次启动，安装 Homebrew 和 Rust 的 `rustup`。
以下命令创建新的工作目录，`T733_PRIVATE` 改成上一步的实际私有目录：

```sh
brew install python@3.13 llvm lld cmake ninja dtc mtools bison lz4 openssl
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export PATH="$(brew --prefix llvm)/bin:$(brew --prefix lld)/bin:$(brew --prefix bison)/bin:$HOME/.cargo/bin:$PATH"
export UNIXNU_RECORD_HASHES=0
export T733_PRIVATE=/Users/rigoligo/expm/unixnu-sm-t733-preserved-20261108

mkdir -p "$HOME/unixnu-t733-rebuild"
cd "$HOME/unixnu-t733-rebuild"
"$(brew --prefix python@3.13)/bin/python3.13" -m venv .venv
source .venv/bin/activate
export PYTHON="$(command -v python3)"

git clone --recurse-submodules --branch rebuild/76b-20261108 \
  https://github.com/RigoLigoRLC/unixnu-sm-t733.git device
cd device
bash scripts/bootstrap-base.sh ../core
export UNIXNU_BASE="$(cd ../core && pwd)"
cp "$T733_PRIVATE/local.toml" local.toml
chmod 600 local.toml
mkdir -p .cache/inputs
cp "$T733_PRIVATE/data.dbr" .cache/inputs/data.dbr
```

恢复 tag 固定教程、输入准备和部署工具；驱动运行代码沿用 76b。core 的恢复提交
`c17e7eab4004b144b91b677e016912212464b8f6` 仅修正两个加密子模块的下载地址，
其 gitlink 保持 SoftwareAES `5add7a8` 和 AppleKeyStoreState `635b8f9`。
原构建 tag `build/system-gpu-upstream-76b-source-1` 保留原提交，继续用于追溯原包。

`bootstrap-base.sh` 的目标目录必须不存在。设备的 `support/UniXNU.rev` 固定 core，
`feeds.toml` 固定 SM7325 `14d3ee4`，递归 gitlink 固定其他模块。
新目录无需旧工作区的环境脚本或 `--local-module` 绝对路径。

Turnip 会准备固定 Mesa、glslang、bison 及 Python 依赖；Metal 会准备 metal2vulkan
`624bab3918de5b978e1101517abca2b4fa426565`、Rust 1.98.0 及 `aarch64-apple-ios`。
Metal 需要 LLVM 21 或更新版本的 `llvm-as`。菜单构建器自行组合 EDK 所需的 LLVM/LLD 工具目录。

## 3. 从 Apple 原始输入重新生成系统输入

以下命令都在 `device` 内执行。先配置以取得固定 SoC，再运行读取 IPSW 的命令。
core 使用 board 记录的 Apple URL 按需下载成员。若已有重新下载的完整 IPSW，
可先设置 `UNIXNU_IPSW=/absolute/path/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw`。

```sh
python3 "$UNIXNU_BASE/unixnu" defconfig gts7fewifi_defconfig -D . --out build/work
python3 "$UNIXNU_BASE/unixnu" build userdata -D . --out build/work
python3 "$UNIXNU_BASE/unixnu" ipsw fetch \
  StaticTrustCache 'Cryptex1,SystemTrustCache' -D . --out build/work

mkdir -p build/os-mount
hdiutil attach -readonly -nobrowse -noverify -noautofsck \
  -mountpoint "$PWD/build/os-mount" build/work/ipsw/21H461/044-76040-038.dmg
cp build/os-mount/usr/libexec/debugserver .cache/inputs/debugserver
ln -s "$PWD/build/os-mount" .cache/inputs/os-root

python3 scripts/prepare-system-trust.py \
  --static build/work/ipsw/21H461/044-76040-038.dmg.trustcache \
  --system build/work/ipsw/21H461/044-76034-038.dmg.trustcache \
  --output build/trust

root_output="$PWD/build/work/gts7fewifi/ipados-17.7.11-21H461/gts7fewifi/userdata"
python3 scripts/prepare-root-policies.py \
  --root-image "$root_output/userdata-root.img" \
  --layout "$root_output/userdata-root.json" \
  --volumes package/root-volumes.json \
  --manifest-tool "$UNIXNU_BASE/kits/VerifiedRoot/tools/manifest.py" \
  --private-key "$T733_PRIVATE/owner.pem" \
  --output build/policies
python3 scripts/import-build-inputs.py \
  --policies build/policies \
  --trust-cache build/trust/original-static-system.bundle
```

`prepare-root-policies.py --private-key` 使用原私钥并导出公钥，按原始两卷、固定 UUID 和
generation 1 重新签名。生成目录须不存在。日常重建复用这次输入时跳过本节。
OS 挂载供壁纸和 Metal 的 `.metallib` 清单读取；Mac 重启后需要再次挂载到相同位置。
构建入口会自动提取 kernelcache 并编译平台绑定内核。

## 4. 从 Samsung 原厂包重新提取固件

使用 [samloader 2.2.0](https://github.com/topjohnwu/samloader-rs/releases/tag/2.2.0)
的 macOS 程序，将其所在目录加入 PATH。下载原版本：

```sh
mkdir -p .cache/samsung
samloader download --model SM-T733 --region CHN \
  --version T733ZCS9DYE1/T733CHN9DYE1/T733ZCS9DYE1/T733ZCS9DYE1 \
  --out-dir "$PWD/.cache/samsung"
```

此版本 ZIP 名为 `SM-T733_4_20250514094921_fgj7czrxbu_fac.zip`。下载服务若不再提供该版本，
从原厂固件归档取得相同 PDA/CSC 版本。解包后从 AP 提取 super、BOOT、recovery，从 BL 提取 APNHLOS：

```sh
mkdir -p .cache/samsung/packages .cache/samsung/images
unzip .cache/samsung/SM-T733_4_20250514094921_fgj7czrxbu_fac.zip -d .cache/samsung/packages
tar -xf .cache/samsung/packages/AP_T733ZCS9DYE1_T733ZCS9DYE1_MQB96142448_REV00_user_low_ship_MULTI_CERT_meta_OS14.tar.md5 \
  -C .cache/samsung/images super.img.lz4 boot.img.lz4 recovery.img.lz4
tar -xf .cache/samsung/packages/BL_T733ZCS9DYE1_T733ZCS9DYE1_MQB96142448_REV00_user_low_ship_MULTI_CERT.tar.md5 \
  -C .cache/samsung/images NON-HLOS.bin.lz4
lz4 -d .cache/samsung/images/super.img.lz4 .cache/samsung/images/super.img
lz4 -d .cache/samsung/images/NON-HLOS.bin.lz4 .cache/samsung/images/apnhlos.img
lz4 -d .cache/samsung/images/boot.img.lz4 .cache/samsung/images/boot.img
lz4 -d .cache/samsung/images/recovery.img.lz4 .cache/samsung/images/recovery.img

git clone https://github.com/unix3dgforce/lpunpack.git .cache/lpunpack
git -C .cache/lpunpack checkout --detach c59b8f3b069c5a8aa438a049fa4a091177172434
python3 .cache/lpunpack/lpunpack.py -p vendor -p odm \
  .cache/samsung/images/super.img .cache/samsung/partitions
python3 -m pip install dissect.extfs==3.15 dissect.fat==3.13
python3 scripts/extract-vendor-inputs.py \
  --images .cache/samsung/partitions \
  --apnhlos .cache/samsung/images/apnhlos.img \
  --output .cache/samsung/extracted
python3 scripts/import-vendor-inputs.py \
  --extraction .cache/samsung/extracted
```

lpunpack 负责展开 Android sparse super。导入器按 `package/firmware-inputs.toml`
选择 FT8203、GMU/SQE、ZAP 等输入，缓存写入 `.cache/firmware/vendor/T733ZCS9DYE1/`。

## 5. 构建完整系统

```sh
build_tag=system-gpu-upstream-76b-rebuild-1
bash scripts/build.sh "$build_tag"
```

每次使用新 `build_tag`。入口依次执行 configure、resolve、kernel、menu、turnip、metal、
wallpaper、debug-tools、payload；九个阶段的退出码记录在
`build/releases/$build_tag/stages.tsv`，各阶段命令和输出在同目录 `logs/`。
成功后得到 `kernel.macho`、`kernel.json`、`UniXNU.efi` 和 512 MiB `payload.img`。
失败后按相应阶段日志处理，下一轮使用新名称。无需保留旧工作区的中间输出。

## 6. 更新已经安装 UniXNU 的这台平板

本仓库自带 MSC 服务及部署入口。主机终端执行一次：

```sh
python3 -m pip install -r tools/requirements-usb-console.txt
sudo /usr/bin/python3 -I tools/msc-service/service.py serve --detach
tools/msc-service/mscctl list
bash tools/deploy-t733-msc.sh "$build_tag"
```

平板须在固件 MSC 模式。部署入口先开启被动 USB 采集，服务重新枚举本机 userdata，
检查分区几何及 SHNGROOT，写入相对 `0x22F400000` 的完整 512 MiB FAT，再执行 sync。
成功后完整重启平板固件，选择 `Boot UniXNU`。现有 OS/SystemOS 和 Data 保留。
`Install / erase Data` 会重建 Data。服务用法见 [MSC 说明](../tools/msc-service/README.md)。

## 7. 需要重建 UEFI 或完整首次安装包时

完成上面的系统输入准备后，取得设备固定的 UEFI 版本：

```sh
git clone --no-checkout https://github.com/RigoLigoRLC/unixnu-mu-silicium.git ../uefi
git -C ../uefi checkout --detach "$(cat support/Mu-Silicium.rev)"
git -C ../uefi submodule update --init --recursive
brew install acpica nasm
python3 -m pip install -r ../uefi/pip-requirements.txt
export UNIXNU_UEFI="$(cd ../uefi && pwd)"
```

导入固定 Aloha 和原厂 recovery 的输入，命令中的子 shell 完成后返回设备仓库：

```sh
device_root="$PWD"
(
  cd "$UNIXNU_UEFI"
  mkdir -p .cache
  git clone --branch xyz/sm7325/gts7fewifi \
    https://github.com/qaz6750/mu_aloha_platforms.git .cache/aloha
  git -C .cache/aloha checkout --detach 9e6cb5f1249d18bab50fecb614b4928f6ed55c8b
  git clone https://android.googlesource.com/platform/system/tools/mkbootimg .cache/mkbootimg
  git -C .cache/mkbootimg checkout --detach d2bb0af5ba6d3198a3e99529c97eda1be0b5a093
  python3 .cache/mkbootimg/unpack_bootimg.py \
    --boot_img "$device_root/.cache/samsung/images/recovery.img" --out .cache/stock-recovery
  python3 Resources/Scripts/import_gts7fewifi_inputs.py \
    --aloha .cache/aloha \
    --recovery-dtb .cache/stock-recovery/dtb \
    --recovery-dtbo .cache/stock-recovery/recovery_dtbo
)
```

该步骤导入 Qualcomm EFI、DEPEX、RawFiles，并保留本机 uefipil 配置。
来源及许可见 UEFI 仓库的 `notes/RELEASE_STRUCTURE.md`。

先生成备用平台 FAT，再通过统一入口生成系统、BOOT/recovery 或首次安装包：

```sh
export CLANG_BIN="$PWD/build/work/gts7fewifi/ipados-17.7.11-21H461/gts7fewifi/toolchain/"
bash scripts/build.sh platform-rebuild-1 --platform
bash scripts/build.sh system-firmware-rebuild-1 --firmware \
  --uefi "$UNIXNU_UEFI" \
  --stock-recovery "$PWD/.cache/samsung/images/recovery.img" \
  --stock-boot "$PWD/.cache/samsung/images/boot.img" \
  --recovery-payload "$PWD/build/releases/platform-rebuild-1/payload.img"
```

这里的 `CLANG_BIN` 使用第 5 节菜单阶段已经建立的 LLVM/LLD 工具目录，并传给独立 UEFI 构建器。

首次完整安装把上述 `--firmware` 换成 `--install`，使用另一个新 tag，并增加
`--root-image "$root_output/userdata-root.img"`。产物
`gts7fewifi-unixnu-system.tar` 包含 userdata，刷入会覆盖原系统和 Data。
日常更新使用第 6 节的 FAT。构建入口只生成镜像，设备写入由部署步骤执行。

## 8. 本次整理的检查范围

2026-11-08 核对设备、core、SoC、UEFI 及递归子模块共 74 个远端提交，72 个可访问；
发现 SoftwareAES 和 AppleKeyStoreState 的本地适配提交没有发布，已准备对应 fork
地址修正。教程逐项对应当前脚本，补齐重新提取厂商固件和复用签名私钥的入口。
本次未执行完整系统或固件重编译；76b 的九阶段成功和实机启动属于已有交付证据。
