// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
int t733_pmic_bus_start(void);
void t733_pmic_keys_start(void);
bool t733_pmic_keys_hid_start(void);
int t733_pmic_keys_hid_report(void *context, uint32_t pressed);
void t733_pmic_keys_dump(void);
#ifdef __cplusplus
}
#endif
