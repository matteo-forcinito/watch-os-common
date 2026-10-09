#ifndef HARDWARE_KNOB_H
#define HARDWARE_KNOB_H

/*
 * Rotary knob (only on boards with board_info()->has_knob).
 *
 * Turning it reports "detents" (clicks you feel): +1 clockwise, -1
 * counter-clockwise. Its push button is reported as a normal button
 * (hardware/button.h), not here.
 *
 * The callback runs in a driver task or timer: keep it short (post to a
 * queue / wake a task). Steps are accumulated, so a fast turn is reported
 * as one call with a bigger delta.
 */

#include "esp_err.h"

typedef void (*knob_cb_t)(int delta);

/* Boards without a knob return ESP_ERR_NOT_SUPPORTED. */
esp_err_t knob_init(knob_cb_t callback);

#endif
