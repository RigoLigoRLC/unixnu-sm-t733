# USB Ethernet DMA batches

The USB worker polls the DWC3 event ring once per millisecond. Endpoint 7 has
one finite transfer containing up to 32 Ethernet frames; endpoint 6 has one
finite transfer containing 16 receive buffers. Hardware advances between
frames while the worker sleeps. The network transmit queue holds 128 copied
frames and returns its existing errors when full or busy.

Each transmitted frame ends with an unchained normal TRB. A frame whose length
is an exact multiple of the endpoint maximum packet size uses a chained data
TRB followed by an unchained zero-length TRB. Only the last TRB of the batch
sets LST and IOC. A successful transfer-complete event releases all transmit
slots together after checking every TRB's ownership, status and residual size.

Receive TRBs set CSP and IOC so a short packet finishes its current buffer and
hardware proceeds to the next buffer. Endpoint 6 enables transfer-in-progress
events. The worker consumes consecutive completed TRBs in order, copies valid
frames into NativeUsbNetwork's bounded receive queue, and preserves oversized
frame discard state through the terminating short packet. The last TRB sets
LST. The buffers and TRBs are rebuilt only after transfer-complete accounts for
all 16 receive slots. Partial completion reads never rewrite active TRBs.

The permanent DMA allocation is 128 KiB, aligned to 128 KiB. Addresses below
0x4000 retain the control, serial and notification buffers. Ethernet storage is:

| Offset | Size | Contents |
| --- | --- | --- |
| 0x4000 | 256 bytes | 16 receive TRBs |
| 0x4400 | 1024 bytes maximum | 32 data TRBs and up to 32 zero-length TRBs |
| 0x8000 | 32 KiB | 16 receive buffers, 2048 bytes each |
| 0x10000 | 64 KiB | 32 transmit buffers, 2048 bytes each |

CPU cache maintenance publishes complete batches before START. A slot stays
allocated for the controller's entire lifetime. Alternate-setting changes end
the active endpoint transfers and drain old events before rebuilding batches;
controller recovery completes halt/reset before rewriting DMA. Serial logging
uses its existing endpoint and buffers. Polling retains its one-millisecond
sleep and does not spin for network completion.

The DWC3 field definitions, per-request CHN handling and explicit zero-length
TRBs follow the hardware procedures documented by Linux's
[gadget implementation](https://codebrowser.dev/linux/linux/drivers/usb/dwc3/gadget.c.html)
and [endpoint configuration definitions](https://codebrowser.dev/linux/linux/drivers/usb/dwc3/gadget.h.html).
The finite batches are implemented directly in NativeUsbDebug. The existing
source notices and NOTICE file continue to identify the original driver source.

The 67 capture saved 40,776,336 bytes in 43.485 seconds with one Ethernet frame
per polling iteration. The batch implementation removes that per-frame wait;
its deployed throughput remains to be recorded by the coordinating capture.
