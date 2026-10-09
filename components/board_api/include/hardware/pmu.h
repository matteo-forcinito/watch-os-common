#ifndef HARDWARE_PMU_H
#define HARDWARE_PMU_H

/*
 * Power: battery, charger, USB cable and power switch.
 *
 * A real PMU (AXP2101) or a simpler circuit (charger status pins + battery
 * voltage on the ADC + a power latch): the board decides. pmu_present() is
 * true when at least the battery level is known.
 *
 * On boards with a PWR key wired to the PMU, the PMU latches presses and
 * pmu_poll_key() reads and clears them; elsewhere it returns PMU_KEY_NONE.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum
{
    PMU_KEY_NONE,
    PMU_KEY_SHORT,
    PMU_KEY_LONG,
} pmu_key_t;

esp_err_t pmu_init(void);

bool pmu_present(void);

/* 0..100, or -1 if unknown (no battery / no PMU). */
int pmu_battery_percent(void);

/* Battery voltage in millivolts, 0 if unknown. */
int pmu_battery_mv(void);

/*
 * Battery voltage measured right now (pmu_battery_mv() may return a value a
 * few seconds old). For the battery health test, which compares the voltage
 * with and without a load. Blocks a few ms; not from an interrupt.
 */
int pmu_battery_mv_fresh(void);

bool pmu_battery_present(void);
bool pmu_is_charging(void);
bool pmu_usb_present(void);

/* Last PWR key press since the previous call. */
pmu_key_t pmu_poll_key(void);

/* Cut the power (like holding PWR). The board starts again with a PWR press. */
void pmu_power_off(void);

#endif
