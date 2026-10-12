#include "ui/keyboard_layout.h"

#include <math.h>
#include <string.h>

ui_key_rect_t ui_keyboard_band(int32_t width, int32_t height, bool round, int32_t y, int32_t h, int32_t inset)
{
    int32_t span = width - 2 * inset;
    if (round)
    {
        float radius = (float)(width < height ? width : height) / 2.0f - inset;
        float d0 = fabsf((float)y - (float)height / 2.0f);
        float d1 = fabsf((float)(y + h) - (float)height / 2.0f);
        float dy = fmaxf(d0, d1);
        float square = radius * radius - dy * dy;
        span = square > 0 ? (int32_t)(2.0f * sqrtf(square)) : 0;
    }
    return (ui_key_rect_t){(width - span) / 2, y, span, h};
}

static void split(ui_key_rect_t band, ui_key_rect_t *out, int count)
{
    for (int i = 0; i < count; i++)
    {
        int32_t x0 = band.w * i / count;
        int32_t x1 = band.w * (i + 1) / count;
        out[i] = (ui_key_rect_t){band.x + x0, band.y, x1 - x0, band.h};
    }
}

void ui_keyboard_layout_init(ui_keyboard_layout_t *out, int32_t width, int32_t height, bool round)
{
    memset(out, 0, sizeof(*out));
    int32_t pad_top = round ? height * 24 / 100 : 68;
    int32_t pad_bottom = round ? height * 70 / 100 : height - 52;
    out->header =
        ui_keyboard_band(width, height, round, round ? height * 7 / 100 : 0, round ? height * 8 / 100 : 36, 6);
    out->suggestions =
        ui_keyboard_band(width, height, round, round ? height * 16 / 100 : 36, round ? height * 7 / 100 : 32, 6);
    out->pad = (ui_key_rect_t){0, pad_top, width, pad_bottom - pad_top};
    for (int row = 0; row < 4; row++)
    {
        int32_t y0 = pad_top + out->pad.h * row / 4;
        int32_t y1 = pad_top + out->pad.h * (row + 1) / 4;
        ui_key_rect_t band = ui_keyboard_band(width, height, round, y0, y1 - y0, 6);
        band.y -= pad_top;
        split(band, &out->keys[row * 7], 7);
    }
    if (round)
    {
        ui_key_rect_t top = ui_keyboard_band(width, height, true, height * 71 / 100, height * 10 / 100, 6);
        ui_key_rect_t bottom = ui_keyboard_band(width, height, true, height * 82 / 100, height * 10 / 100, 6);
        split(top, out->controls, 3); /* shift, layer, apostrophe/cancel */
        /* Space has twice the width of backspace and Enter. */
        int32_t space = bottom.w / 2;
        out->controls[3] = (ui_key_rect_t){bottom.x, bottom.y, space, bottom.h};
        int32_t backspace = (bottom.w - space) / 2;
        out->controls[4] = (ui_key_rect_t){bottom.x + space, bottom.y, backspace, bottom.h};
        out->controls[5] =
            (ui_key_rect_t){bottom.x + space + backspace, bottom.y, bottom.w - space - backspace, bottom.h};
    }
    else
    {
        static const int weights[6] = {52, 60, 44, 124, 44, 44};
        int32_t x = 0;
        for (int i = 0; i < 6; i++)
        {
            int32_t next = i == 5 ? width : x + width * weights[i] / 368;
            out->controls[i] = (ui_key_rect_t){x, height - 52, next - x, 52};
            x = next;
        }
    }
}
