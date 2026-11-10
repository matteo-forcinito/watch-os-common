#ifndef REMOTE_BAR_LAYOUT_H
#define REMOTE_BAR_LAYOUT_H
#include "ui/keyboard_layout.h"

static inline bool remote_bar_size(int32_t w, int32_t h)
{
    return h <= 200 && w >= 2 * h;
}

static inline bool remote_narrow_size(int32_t w, int32_t h)
{
    return w <= 200 && h >= 2 * w;
}

/* Five shortcut keys and a 2x2 media bank below a compact two-line header. */
static inline ui_key_rect_t remote_bar_key(int32_t w, int32_t h, int index)
{
    if (remote_narrow_size(w, h))
    {
        int32_t height = (h - 250) / 5;
        return (ui_key_rect_t){8, 110 + height * index, w - 16, height};
    }
    int32_t bank = w * 3 / 4;
    int32_t left = 8 + (bank - 8) * index / 5;
    int32_t right = 8 + (bank - 8) * (index + 1) / 5;
    return (ui_key_rect_t){left, 74, right - left, h - 80};
}

static inline ui_key_rect_t remote_bar_media(int32_t w, int32_t h, int index)
{
    if (remote_narrow_size(w, h))
    {
        int32_t width = (w - 16) / 2;
        return (ui_key_rect_t){8 + width * (index % 2), h - 108 + 50 * (index / 2), width, 50};
    }
    int32_t left = w * 3 / 4 + 4, width = w - left - 8;
    int32_t height = (h - 80) / 2;
    return (ui_key_rect_t){left + width * (index % 2) / 2, 74 + height * (index / 2),
                           width / 2, height};
}
#endif
