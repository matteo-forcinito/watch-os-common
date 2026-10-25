/* Compact transfer progress. A perimeter line invalidates nearly the whole
 * display on every percentage change; this only redraws a narrow strip. */
#include "ui/ui.h"
#include "hardware/board.h"

lv_obj_t *ui_border_progress_create(lv_obj_t *screen, uint32_t color)
{
    lv_obj_t *bar = lv_bar_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_ignore_layout(bar, true);
    lv_obj_set_size(bar, board_info()->width * 56 / 100, 6);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0,
                 board_info()->round ? -board_info()->height * 14 / 100 : -12);
    lv_obj_set_clickable(bar, false);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_CARD_HI), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    return bar;
}

void ui_border_progress_set(lv_obj_t *bar, int32_t value, int32_t maximum)
{
    if (bar == NULL) return;
    int32_t pct = maximum > 0 ? (int32_t)((int64_t)value * 100 / maximum) : 0;
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    if (lv_bar_get_value(bar) != pct) lv_bar_set_value(bar, pct, LV_ANIM_OFF);
}

void ui_border_progress_color(lv_obj_t *bar, uint32_t color)
{
    if (bar != NULL) lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
}
