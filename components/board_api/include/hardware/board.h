#ifndef HARDWARE_BOARD_H
#define HARDWARE_BOARD_H

/*
 * The board: what this device has, and the one call that wakes it all up.
 *
 * Everything above the hardware layer (core, services, apps) asks
 * board_info() instead of using pins or #ifdefs, so the same apps run on
 * any supported board.
 *
 *     board_init()
 *       ├─ I2C bus           (shared by touch, PMU, RTC, codec, expander)
 *       ├─ IO expander       (display/touch reset lines, if present)
 *       ├─ PMU               (battery, charger, PWR key)
 *       ├─ RTC               (time kept while the ESP32 is off)
 *       └─ SD card
 *
 * Display, touch and buttons are started by the core (LVGL port and power
 * manager), because they need callbacks from it.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct
{
    const char *name;        /* the exact board, for Settings > Info */
    const char *product;     /* "AMOLED Watch": web page title, setup Wi-Fi network */
    const char *short_name;  /* "AMOLED": Bluetooth name ("Bangle.js AMOLED") */
    uint16_t width;
    uint16_t height;
    bool round;           /* round panel: keep content away from the corners */
    bool has_touch;
    bool has_knob;        /* rotary knob (hardware/knob.h) */
    bool has_pmu;
    bool has_rtc;
    bool has_audio;
    bool has_sdcard;
    bool has_usb_otg;     /* ESP32 native USB on a connector: USB drive, USB keyboard */
    bool has_buttons;     /* At least one physical navigation button, not just a dial. */
    uint16_t display_corner_radius; /* Visible rectangular panel corner, pixels; 0 = square. */
} board_info_t;

/* Bring up the buses and the always-on chips. Call once, early in app_main(). */
esp_err_t board_init(void);

const board_info_t *board_info(void);

#endif
