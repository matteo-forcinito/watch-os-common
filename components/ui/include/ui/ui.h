#ifndef UI_UI_H
#define UI_UI_H

/*
 * The look of the system: colors, fonts and ready-made widgets, so every app
 * looks the same and stays short.
 *
 *   Pure black background (AMOLED: black pixels are off), rounded cards,
 *   one accent color, large touch targets, press feedback that shrinks the
 *   card a little (feels physical), smooth scrolling with momentum.
 *
 * A typical settings-like page:
 *
 *     lv_obj_t *page = ui_page(screen, "Display");
 *     ui_slider_row(page, LV_SYMBOL_IMAGE, "Luminosità", 10, 255, value, on_change, NULL);
 *     ui_switch_row(page, LV_SYMBOL_EYE_OPEN, UI_COLOR_PURPLE, "Always on", on, on_toggle, NULL);
 *     ui_row(page, LV_SYMBOL_WIFI, UI_COLOR_BLUE, "Wi-Fi", "Casa", on_click, NULL);
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* ---- palette (iOS / watchOS-like system colors, readable on black) */
#define UI_COLOR_BG          0x000000
#define UI_COLOR_CARD        0x1C1C1E
#define UI_COLOR_CARD_HI     0x2C2C2E
#define UI_COLOR_SEPARATOR   0x38383A
#define UI_COLOR_TEXT        0xFFFFFF
#define UI_COLOR_TEXT_DIM    0x8E8E93
#define UI_COLOR_RED         0xFF453A
#define UI_COLOR_ORANGE      0xFF9F0A
#define UI_COLOR_YELLOW      0xFFD60A
#define UI_COLOR_GREEN       0x30D158
#define UI_COLOR_TEAL        0x40C8E0
#define UI_COLOR_BLUE        0x0A84FF
#define UI_COLOR_INDIGO      0x5E5CE6
#define UI_COLOR_PURPLE      0xBF5AF2
#define UI_COLOR_PINK        0xFF375F
#define UI_COLOR_GRAY        0x636366

/* ---- fonts */
extern const lv_font_t *ui_font_clock;      /* huge light digits (watch face) */
extern const lv_font_t *ui_font_big;        /* large light digits / numbers */
extern const lv_font_t *ui_font_title;      /* page titles (semibold) */
extern const lv_font_t *ui_font_headline;   /* big semibold text (station, city) */
/*
 * Text fonts: LVGL's pre-rendered Montserrat (fast, ASCII + symbols) with a
 * Montserrat TTF fallback for accents and the like (à è ì ò ù ° · €).
 */
extern const lv_font_t *ui_font_small;      /* 16 px */
extern const lv_font_t *ui_font_body;       /* 20 px */
extern const lv_font_t *ui_font_large;      /* 24 px */
#define UI_FONT_SMALL   (ui_font_small)
#define UI_FONT_BODY    (ui_font_body)
#define UI_FONT_LARGE   (ui_font_large)
#define UI_FONT_ICON    (&lv_font_montserrat_24)

/* Theme, fonts and the shared widgets (call once, with the LVGL lock held). */
void ui_init(void);

lv_color_t ui_accent(void);
static inline lv_color_t ui_color(uint32_t hex) { return lv_color_hex(hex); }

/* ---- layout */

/*
 * A vertical, scrolling page with a big title that scrolls away with the
 * content. Returns the container to put rows in. `title` may be NULL.
 */
lv_obj_t *ui_page(lv_obj_t *screen, const char *title);

/* The page title (ui_page adds one; for pages rebuilt with lv_obj_clean). Centred on round panels. */
lv_obj_t *ui_title(lv_obj_t *page, const char *text);

/* Small gray caption between groups of rows. */
lv_obj_t *ui_section(lv_obj_t *page, const char *text);

/* Plain text paragraph (wraps). */
lv_obj_t *ui_text(lv_obj_t *page, const char *text, bool dim);

/* ---- rows (cards) */

/* Clickable row: [icon bubble] text ........ value > */
lv_obj_t *ui_row(lv_obj_t *page, const char *icon, uint32_t color, const char *text,
                 const char *value, lv_event_cb_t on_click, void *user_data);

/* The value label of a row created by ui_row() (to update it). */
lv_obj_t *ui_row_value(lv_obj_t *row);

/* Row with a switch. on_change gets LV_EVENT_VALUE_CHANGED from the switch. */
lv_obj_t *ui_switch_row(lv_obj_t *page, const char *icon, uint32_t color, const char *text,
                        bool on, lv_event_cb_t on_change, void *user_data);

/* Row with a title and a slider under it. Returns the slider. */
lv_obj_t *ui_slider_row(lv_obj_t *page, const char *icon, const char *text,
                        int32_t min, int32_t max, int32_t value, lv_event_cb_t on_change, void *user_data);

/* Big rounded button. */
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t on_click, void *user_data);

/* Round colored bubble with an LV_SYMBOL or an image ("S:/..."). */
lv_obj_t *ui_icon_bubble(lv_obj_t *parent, const char *icon, uint32_t color, int32_t size);

/* Give any object the card look + press feedback. */
void ui_make_card(lv_obj_t *obj);

/* Passive screen-border progress. Call from the LVGL task; NULL is safe.
 * Circle on round boards, rounded rectangle otherwise. No animation task.
 * Parent must cover the display; delete the parent to free the instance. */
lv_obj_t *ui_border_progress_create(lv_obj_t *screen, uint32_t color);
void ui_border_progress_set(lv_obj_t *border, int32_t value, int32_t maximum);
void ui_border_progress_color(lv_obj_t *border, uint32_t color);

/* ---- overlays */

/* Short message sliding in from the top, gone after ~2 s. */
void ui_toast(const char *text);

/* Modal question. cb(confirmed, user_data) runs in the lvgl task. */
typedef void (*ui_confirm_cb_t)(bool confirmed, void *user_data);
void ui_confirm(const char *title, const char *text, const char *ok_text, uint32_t ok_color,
                ui_confirm_cb_t cb, void *user_data);

/*
 * Full-screen text entry (keyboard). cb(text, user_data) is called with the
 * text on "OK", or with NULL if cancelled.
 */
typedef void (*ui_text_cb_t)(const char *text, void *user_data);
void ui_text_input(const char *title, const char *initial, bool password, ui_text_cb_t cb, void *user_data);

/* ---- drawings */

/*
 * Weather icon drawn with shapes (no image files), from a WMO weather code
 * (the one Open-Meteo returns): 0 clear, 1-3 clouds, 45 fog, 61 rain,
 * 71 snow, 95 storm... `night` draws a moon instead of the sun.
 */
lv_obj_t *ui_weather_icon(lv_obj_t *parent, int wmo_code, bool night, int32_t size);

/* Short Italian description of a WMO code ("Sereno", "Pioggia"...). */
const char *ui_weather_text(int wmo_code);

/*
 * Image source for an SD card path ("S:/apps/x/icon.png"): PNG and LVGL 9
 * .bin are returned as is, LVGL 8 .bin (old Arduino launcher icons) are
 * converted once and cached. NULL if the file cannot be used.
 */
const void *ui_image_src(const char *path);

/* Battery symbol for a percentage. */
const char *ui_battery_symbol(int percent);

#endif
