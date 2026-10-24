# T733 CPU display and key-state services

`scripts/build-software-display.py` generates the original 21H461 Legacy class declarations and bindings, then compiles `NativeLegacyDisplay`, `NativeKeyStoreEvents`, `DisplayRegistration.cpp`, and `KeyStoreRegistration.cpp` into `build/native-legacy-display/libNativeLegacyDisplay.late.a` in the current XNU tree. It reads the supplied original kernelcache and the prelink layout's slide. It performs the parsing needed to bind the real methods directly; it does not invoke the upstream audit programs or calculate file hashes.

`DisplayRegistration.cpp` registers the native subclass after IOMobileGraphicsFamily loads. Once IOBSD exists, it loads IOSurface and publishes an IOPlatformDevice containing the actual firmware framebuffer range. The original Legacy family supplies user clients, surface references, request queues, and completion notifications. NativeLegacyDisplay copies validated completed BGRA surfaces with the CPU into the 1600x2560 framebuffer, then calls the original completion method. The first copy disables the text screen. Diagnostic messages continue through the kernel log path.

The `/chosen` display scale is 2, giving 800x1280 logical points. The T733 compile define treats the validated firmware panel as primary in both verbose text and graphics boot modes. `v_display` is the boot console mode, so a `-v` boot remains eligible to show the desktop. A 60 Hz work-loop timer supplies requested compositor refresh notifications. Actual frame cadence is limited by CPU rendering and copying.

The system configuration enables the existing original QuartzCore software path by exposing this framebuffer without publishing a Metal accelerator or plugin. GPU firmware and hardware drivers are outside this archive. Unknown original backend calls retain NativeLegacyDisplay's panic with the slot and caller so actual missing behavior can be located from logs.

`KeyStoreRegistration.cpp` registers the existing NativeKeyStoreEvents implementation under AppleKeyStore after IOBSD. Its client reports the configured state with no keybag or passcode (`sls=3`) and provides the existing notification and state-query selectors. It does not provide cryptographic key storage. The Data storage mode does not gate this service.

The archive compiled successfully on 2026-10-23 with the current XNU IOPlatformExpert recipe. Full kernel prelink, FAT packaging, and physical display results belong to the coordinating build and device run.
