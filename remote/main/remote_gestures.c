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

#include "core/lv_port.h"

#include <math.h>
#include <stdlib.h>

remote_edge_t remote_edge_at(lv_point_t p)
{
    if (remote_rails_wanted())
        return REMOTE_EDGE_NONE;   /* bar panels: the rails are separate controls */
    int32_t w = remote_screen_w(), h = remote_screen_h();
    int32_t band = LV_MIN(w, h) * 14 / 100;
    if (!board_info()->round && LV_MIN(w, h) <= 200)
        band = 32; /* A finger-sized target on the 172-pixel bar, in either orientation. */
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
    float step = LV_MIN(remote_screen_w(), remote_screen_h()) <= 200 && edge == REMOTE_EDGE_TOP ? 12 : step_px[edge];
    while (fabsf(*carry) >= step)
    {
        int sign = *carry > 0 ? 1 : -1;
        *carry -= sign * step;
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
        if (marks[i] != NULL)
            lv_obj_set_style_bg_color(marks[i], i == lit ? ui_accent() : ui_color(UI_COLOR_SEPARATOR), 0);
}

/* ------------------------------------------------------------------ rails */

/*
 * Bar panels (172 px on the short side): the edges of the pad are too thin to
 * find by feel, and a scroll that starts a pixel off moves the pointer. There
 * the gestures become real controls around a smaller pad:
 *
 *   portrait 172x640                  landscape 640x172
 *   ┌────────────────┐                ┌────┬──────────────────────┬────┬──────┐
 *   │  header        │                │ ♪  │         pad          │ ▲  │ Clic │
 *   ├──────────┬─────┤                │ ── │                      │ ── │      │
 *   │          │  ▲  │                │ ── ├──────────────────────┤ ── ├──────┤
 *   │   pad    │ ──  │  scroll        │ ── │  ◀ ── ── ── ── ── ▶  │ ▼  │Destro│
 *   │          │ ──  │                └────┴──────────────────────┴────┴──────┘
 *   ├──────────┴─────┤                 volume      horizontal scroll   scroll
 *   │ ◀ ── ── ── ▶   │  horizontal
 *   ├────────┬───────┤
 *   │  Clic  │ Destro│
 *   └────────┴───────┘
 *
 * A rail is a track with grip lines that follow the finger like a wheel.
 */

#define RAIL_GRIP_SPACING 16

typedef struct
{
    remote_edge_t edge;
    uint32_t accent;
    float carry;
    int32_t offset;
    int32_t length; /* travel of the grips (track length minus the icon) */
    lv_point_t last;
    lv_obj_t *grips[24];
    int grip_count;
} rail_t;

bool remote_rails_wanted(void)
{
    return !board_info()->round && LV_MIN(remote_screen_w(), remote_screen_h()) <= 200;
}

static void rail_place_grips(lv_obj_t *track, rail_t *rail)
{
    bool vertical = remote_edge_vertical(rail->edge);
    int32_t span = rail->grip_count * RAIL_GRIP_SPACING;
    int32_t shift = ((rail->offset % span) + span) % span;
    int32_t across = vertical ? lv_obj_get_width(track) : lv_obj_get_height(track);
    for (int i = 0; i < rail->grip_count; i++)
    {
        int32_t at = (i * RAIL_GRIP_SPACING + shift) % span;
        bool shown = at + 3 <= rail->length;
        lv_obj_set_hidden(rail->grips[i], !shown);
        int32_t mark = across * 45 / 100;
        if (vertical)
            lv_obj_set_pos(rail->grips[i], (across - mark) / 2, 28 + at);
        else
            lv_obj_set_pos(rail->grips[i], 28 + at, (across - mark) / 2);
    }
}

static void rail_light(lv_obj_t *track, rail_t *rail, bool on)
{
    lv_color_t tint = ui_color(rail->accent);
    lv_obj_set_style_bg_color(track, on ? lv_color_mix(tint, lv_color_hex(0x07080D), 70) : ui_color(UI_COLOR_CARD), 0);
    lv_obj_set_style_border_opa(track, on ? LV_OPA_COVER : LV_OPA_40, 0);
    for (int i = 0; i < rail->grip_count; i++)
        lv_obj_set_style_bg_color(rail->grips[i], on ? tint : ui_color(UI_COLOR_TEXT_DIM), 0);
}

static void rail_event(lv_event_t *e)
{
    lv_obj_t *track = lv_event_get_current_target_obj(e);
    rail_t *rail = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (code == LV_EVENT_DELETE)
    {
        free(rail);
        return;
    }
    if (indev == NULL)
        return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if (code == LV_EVENT_PRESSED)
    {
        rail->last = point;
        rail->carry = 0;
        rail_light(track, rail, true);
    }
    else if (code == LV_EVENT_PRESSING)
    {
        bool vertical = remote_edge_vertical(rail->edge);
        int32_t delta = vertical ? point.y - rail->last.y : point.x - rail->last.x;
        rail->last = point;
        if (delta == 0)
            return;
        remote_edge_move(rail->edge, delta, &rail->carry);
        rail->offset += delta;
        rail_place_grips(track, rail);
    }
    else
    {
        rail_light(track, rail, false);
    }
}

static lv_obj_t *rail_create(lv_obj_t *screen, ui_key_rect_t r, remote_edge_t edge, uint32_t accent)
{
    rail_t *rail = calloc(1, sizeof(*rail));
    if (rail == NULL)
        return NULL;
    rail->edge = edge;
    rail->accent = accent;
    bool vertical = remote_edge_vertical(edge);

    lv_obj_t *track = lv_obj_create(screen);
    lv_obj_remove_style_all(track);
    lv_obj_set_pos(track, r.x, r.y);
    lv_obj_set_size(track, r.w, r.h);
    lv_obj_set_style_radius(track, LV_MIN(r.w, r.h) / 2, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(track, 1, 0);
    lv_obj_set_style_border_color(track, ui_color(accent), 0);
    lv_obj_set_style_clip_corner(track, true, 0);
    lv_obj_set_scrollable(track, false);
    lv_obj_set_press_lock(track, true);
    lv_obj_set_gesture_bubble(track, false);
    lv_obj_set_ext_click_area(track, 6);
    lv_obj_add_event_cb(track, rail_event, LV_EVENT_PRESSED, rail);
    lv_obj_add_event_cb(track, rail_event, LV_EVENT_PRESSING, rail);
    lv_obj_add_event_cb(track, rail_event, LV_EVENT_RELEASED, rail);
    lv_obj_add_event_cb(track, rail_event, LV_EVENT_PRESS_LOST, rail);
    lv_obj_add_event_cb(track, rail_event, LV_EVENT_DELETE, rail);

    /* What the rail does, at its start; the grips fill the rest */
    static const char *const icons[] = {LV_SYMBOL_UP LV_SYMBOL_DOWN, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT,
                                        LV_SYMBOL_VOLUME_MAX, LV_SYMBOL_IMAGE};
    lv_obj_t *glyph = lv_label_create(track);
    lv_label_set_text(glyph, edge == REMOTE_EDGE_RIGHT ? LV_SYMBOL_UP : icons[edge]);
    lv_obj_set_style_text_font(glyph, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(glyph, ui_color(accent), 0);
    if (vertical)
        lv_obj_align(glyph, LV_ALIGN_TOP_MID, 0, 8);
    else
        lv_obj_align(glyph, LV_ALIGN_LEFT_MID, 8, 0);

    int32_t length = (vertical ? r.h : r.w) - 28 - 12;
    rail->length = length;
    rail->grip_count = LV_MIN(24, length / RAIL_GRIP_SPACING + 1);
    int32_t across = vertical ? r.w : r.h;
    int32_t mark = across * 45 / 100;
    for (int i = 0; i < rail->grip_count; i++)
    {
        lv_obj_t *grip = lv_obj_create(track);
        lv_obj_remove_style_all(grip);
        lv_obj_set_size(grip, vertical ? mark : 3, vertical ? 3 : mark);
        lv_obj_set_style_radius(grip, 2, 0);
        lv_obj_set_style_bg_opa(grip, LV_OPA_COVER, 0);
        lv_obj_remove_flag(grip, LV_OBJ_FLAG_CLICKABLE);
        rail->grips[i] = grip;
    }
    rail_light(track, rail, false);
    rail_place_grips(track, rail);
    return track;
}

static void click_key_event(lv_event_t *e)
{
    int button = (int)(intptr_t)lv_event_get_user_data(e);
    remote_send_ok(hid_link_click((uint8_t)button, 1));
}

static void click_key(lv_obj_t *screen, ui_key_rect_t r, const char *caption, int button, uint32_t accent,
                      bool primary)
{
    lv_obj_t *key = lv_obj_create(screen);
    lv_obj_remove_style_all(key);
    lv_obj_set_pos(key, r.x, r.y);
    lv_obj_set_size(key, r.w, r.h);
    lv_obj_set_style_radius(key, 18, 0);
    lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
    lv_color_t tint = ui_color(accent);
    lv_obj_set_style_bg_color(key, primary ? lv_color_mix(tint, lv_color_hex(0x07080D), 90) : ui_color(UI_COLOR_CARD), 0);
    lv_obj_set_style_border_width(key, 1, 0);
    lv_obj_set_style_border_color(key, tint, 0);
    lv_obj_set_style_border_opa(key, primary ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_bg_color(key, tint, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(key, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_scrollable(key, false);
    lv_obj_set_gesture_bubble(key, false);
    lv_obj_add_event_cb(key, click_key_event, LV_EVENT_CLICKED, (void *)(intptr_t)button);
    lv_port_set_focusable(key);
    lv_obj_t *label = lv_label_create(key);
    lv_label_set_text(label, caption);
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(label, primary ? ui_color(UI_COLOR_TEXT) : ui_color(UI_COLOR_TEXT_DIM), 0);
    lv_obj_center(label);
}

ui_key_rect_t remote_rails_create(lv_obj_t *screen, uint32_t accent)
{
    int32_t w = remote_screen_w(), h = remote_screen_h();
    ui_key_rect_t head = remote_header_band();
    const int32_t gap = 6;
    int32_t top = head.y + head.h + gap;
    if (w < h)
    {
        int32_t keys_h = 60, pan_h = 36, rail_w = 38;
        int32_t keys_y = h - gap - keys_h;
        int32_t pan_y = keys_y - gap - pan_h;
        int32_t half = (w - 3 * gap) / 2;
        click_key(screen, (ui_key_rect_t){gap, keys_y, half, keys_h}, "Clic", 1, accent, true);
        click_key(screen, (ui_key_rect_t){2 * gap + half, keys_y, w - 3 * gap - half, keys_h}, "Destro", 2, accent,
                  false);
        rail_create(screen, (ui_key_rect_t){gap, pan_y, w - 2 * gap, pan_h}, REMOTE_EDGE_BOTTOM, accent);
        rail_create(screen, (ui_key_rect_t){w - gap - rail_w, top, rail_w, pan_y - gap - top}, REMOTE_EDGE_RIGHT,
                    accent);
        return (ui_key_rect_t){gap, top, w - 3 * gap - rail_w, pan_y - gap - top};
    }
    int32_t keys_w = 84, rail_w = 42, pan_h = 32;
    int32_t bottom = h - gap;
    int32_t keys_x = w - gap - keys_w;
    int32_t scroll_x = keys_x - gap - rail_w;
    int32_t key_h = (bottom - top - gap) / 2;
    click_key(screen, (ui_key_rect_t){keys_x, top, keys_w, key_h}, "Clic", 1, accent, true);
    click_key(screen, (ui_key_rect_t){keys_x, top + key_h + gap, keys_w, bottom - top - key_h - gap}, "Destro", 2,
              accent, false);
    rail_create(screen, (ui_key_rect_t){scroll_x, top, rail_w, bottom - top}, REMOTE_EDGE_RIGHT, accent);
    rail_create(screen, (ui_key_rect_t){gap, top, rail_w, bottom - top}, REMOTE_EDGE_LEFT, accent);
    int32_t pad_x = 2 * gap + rail_w, pad_w = scroll_x - gap - pad_x;
    rail_create(screen, (ui_key_rect_t){pad_x, bottom - pan_h, pad_w, pan_h}, REMOTE_EDGE_BOTTOM, accent);
    return (ui_key_rect_t){pad_x, top, pad_w, bottom - pan_h - gap - top};
}

