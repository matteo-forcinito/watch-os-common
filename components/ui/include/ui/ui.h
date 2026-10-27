#ifndef UI_UI_H
#define UI_UI_H

/*
 * The look of the system: colors, fonts and ready-made widgets, so every app
 * looks the same and stays short.
 *
 *   NEON NOIR: pure black background (AMOLED: black pixels are off), thin
 *   neon outlines instead of filled bubbles, tracked uppercase captions,
 *   one accent colour, large touch targets, colour-flash press feedback,
 *   smooth scrolling with momentum. Tokens: core/ui_tokens.h.
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
#include "hardware/board.h"
#include "core/ui_tokens.h"

/*
 * ---- palette: NEON NOIR (see core/ui_tokens.h, the single source of the
 * design tokens). The UI_COLOR_* names are kept as aliases so every app
 * follows the theme; new code should use the TK_* tokens directly.
 */
#define UI_COLOR_BG          TK_VOID
#define UI_COLOR_CARD        TK_SURFACE_1
#define UI_COLOR_CARD_HI     TK_SURFACE_2
#define UI_COLOR_SEPARATOR   TK_LINE
#define UI_COLOR_TEXT        TK_TEXT
#define UI_COLOR_TEXT_DIM    TK_TEXT_2
#define UI_COLOR_RED         TK_RED
#define UI_COLOR_ORANGE      TK_AMBER
#define UI_COLOR_YELLOW      TK_AMBER
#define UI_COLOR_GREEN       TK_LIME
#define UI_COLOR_TEAL        TK_CYAN
#define UI_COLOR_BLUE        TK_CYAN
#define UI_COLOR_INDIGO      TK_VIOLET
#define UI_COLOR_PURPLE      TK_VIOLET
#define UI_COLOR_PINK        TK_MAGENTA
#define UI_COLOR_GRAY        TK_TEXT_3

/* Tracked uppercase HUD captions (ASCII only) and boot/unit micro text. */
#define UI_FONT_HUD     (&lv_font_montserrat_14)
#define UI_FONT_MICRO   (&lv_font_montserrat_12)
#define UI_HUD_LETTER_SPACE   3
#define UI_MICRO_LETTER_SPACE 2

/* UI dimensions follow the logical display; hardware/board.h stays native. */
const board_info_t *ui_board_info(void);

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
#define UI_FONT_CAPTION (&lv_font_montserrat_12) /* short ASCII labels on compact controls */

/* Theme, fonts and the shared widgets (call once, with the LVGL lock held). */
void ui_init(void);

lv_color_t ui_accent(void);
static inline lv_color_t ui_color(uint32_t hex) { return lv_color_hex(hex); }

/* Neon `color` mixed over black (TK_GLOW_G1/G2/G3): the "glow" of a colour.
 * Cheap but not free: call it when building widgets, not on every frame. */
static inline lv_color_t ui_glow(uint32_t color, uint8_t level)
{
    return lv_color_mix(lv_color_hex(color), lv_color_black(), level);
}

/* The user accent as 0xRRGGBB, mapped onto the neon palette. */
uint32_t ui_accent_hex(void);

/*
 * True while live animations should rest: a screen slide is running or the
 * user is dragging/scrolling. Every "live" timer checks it at each tick, so
 * transitions get the whole frame time.
 */
bool ui_motion_paused(void);

/*
 * Boot splash on lv_layer_top(), shown right after lv_port_init() (takes the
 * LVGL lock itself). It animates while services start, holds still while
 * the UI is built and dismisses itself once the home screen is loaded.
 * Never delays the boot; does nothing without a display (recovery).
 */
void ui_splash_show(void);

/* ---- layout */

/*
 * A vertical, scrolling page with a big title that scrolls away with the
 * content. Returns the container to put rows in. `title` may be NULL.
 */
lv_obj_t *ui_page(lv_obj_t *screen, const char *title);

/* The page title (ui_page adds one; for pages rebuilt with lv_obj_clean). Centred on round panels. */
lv_obj_t *ui_title(lv_obj_t *page, const char *text);

/* Section caption: "// SECTION" in tracked uppercase accent (plain small text when non-ASCII). */
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

/* HUD chip: outlined circle with a neon LV_SYMBOL, or an image ("S:/...") clipped to it. */
lv_obj_t *ui_icon_bubble(lv_obj_t *parent, const char *icon, uint32_t color, int32_t size);

/* Recolour a chip made by ui_icon_bubble() (ring, tint and glyph). */
void ui_icon_bubble_set_color(lv_obj_t *bubble, uint32_t color);

/* Give any object the card look + press feedback. */
void ui_make_card(lv_obj_t *obj);

/* Optional passive perimeter progress, quantized to 1 percent. No timer or
 * animation task; unchanged progress does not redraw. NULL is safe. */
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
