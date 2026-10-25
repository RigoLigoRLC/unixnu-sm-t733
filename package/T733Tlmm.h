// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum T733TlmmTrigger {
    T733_TLMM_LEVEL_LOW,
    T733_TLMM_LEVEL_HIGH,
    T733_TLMM_EDGE_FALLING,
    T733_TLMM_EDGE_RISING,
    T733_TLMM_EDGE_BOTH,
};

typedef void (*T733TlmmHandler)(unsigned pin, void *context);

/* Registration runs on a thread and reserves the pin for the lifetime of the boot.
 * The controller configures GPIO input and leaves its summary interrupt masked.
 * Callbacks only schedule work: they run in interrupt context or synchronously
 * from rearm when an edge arrived while masked. They must not sleep or access a bus.
 */
int t733_tlmm_irq_register(unsigned pin, enum T733TlmmTrigger trigger,
    T733TlmmHandler handler, void *context);

/* mask preserves raw edge capture. ack explicitly discards the current latch;
 * normal worker completion uses rearm without ack so new edges remain observable.
 * A level interrupt rearms only after its active level has cleared. An edge
 * already latched at rearm is acknowledged and delivered with the pin still masked.
 */
void t733_tlmm_irq_mask(unsigned pin);
void t733_tlmm_irq_ack(unsigned pin);
bool t733_tlmm_irq_rearm(unsigned pin);
int t733_tlmm_level(unsigned pin);
uint32_t t733_tlmm_irq_count(unsigned pin);
void t733_tlmm_dump_stats(void);

#ifdef __cplusplus
}
#endif
