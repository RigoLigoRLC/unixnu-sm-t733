# GPU copy resources in the capture

The copy investigation records resource information while backboardd continues
rendering. Breakpoints and process suspension are excluded from performance
capture. Package 67 is running on the tablet. The changes described here are
source changes until the build and deployment results below are recorded.

## Correlation

The Metal runtime assigns one operation ID immediately before each Vulkan
buffer-to-image copy. Its bounded metadata records contain the Vulkan source
buffer, destination image, up to 32 copy regions and the runtime call's purpose.
The original region count and a truncation flag identify larger copies.
Image records preserve the texture identity, buffer relationship, row pitches
and the reason the image could not use the buffer in place. Existing images
emit these descriptions when used during a capture.

Turnip saves the operation ID only for the next copy with matching source and
destination handles on the same command buffer. Resetting a command buffer or
consuming that copy clears the pending association. The existing GPU start
timestamp payload retains the ID and both handles; the GPU timestamp count is
unchanged. The host joins these records to show the exact copy parameters on
the GPU duration slice. Missing source metadata remains explicitly missing.

The 176-byte event layout is unchanged. Metal points 8--14 carry the operation,
region, subresource, offset, image identity, layout and in-place refusal text.
Refusal text retains up to 256 bytes, with the original length for detecting
truncation. Turnip argument keys 34--36 carry source buffer, destination image
and operation ID. Raw records remain available for later analysis.

## USB transport

Package 67 transmits at roughly 0.8 MB/s during the capture because each 1 ms
worker iteration can start only one Ethernet frame. The first 20-second window
saved 230224 events and missed 147074 sequence positions; 905 hardware samples
and 166 GPU jobs were retained without losses. The capture remains incomplete.

Commit `8e109a1` queues up to 32 transmit frames and 16 receive buffers in DWC3
TRBs, retaining the existing worker sleep. Transfer completion determines when
the fixed DMA buffers can be reused. Implementation and reference details are
in `drivers/NativeUsbDebug/NETWORK_DMA.md`.
