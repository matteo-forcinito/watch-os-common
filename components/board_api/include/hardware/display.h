#ifndef HARDWARE_DISPLAY_H
#define HARDWARE_DISPLAY_H

/*
 * The screen (e.g. SH8601 AMOLED, SPD2010 LCD - see the board's hardware layer).
 *
 * On AMOLED panels pixels make their own light: a black pixel is simply off
 * and costs (almost) nothing. That is why the UI uses black backgrounds and
 * why the always-on face is mostly black at low brightness. On LCD panels
 * the backlight costs the same whatever is shown.
 *
 * Drawing is asynchronous: display_draw() queues the pixels on the SPI bus
 * (DMA) and returns; `done` (from display_init) is called from an
 * interrupt when the transfer finished and the buffer can be reused.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*display_done_cb_t)(void *ctx);

esp_err_t display_init(display_done_cb_t done, void *ctx);

/* Send the pixels of the rectangle x1..x2, y1..y2 (inclusive), RGB565 big endian. */
esp_err_t display_draw(int x1, int y1, int x2, int y2, const void *pixels);

/*
 * Grow a dirty rectangle (inclusive) to what the controller can address:
 * many panels only accept windows aligned to 2 or 4 pixels. Called by the
 * LVGL port for every area it is about to redraw.
 */
void display_round_area(int *x1, int *y1, int *x2, int *y2);

/* 0 = darkest visible .. 255 = brightest. */
esp_err_t display_set_brightness(uint8_t level);

/*
 * Panel sleep: screen off, controller in its lowest-power state (the image
 * in its memory is kept). Waking takes ~120 ms.
 */
esp_err_t display_sleep(bool sleep);

uint16_t display_width(void);
uint16_t display_height(void);

#endif
