// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T733_TOUCH_FIRMWARE_PATH
#error T733_TOUCH_FIRMWARE_PATH must name the extracted FT8203 firmware
#endif

.section __TEXT,__const
.p2align 4
.globl _t733_touch_firmware
_t733_touch_firmware:
.incbin T733_TOUCH_FIRMWARE_PATH
L_t733_touch_firmware_end:
.p2align 2
.globl _t733_touch_firmware_size
_t733_touch_firmware_size:
.long L_t733_touch_firmware_end - _t733_touch_firmware
