# A642L 图形内存分配与硬件一致性设计

2026-10-28。本文保存实施前的设计讨论，随 Git 提交 `1a2a489` 保存。
当前实施以[CPU 缓存维护删除记录](GPU_CPU_CACHE_REMOVAL_60.md)为准；下文提出的
IOSurface 创建 hook、活跃页面属性转换和额外持有记录不作为实施要求。

## 1. 必须满足的行为

1. 普通图形内存能够采用 CPU WB 和 GPU 硬件一致性映射时，分配器必须采用这一方式。
   不能通过把全部页面改成 WC 来绕过硬件一致性接入。
2. Metal 的存储与缓存需求、Vulkan 选择的内存类型、内核实际分配、CPU 映射、GPU/DPU
   映射和 BO 复用必须使用同一份实际属性记录，不能在跨层调用时丢失。
3. 常规 SpringBoard/backboardd 的 buffer、texture、heap 和 IOSurface 渲染不执行
   每次提交或每次显示时的 CPU 逐行 clean/invalidate。这个要求直接约束设计，
   不以先测得某个覆盖比例为前提。
4. 已有外部 Normal NC/WC 页不产生普通 CPU 数据缓存行，采用对应映射和屏障；
   `NonWriteBack` 不能直接推导出需要手动维护缓存。兼容已有 NC 输入不代表新建普通
   图形资源允许继续采用 NC；本机可控制的 IOSurface 创建与映射也必须统一到 WB。
5. 页面属性和物理重叠关系在分配、导入、建立或解除映射时处理。提交读取 BO 已保存的
   属性与依赖，不重新逐页查询 WIMG、不反复扫描物理页、不做全 BO cache 操作。
6. 每项软件维护例外必须说明实际 Metal 创建方法、参数、外部内存来源、真实 CPU 属性、
   设备能力以及为何无法使用硬件一致性或 NC 的免维护方式。`unknown`、`mixed`、
   `non-writeback` 和 `IOSurface` 本身都不是充分理由。
7. CPU 数据缓存维护、GPU 自身缓存命令、SMMU 页表可见性、CPU 屏障和 fence 依赖分别
   按它们服务的组件处理；取消重复 CPU 数据缓存操作不改变读写依赖或对象寿命。
8. 优先移植本地 SM7325 KGSL、ION、SDE 和 Linux DMA 的现成做法，XNU 新增的页面属性
   生命周期接口单独标明为本机设计。Mesa 和 Metal2Vulkan 修改必须进入维护补丁集。

## 2. 当前已经确定的缺口

59b 操作期 256 笔提交的 CPU 均值为 0.996805 ms、中位数为 0.911 ms，其中软件缓存
维护均值为 0.969016 ms。每笔仍维护 32,768,000 字节导入内存，记录在工作区
`logs/gpu-coherent-59b-runtime/interaction-1.raw` 与 `submission-costs-summary.json`。
这批页的原始 WIMG 未进入保存的记录，不能把它们直接归为某一种 CPU 内存类型。

三个独立设计任务已确定以下跨层问题：

- Metal2Vulkan 已保留存储/缓存需求，但部分 Private/纹理选择只按 DEVICE_LOCAL 和
  类型顺序决定，新增 cached coherent 类型后也需要调整这些调用的选择规则。
- Turnip Darwin 分配函数收到 `mem_property` 后，旧 `BoAlloc` 只向内核传尺寸、
  对齐和物理连续性，缓存要求在此丢失。
- 内核 owned BO 被统一改成 WC，backing cache 也只接收 WC；当前 WB coherent
  映射只接入了合格的 imported BO。
- import 将所有非 COPYBACK 属性压缩为 `NonWriteBack`，提交再对这些导入统一维护，
  丢失了 Normal NC/RT 等无需逐行维护的区别。
- 显示路径每次 present 无条件清理整个表面，还存在绕过 VM 映射属性的内核内部
  publish/acquire 调用。

源码位置、成熟参考和具体改动入口由以下专题分别负责：

- [owned BO 分配、映射、复用和 fence 生命周期](design/GPU_COHERENT_ALLOCATOR_2026-10-28.md)
- [Metal 参数、Vulkan 类型与软件维护例外表](design/GPU_METAL_MEMORY_POLICY_2026-10-28.md)
- [外部页面、CPU 别名与 DPU 显示共享](design/GPU_IMPORTED_MEMORY_DISPLAY_2026-10-28.md)

## 3. 从分配到回收的统一处理

Metal2Vulkan 先按真实 Metal 参数产生内存需求。Turnip 选择符合需求的 Vulkan 内存
类型，并将 CPU 是否可见、是否缓存、是否要求免应用显式 flush，以及外部导入来源
传给内核。内核对自有内存优先建立 WB backing，对外部内存固定原始 backing 后确认
实际页面属性；最终 CPU/GPU/DPU 属性由内核保存并返回，用户参数不能伪造结果。

同一 backing 的记录保存原始 descriptor/exporter、稳定物理页、实际 CPU 缓存属性、
CPU 映射和设备映射引用、当前读写 fence，以及属性是否允许更改。各 GPU/DPU 映射
分别保存本设备实际启用的一致性属性。重复导入和 heap 子分配引用同一 backing 的
对应范围，避免创建会独立改变相同物理页属性的第二个所有者。

CPU 映射存活或设备仍持有 backing 时，缓存属性必须保持一致；属性变化在映射和
分配生命周期的接口完成，不靠提交时逐页复查修补。XNU 的 pin 操作只保证页不被
回收，属性稳定需要另外的生命周期约束。

提交依次取得资源引用、处理读写 fence、执行所需屏障并发布命令；GPU 完成后保留
正确的 GPU cache 完成事件，随后解除 job 引用。WB coherent 和真正 NC/WC 的普通
提交均不调用 CPU 逐行 clean/invalidate。显示提交同样根据 DPU attachment 的真实
属性等待 GPU producer 并切换扫描，不重新清理全部 IOSurface 页面。

最后一个 CPU/GPU/DPU 引用结束后，backing 才进入可复用缓存。缓存匹配同时考虑
尺寸、CPU 实际属性、设备一致性方式及所需权限；复用不切换活跃页面的属性。
释放回 XNU 前完成全部映射解除和 fence 回收，由 backing 所有者统一负责属性恢复。

## 4. Metal 分配参数审查表

当前源码没有显示任何一种 Metal 分配参数本身必须依赖 CPU 每帧逐行维护缓存。
下面是目标处理；“不需要”指 CPU 对资源数据执行 clean/invalidate，屏障和 fence
仍按前节处理。入口和参数到现有代码的详细对应见
[Metal 参数专题](design/GPU_METAL_MEMORY_POLICY_2026-10-28.md)。

| Metal 创建入口及参数形态 | 最终存储与映射规则 | 每次提交/显示的 CPU 逐行维护 |
| --- | --- | --- |
| `newBufferWithLength:options:` / `newBufferWithBytes:length:options:`，`Shared + DefaultCache`，缓存/存储位为 `0x00` | 自有 WB backing，GPU 硬件一致性 | 不需要 |
| 同两入口，`Shared + WriteCombined`，`0x01` | 保留 Metal2Vulkan 在统一内存上的已有选择：该参数是 hint，可用时仍优先 cached coherent | 不需要 |
| 同两入口，`Private`，`0x20/0x21` | 普通 DRAM backing 也优先选择硬件一致性，内部上传和映射使用相同实际属性 | 不需要 |
| 同两入口，`Managed`，`0x10/0x11`，当前 runtime 接受 | 按 unified runtime 的存储与同步语义处理，常规 DRAM backing 使用硬件一致性 | 不需要；资源范围与同步语义另行保留 |
| `newTextureWithDescriptor:`，`Shared/Managed/Private`，两种 CPU cache mode | 图像 backing 和上传/回读资源分别按实际用途选 type，符合条件的 DRAM 全部使用硬件一致性 | 不需要 |
| `newHeapWithDescriptor:`，`Automatic/Placement + Shared/Private`，子资源缓存/存储参数与 heap 匹配 | heap 选择一次 WB coherent 内存类型，子分配继承同一 backing 属性和寿命 | 不需要 |
| `newTextureWithDescriptor:`，`Memoryless` | 当前没有 lazy BO，普通 DRAM backing 使用 WB coherent，保留 Memoryless 的内容使用规则 | 不需要 |
| `newBufferWithBytesNoCopy:length:options:deallocator:`，`0x00/0x01/0x10/0x11`，原始页为稳定 WB | 保留原始页，取得属性稳定保证，GPU 使用硬件一致性映射 | 不需要 |
| `newTextureWithDescriptor:iosurface:plane:`，有效 plane，descriptor 为 `Shared/Managed/Private`，原始页为稳定 WB | frontend 固定以 Shared 导入 backing，GPU 和 DPU 各自建立 coherent attachment | 不需要 |
| 上述 no-copy/IOSurface 导入，原始页确认为 Normal NC/WC，含本板实际映成 Normal NC 的 RT 页 | 只作为既有外部输入兼容，保留有效属性并处理别名；本机新建 IOSurface 由实际分配者统一选 WB | 不需要；这不是硬件 snoop，也不是软件逐行维护 |
| buffer-backed/linear texture 或 texture view | 继承父 backing；需要新建 shadow 时选择 WB coherent | 不需要 |
| 上述外部导入，页面属性未知、混合或存在冲突别名 | 在导入阶段取得完整属性并形成有效映射；无法建立稳定合法属性时返回明确错误 | 不进入长期逐帧全 BO 维护 |
| buffer 为 `Memoryless`；no-copy 为 `Private/Memoryless`；heap 为 `Managed/Memoryless/Sparse`；raw no-copy texture | 当前入口拒绝或未接入，不形成成功 BO | 没有维护调用 |

`Shared`、`Private`、`Managed`、`WriteCombined`、IOSurface 和 GPU 写入都不能单独
触发软件维护。原生 IOSurface 的创建者位于 Metal 导入入口之外，Metal descriptor
不能改变既有页面的真实属性；需要改变新建 IOSurface 的默认属性时，改动必须落在
它的实际分配者和所有 CPU 映射入口。

普通 A642L 渲染 BO 没有已发现的必须使用 NC 的硬件要求。对于本机控制的新分配，
WB coherent 是默认规则；现有 NC IOSurface 的形成位置需要在分配者处修正，
不能只在 GPU importer 中跳过 clean 后就视为完整实现。GMU、ZAP 等独立固件对象
采用其原厂访问约定，单列于分配器专题，不混入此 Metal 参数表。

原生 IOSurface 的接入必须在创建 handler 发布 surface ID、port 或 CPU 地址前完成：
取得真实对象和 descriptor，确认自行分配的图形 RAM，建立 WB backing 与所有 CPU
映射，然后才返回。已有 `native_legacy_surface_descriptor` 可以取得实际 descriptor，
创建 handler 的原生 ABI 绑定和登记调用属于本轮必须新增的适配。不能只靠用户传来的
`source=IOSurface` 标志授权修改任意外部页面。

页面发布后，正常规则是属性固定到 backing 释放。已发布的旧 NC surface 只有在原
所有者能保证全部 CPU/GPU/DPU 使用均已停止时才允许一次性转换；否则沿兼容路径
免逐行维护，等正常销毁后由新创建路径产生 WB surface。新镜像重启后创建的常规
surface 必须全部经过前述 WB 创建入口。设计不引入全 CPU 暂停去强改任意活跃表面，
也不把首次 GPU 导入作为强制转换外部页的接口。

当前可确认的常规 Metal 分配参数所对应的必要逐帧软件维护例外为零。以后新增例外
必须补充本表中的具体调用参数、真实属性和硬件限制，不能增加通用的“导入即维护”
或“非 WB 即维护”规则。

## 5. CPU 缓存指令的保留范围

常规 submit、GPU completion 和 DPU present 路径移除针对 coherent/NC backing 的
CPU clean/invalidate 调用。底层 helper 的存在不意味着这些路径允许调用它。

已有缓存页转换成 NC/WC 时，切换前可能需要完成一次清理、失效和映射同步，避免旧
缓存行与新属性并存；这项工作归属性转换的所有者，在停止有关访问之后进行，不由
每笔渲染提交重复执行。新分配的普通 WB coherent backing 不经过这一转换。

内核内部 ring、fence、页表及固件每个保留调用的位置、设备要求和操作时机在
[分配器专题](design/GPU_COHERENT_ALLOCATOR_2026-10-28.md)和
[显示/外部页专题](design/GPU_IMPORTED_MEMORY_DISPLAY_2026-10-28.md)中单列，
不能作为 Metal 常规资源的软件维护例外。

## 6. 实施完成的条件

- 普通 Metal 参数到 Vulkan memory type、Darwin ABI 和实际 BO 属性完整贯通；
  cached coherent 能力只在对应分配、CPU map、GPU map 和复用路径全部实现后公布。
- Metal 入口矩阵的常规资源全部有明确处理，不能仅修复某一类 IOSurface 或新建 buffer。
- import 分类保留真实属性及其取得依据，未知来源在导入时完成分类或明确返回不支持，
  不以每帧维护整块内存长期掩盖缺失的属性管理。
- GPU 与 DPU 的普通交接不调用 CPU 数据缓存逐行维护；硬件缓存事件、屏障与 fence
  仍按各自设备的实际要求执行。
- 内核内部 ring、fence、页表和固件内存分别有其设备访问方式，独立于 Metal 参数表。
- 实施后沿用有界累计记录区分实际 coherent/NC 映射及软件维护原因，不添加逐帧打印；
  性能采样确认实施效果，不承担选择默认内存策略的职责。
