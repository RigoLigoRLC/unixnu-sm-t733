# Early framebuffer diagnostics

These headers implement framebuffer stage text, values and panic output for the
seven patches listed in `patches/t733-xnu/early-entry.list`.
`XNU_EARLY_ENTRY_MARKER=1` selects the patches and adds this include directory to
their compilation units. Both device defconfigs disable this group.

USB logging and task sampling use NativeUsbDebug. They remain available with the
early framebuffer group disabled.
