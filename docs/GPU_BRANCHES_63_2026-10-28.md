# GPU 优化与 Apple3 分支

2026-10-28。用户指定设备、core、SM7325 和 AdrenoMetal 使用以下两个分支：

| 分支 | 内容 |
| --- | --- |
| `unixnu-sm-t733` | BO 复用、硬件一致性、异步任务和 timeline、按完成点睡眠、Metal 资源摘要与阶段同步、DPU 读占用和完成中断、ARGB32/BGRA8 sRGB 系统格式选择 |
| `unixnu-sm-t733-apple3` | 上述全部优化和相同格式策略，加上 Apple3 前端、编译器、纹理与 resolve 功能 |

功能源码固定点如下；后续构建记录的文档提交不改变这些源码身份。

| 仓库 | 优化主分支 | Apple3 分支 |
| --- | --- | --- |
| 设备 | `d53c4817ca4b0fa53b1f9244f3394abd7989ae6c` | `d478f17b01fc3fdde75ee65db8d2837a3c7832ed` |
| core | `00048c9d23b612d943517746aea70097ba6a380c` | `d95e21c80e31ec2a2c9004fda798f5b772a69712` |
| SM7325 | `9af303d1bcae2469d81fc0377d0f747d3ec59609` | `f5920f5aa4bdda37771cd153c82e74d43b40545d` |
| AdrenoMetal | `cc4c26f3ba1091ecb7576d3644fdb5169db3efca` | `effe11ea69880ad8c7df019cfed48b51f99cc53d` |

两边共享 Adreno6xx `512567f430e4a35f58a3c06aaedc1b2c45e24868`、NativeMobileDisplay `d15d6c91dd01698262404755d0568bd85277e46b` 和 ArmSmmu4K `599ba82f96f3e10b0afbd784c040127d38e2683a`。新显示的 Surface 读占用依赖新 GPU 调度器，这两个驱动作为同组使用。

## 提交与源码边界

主分支的 M2V 补丁为 `0001` 至 `0011`。`0006/0011` 保留资源可被 CPU 访问的标记、增量访问摘要、HOST 可见性、barrier/fence 阶段、串行 compute barrier 省略和统计。原有的内存池及硬件一致性优化保留。

Apple3 分支在此基础上加入 `0012` 至 `0018`，并扩充共享文件中的 ICB、memoryless backing、PVRTC 和细分路径；前端 Extended 对象、XR 采样、LOD/argument sampler、depth/stencil resolve 和 256M texture buffer 也属于该分支。Metal 的 `6b9aadd` 保存独立的功能加入提交。两边的系统格式选择及观测相同。

既有发布提交和构建标签保留，分支通过普通新增提交组织；Apple3 分支已合入优化主分支。设备 `support/UniXNU.rev`、`feeds.toml`、`support/repositories.json` 和 core/SoC 的组件版本分别固定对应内容，core gitlink 与组件版本一致。

## 构建结果

`system-gpu-opt-63b` 使用优化主分支，configure、resolve、kernel、menu、turnip、metal、wallpaper、debug-tools、payload 九阶段均退出 0。FAT 位于 `build/releases/system-gpu-opt-63b/payload.img`；源码身份、命令和逐阶段结果分别在同目录 `source-revisions.json`、`stages.tsv` 和 `logs/`。不可移动标签为 `build/system-gpu-opt-63b-source-1`。

```sh
source tools/activate.sh
bash tools/build-t733-recovery.sh system-gpu-opt-63b --system --payload-only \
  --local-module soc:sm7325=/Users/rigoligo/expm/unixnu-sm-t733/sources/unixnu-sm7325-common
```

完整 Apple3 实现已有 [62j 构建记录](GPU_STACK_62J_BUILD_2026-10-28.md)。本次分支整理没有给旧镜像重新指定源码身份。63a 是尚未加入系统 SDR 策略的中间包。63b 已刷入，实际启动暴露异步等待请求被用户态转发长度限制拒绝的问题；两个分支均已加入修复，主分支 63c2 九阶段构建成功，等待部署。后续提交和故障证据见[63c2 记录](GPU_ASYNC_WAIT_63C_2026-10-28.md)。

## 切换源码

设备分支选择后，core 必须使用该分支 `support/UniXNU.rev`，SoC 必须使用 `feeds.toml`，驱动按 gitlink 和组件表获取。`scripts/bootstrap-base.sh` 能在新 core 目录检出设备指定版本。已有 core 的 `.cache/mesa-work` 和 `.cache/gpu-reference/metal2vulkan` 是登记补丁的展开副本；切换功能分支时使用独立 core 目录，或按目标分支的登记补丁重新准备这些缓存。两条分支的缓存集合不同。
