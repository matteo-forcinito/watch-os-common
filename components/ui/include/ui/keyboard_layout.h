#ifndef UI_KEYBOARD_LAYOUT_H
#define UI_KEYBOARD_LAYOUT_H

/* Pixel geometry shared by the local and HID keyboards. Every key rectangle
 * fits inside the visible circle; drawing and swipe decoding use these bounds. */
#include <stdbool.h>
#include <stdint.h>

#define UI_KEYBOARD_KEYS 28
#define UI_KEYBOARD_CONTROLS 6

typedef struct
{
    int32_t x, y, w, h;
} ui_key_rect_t;

typedef struct
{
    ui_key_rect_t header;
    ui_key_rect_t suggestions;
    ui_key_rect_t pad;
    ui_key_rect_t keys[UI_KEYBOARD_KEYS];         /* relative to pad */
    ui_key_rect_t controls[UI_KEYBOARD_CONTROLS]; /* relative to screen */
} ui_keyboard_layout_t;

/* Maximum horizontal band whose four corners fit the round panel. */
ui_key_rect_t ui_keyboard_band(int32_t width, int32_t height, bool round, int32_t y, int32_t h, int32_t inset);
void ui_keyboard_layout_init(ui_keyboard_layout_t *out, int32_t width, int32_t height, bool round);

#endif
