/*
 * Edge gestures shared by the touch surfaces of Remote (mouse, air mouse).
 *
 *   along the right edge   vertical scroll     along the bottom edge  horizontal scroll
 *   along the left edge    PC volume           along the top edge     PC brightness
 *
 * An edge claims a touch only when the touch starts on it and its first move
 * goes along the edge (the caller decides with remote_edge_claims()): a tap on
 * an edge stays a click and a move inwards stays the pointer. Round panels use
 * the outer ring, split in four sectors around the centre.
 */

#include "remote.h"
#include "hardware/board.h"
#include "ui/ui.h"

#include <math.h>

remote_edge_t remote_edge_at(lv_point_t p)
{
    int32_t w = remote_screen_w(), h = remote_screen_h();
    int32_t band = LV_MIN(w, h) * 14 / 100;
    if (board_info()->round)
    {
        int32_t dx = p.x - w / 2, dy = p.y - h / 2, inner = LV_MIN(w, h) / 2 - band;
        if (dx * dx + dy * dy < inner * inner)
            return REMOTE_EDGE_NONE;
        if (LV_ABS(dx) >= LV_ABS(dy))
            return dx > 0 ? REMOTE_EDGE_RIGHT : REMOTE_EDGE_LEFT;
        return dy > 0 ? REMOTE_EDGE_BOTTOM : REMOTE_EDGE_TOP;
    }
    int32_t distance[4] = {w - 1 - p.x, h - 1 - p.y, p.x, p.y};
    remote_edge_t edge = REMOTE_EDGE_NONE;
    for (int i = 0; i < 4; i++)
        if (distance[i] < band && (edge == REMOTE_EDGE_NONE || distance[i] < distance[edge]))
            edge = (remote_edge_t)i;
    return edge;
}

bool remote_edge_vertical(remote_edge_t edge)
{
    return edge == REMOTE_EDGE_RIGHT || edge == REMOTE_EDGE_LEFT;
}

bool remote_edge_claims(remote_edge_t edge, int32_t tx, int32_t ty)
{
    if (edge == REMOTE_EDGE_NONE)
        return false;
    return remote_edge_vertical(edge) ? LV_ABS(ty) > LV_ABS(tx) : LV_ABS(tx) > LV_ABS(ty);
}

const char *remote_edge_name(remote_edge_t edge)
{
    static const char *const names[] = {"Scorri", "Scorri", "Volume", "Luminosità"};
    return edge == REMOTE_EDGE_NONE ? "" : names[edge];
}

void remote_edge_move(remote_edge_t edge, int32_t delta, float *carry)
{
    static const float step_px[] = {12, 12, 18, 24}; /* right, bottom, left, top */
    if (edge == REMOTE_EDGE_NONE)
        return;
    *carry += delta;
    while (fabsf(*carry) >= step_px[edge])
    {
        int sign = *carry > 0 ? 1 : -1;
        *carry -= sign * step_px[edge];
        switch (edge)
        {
        case REMOTE_EDGE_RIGHT: /* content follows the finger, as on a phone */
            hid_link_pointer(0, 0, 0, remote_wheel((int8_t)sign), 0);
            break;
        case REMOTE_EDGE_BOTTOM:
            hid_link_pointer(0, 0, 0, 0, remote_pan((int8_t)-sign));
            break;
        case REMOTE_EDGE_LEFT: /* finger up: louder */
            hid_link_media(sign < 0 ? HIDL_MEDIA_VOLUME_UP : HIDL_MEDIA_VOLUME_DOWN);
            break;
        case REMOTE_EDGE_TOP: /* finger right: brighter (Consumer Display Brightness +/-) */
            hid_link_media(sign > 0 ? 0x006F : 0x0070);
            break;
        default:
            break;
        }
    }
}

lv_obj_t *remote_edge_mark(lv_obj_t *parent, remote_edge_t edge)
{
    static const char *const icons[] = {LV_SYMBOL_UP "\n" LV_SYMBOL_DOWN, LV_SYMBOL_LEFT "  " LV_SYMBOL_RIGHT,
                                        LV_SYMBOL_VOLUME_MAX, LV_SYMBOL_IMAGE};
    static const lv_align_t aligns[] = {LV_ALIGN_RIGHT_MID, LV_ALIGN_BOTTOM_MID, LV_ALIGN_LEFT_MID, LV_ALIGN_TOP_MID};
    static const int8_t sx[] = {-1, 0, 1, 0}, sy[] = {0, -1, 0, 1};
    int32_t w = remote_screen_w(), h = remote_screen_h();
    bool round = board_info()->round, vertical = remote_edge_vertical(edge);
    int32_t inset = round ? LV_MIN(w, h) * 4 / 100 : 5, thick = 4;
    int32_t length = (vertical ? h : w) * (round ? 22 : 30) / 100;

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, vertical ? thick : length, vertical ? length : thick);
    lv_obj_set_style_radius(bar, thick / 2, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, ui_color(UI_COLOR_SEPARATOR), 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(bar, aligns[edge], sx[edge] * inset, sy[edge] * inset);

    lv_obj_t *glyph = lv_label_create(parent);
    lv_label_set_text(glyph, icons[edge]);
    lv_obj_set_style_text_font(glyph, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(glyph, ui_color(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(glyph, LV_TEXT_ALIGN_CENTER, 0);
    int32_t gap = inset + thick + 8;
    lv_obj_align(glyph, aligns[edge], sx[edge] * gap, sy[edge] * gap);
    return bar;
}

void remote_edge_light(lv_obj_t *const marks[4], remote_edge_t lit)
{
    for (int i = 0; i < 4; i++)
        lv_obj_set_style_bg_color(marks[i], i == lit ? ui_accent() : ui_color(UI_COLOR_SEPARATOR), 0);
}
