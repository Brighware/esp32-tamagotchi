#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load the persisted wall-clock (or a default) into the system clock. */
void clock_boot_init(void);

/* Current wall-clock time in epoch seconds. */
int64_t clock_now(void);

/* Shift the clock by `seconds` (may be negative) and persist it. */
void clock_adjust(int seconds);

/* Persist the current time to NVS so it survives a reboot. */
void clock_persist(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOCK_H */
