# GPU fence 等待与本地开源驱动对照

2026-10-28。本次按用户要求只读对照当前实现与工作区内的 KGSL、DRM/MSM
和 Samsung SDE 源码；本记录不表示已修改这些等待策略。

## 当前结论

GPU 完成包使用 `CACHE_FLUSH_TS` 写入完成时间戳并请求中断，与 MSM A6xx
实现一致。差异集中在 CPU 等待、生产者选择和完成通知。

| 当前代码 | 本地参考 | 影响 |
| --- | --- | --- |
| `NativeAdrenoPublish.cpp` 的等待循环释放 GPU 锁后再次零超时查询；`NativeAdrenoService.cpp` 的 `idle` 执行 `IODelay(5)` 并读取中断状态 | KGSL `adreno_drawctxt.c` 注册 timestamp event 后进入 `wait_event_interruptible_timeout`；MSM `msm_fence.c` 等待队列由退休处理唤醒 | 等待期间持续消耗 CPU，代码中“等待 IRQ”的描述与实际执行不符 |
| `native_adreno_capture_surface_producer` 取得整个 ring 的 `lastSubmitted()` | SDE `msm_atomic.c` 从当前 framebuffer BO 取得 exclusive producer fence | 当前 Surface 可能等待捕获时已经提交的无关任务 |
| 显示生产者、显示工作循环及 DPU 完成通过定时器轮询 | SDE `sde_encoder_phys_vid.c` 在 vblank IRQ 中检查 CTL flush、发出 release/retire fence 并唤醒等待者 | 多层调度增加完成观察和扫描切换的通知延迟 |
| 当前组装 Mesa `tu_knl_darwin.cc` 的 `darwin_queue_submit` 对全部 wait 调用 `darwin_sync_wait` | DRM/MSM `msm_gem_submit.c` 对属于自身 ring context 的依赖跳过等待 | 已在同一有序 ring 上提交的依赖仍可能先阻塞 CPU |

最后一项已同时读取当前 `.cache/mesa-work` 中的组装源码，结论不只来自早期补丁。
同一 ring 的优化需要明确依赖已解析到该 ring；尚未收到的 host signal 仍有不同的
处理要求。完成 fence 同时承担 GPU 写出、对象寿命和访问顺序，不能只删等待调用。

## 源码入口

- 当前 GPU：[NativeAdrenoPublish.cpp](../drivers/Adreno6xx/NativeAdrenoPublish.cpp)、
  [NativeAdrenoService.cpp](../drivers/Adreno6xx/NativeAdrenoService.cpp)、
  [NativeAdrenoRing.cpp](../drivers/Adreno6xx/NativeAdrenoRing.cpp)。
- 当前显示：[NativeLegacyDisplay.cpp](../drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp)。
- KGSL：[adreno_drawctxt.c](../../../references/android_kernel_samsung_sm7325/drivers/gpu/msm/adreno_drawctxt.c)。
- DRM/MSM：[msm_fence.c](../../../references/android_kernel_samsung_sm7325/drivers/gpu/drm/msm/msm_fence.c)、
  [msm_gem_submit.c](../../../references/android_kernel_samsung_sm7325/drivers/gpu/drm/msm/msm_gem_submit.c)、
  [a6xx_gpu.c](../../../references/android_kernel_samsung_sm7325/drivers/gpu/drm/msm/adreno/a6xx_gpu.c)。
- Samsung SDE：[msm_atomic.c](../../../references/android_kernel_samsung_sm7325/techpack/display/msm/msm_atomic.c)、
  [sde_encoder_phys_vid.c](../../../references/android_kernel_samsung_sm7325/techpack/display/msm/sde/sde_encoder_phys_vid.c)。
