// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "T733PmicKeys.h"
#include "NativeQcomSpmi.h"
#include "NativePmicKeys.h"
#include <IOKit/IOLib.h>
#include <kern/thread.h>

extern "C" int t733_pmic_bus_start(void)
{
    static const NativeQcomSpmiConfig config = {
        0x0c440000, 0x1100,
        0x0c600000, 0x02000000,
        0x0e600000, 0x00100000,
        0x0e700000, 0x000a0000,
        0x0c40a000, 0x00026000,
        0x0b220000, 0x2000,
        1, 513, 0,
    };
    return native_qcom_spmi_start(&config);
}

namespace {
static void start(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "t733-pmic-start");
    IOSleep(6000);
    int result = t733_pmic_bus_start();
    if (result) {
        printf("T733 keys: SPMI start=%d\n", result);
        return;
    }

    // PM7325 GPIO6 uses input mode, VIN1 and its 30-uA internal pull-up.
    result = native_qcom_spmi_update8(1, 0x8d40, 0x7f, 0);
    if (!result) {
        result = native_qcom_spmi_update8(1, 0x8d41, 7, 1);
    }
    if (!result) {
        result = native_qcom_spmi_update8(1, 0x8d42, 7, 0);
    }
    if (!result) {
        result = native_qcom_spmi_update8(1, 0x8d43, 0xff, 0);
    }
    if (!result) {
        result = native_qcom_spmi_update8(1, 0x8d46, 0x80, 0x80);
    }
    uint8_t debounce = 0;
    if (!result) {
        result = native_qcom_spmi_read(0, 0x0871, &debounce, 1);
    }
    if (result) {
        printf("T733 keys: PMIC configure=%d\n", result);
        return;
    }
    unsigned shift = debounce & 15;
    unsigned powerDebounce = shift <= 14 ? 1000000u / (1u << (14 - shift)) : 2000000u;
    printf("T733 keys: PMK8350 PBS debounce=%02x duration-us=%u\n", debounce, powerDebounce);

    if (!t733_pmic_keys_hid_start()) {
        printf("T733 keys: original HID startup failed\n");
        return;
    }
    NativePmicKey keys[] = {
        {0, 0x13, 0x80, false, 1u << 1, powerDebounce},
        {1, 0x8d, 0x01, true, 1u << 2, 15000},
        {0, 0x13, 0x40, false, 1u << 3, 15000},
    };
    result = native_pmic_keys_start(keys, sizeof(keys) / sizeof(keys[0]), t733_pmic_keys_hid_report, nullptr);
    printf("T733 keys: PMK8350 PON and PM7325 GPIO6 start=%d\n", result);
}
} // namespace

extern "C" void t733_pmic_keys_start(void)
{
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(start, nullptr, &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
    }
}

extern "C" void t733_pmic_keys_dump(void)
{
    native_qcom_spmi_dump();
    native_pmic_keys_dump();
}
