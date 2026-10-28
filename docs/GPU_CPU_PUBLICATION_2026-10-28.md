# GPU CPU 侧优化源码发布

2026-10-28。发布分支统一为 `unixnu-sm-t733`。本轮发布已有 BO 复用、
硬件一致性内存、CPU 缓存维护删除和 61a 逐帧计时源码。

## 直接涉及的仓库

| 仓库 | 发布提交 | 内容 |
| --- | --- | --- |
| [unixnu-adreno6xx](https://github.com/RigoLigoRLC/unixnu-adreno6xx) | `bdb86b8` | BO backing 缓存、映射与提交、硬件一致性和 GPU 写入记录 |
| [unixnu-adrenometal](https://github.com/RigoLigoRLC/unixnu-adrenometal) | `0d69cf8` | Metal 内存选择、分配池协调和取消多余 IOSurface 刷新 |
| [unixnu-arm-smmu4k](https://github.com/RigoLigoRLC/unixnu-arm-smmu4k) | `599ba82` | SMMU outer-shareable coherent 页表读取 |
| [unixnu-nativemobiledisplay](https://github.com/RigoLigoRLC/unixnu-nativemobiledisplay) | `3ff9ff4` | DPU surface 与页表一致性、61a 逐帧计时 |
| [unixnu-core](https://github.com/RigoLigoRLC/unixnu-core) | `2b582dc` | Turnip 补丁、组件固定版本和 Metal fork 来源 |
| [unixnu-sm-t733](https://github.com/RigoLigoRLC/unixnu-sm-t733) | 本记录所在提交 | 设备集成、构建入口、固定版本和发布清单 |

## 同时补齐的固定依赖

| 仓库 | 发布提交 | 与设备的关系 |
| --- | --- | --- |
| [unixnu-sm7325-common](https://github.com/RigoLigoRLC/unixnu-sm7325-common) | `1829984` | `feeds.toml` 固定的 SoC 配置与组件版本 |
| [unixnu-armcpu](https://github.com/RigoLigoRLC/unixnu-armcpu) | `55cc061` | 设备与 SoC 已固定的 CPU 组件 |
| [unixnu-mu-silicium](https://github.com/RigoLigoRLC/unixnu-mu-silicium) | `d511b74` | `support/Mu-Silicium.rev` 固定的固件源码 |
| [unixnu-mu-basecore](https://github.com/RigoLigoRLC/unixnu-mu-basecore) | `3567108` | 固件已使用的 Mu_Basecore 修改 |

AdrenoMetal 和 Mu_Basecore 新建个人 fork，官方来源保留为本机 `upstream`。
core 的 `.gitmodules` 与组件目录同时改为 Metal fork；UEFI 的 `.gitmodules`
改为 Mu_Basecore fork。设备同步固定这两个父仓库的新提交。

## 构建身份

六个直接仓库及 SM7325 common 同时发布已有标签：

- `build/system-gpu-coherent-60a-source-1`
- `build/system-gpu-frame-profile-61a-source-1`

标签保持原构建提交。本轮新增提交只修改发布来源、版本引用和文档，没有重建
或刷写设备。60a、61a 的原始构建身份仍以各自 `source-revisions.json` 为准。
当前完整仓库地址及提交记录在 [repositories.json](../support/repositories.json)。
