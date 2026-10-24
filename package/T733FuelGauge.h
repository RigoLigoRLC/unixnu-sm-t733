// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum T733FuelGaugeValid {
    T733_FG_SOC = 1u << 0,
    T733_FG_VOLTAGE = 1u << 1,
    T733_FG_CURRENT = 1u << 2,
    T733_FG_TEMPERATURE = 1u << 3,
    T733_FG_CHARGER_STATUS = 1u << 4,
    T733_FG_CYCLE = 1u << 5,
};

/* T733FuelGaugeSample contains only fields obtained in the current read.
 * The battery service owns polling, retention of older values and IOKit publication.
 */
struct T733FuelGaugeSample {
    uint32_t valid;
    int32_t soc_permille;                 // 0..1000, measured SOC without Android UI scaling.
    int32_t voltage_mv;
    int32_t current_ma;                   // Positive into the battery, negative out.
    int32_t gauge_temperature_deci_c;     // Gauge internal temperature, not pack NTC.
    uint32_t cycle_count;                 // Native low eight-bit counter.
    uint8_t battery_present;
    uint8_t external_power;
    uint8_t charging;
    uint8_t full;                         // Charger top-off status.
    uint16_t device_id;
    uint16_t system_status;
    uint16_t auxiliary_status;
    uint16_t raw_soc;
    uint8_t charger_status1;
    uint8_t charger_status2;
};

/* These calls run on the battery worker, serialized and outside interrupt context.
 * A negative errno reports failed bus reads. read clears the sample first and sets
 * valid bits for successful fields even when another field fails.
 */
int t733_fuel_gauge_init(void);
int t733_fuel_gauge_read(struct T733FuelGaugeSample *sample);

/* T733FuelGaugeIRQ accumulates read-to-clear status from the shared SM5714 line.
 * The battery service owns GPIO54 masking, worker scheduling and status publication.
 */
struct T733FuelGaugeIRQ {
    uint8_t source_seen;
    uint8_t source_remaining;
    uint8_t charger[5];
    uint8_t muic[2];
    uint16_t fuel_gauge;
    uint32_t passes;
};

/* setup masks all groups, clears latched status, then enables power-state events.
 * drain performs at most four rounds of pending status reads. A negative errno means that
 * the caller must keep GPIO54 masked and retry on its worker. Even after success,
 * the caller must observe GPIO54 high before re-enabling its level interrupt.
 */
int t733_fuel_gauge_irq_setup(void);
int t733_fuel_gauge_irq_drain(struct T733FuelGaugeIRQ *irq);

#ifdef __cplusplus
}
#endif
