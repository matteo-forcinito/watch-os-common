#ifndef HARDWARE_TOUCH_H
#define HARDWARE_TOUCH_H

/*
 * Capacitive touch (e.g. FT3168, SPD2010).
 *
 * The chip pulls its INT line low while a finger is on the glass. It is used
 * both as an interrupt (read the touch right away instead of polling all the
 * time) and as a wake-up source from light sleep. On some boards the line
 * reaches the ESP32 through an IO expander: same behaviour, a bit more delay.
 *
 * In low-power mode the chip scans slowly and uses very little power, but
 * still pulls INT low when touched: we use it while the screen is off.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Called from an interrupt or a driver task: only notify a task from here. */
typedef void (*touch_irq_cb_t)(void *ctx);

esp_err_t touch_init(touch_irq_cb_t on_touch, void *ctx);

/* Read the first touch point. Returns true while a finger is down. */
bool touch_read(uint16_t *x, uint16_t *y);

/* True while the INT pin is low (finger down), without an I2C transfer. */
bool touch_pin_active(void);

/* Re-arm the INT interrupt (it disables itself each time it fires). */
void touch_irq_arm(void);

/* Low-power scanning (still wakes on touch) or full speed. */
esp_err_t touch_set_low_power(bool low_power);

#endif
