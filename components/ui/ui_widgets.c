#include "ui/ui.h"

#include "core/lv_port.h"
#include "hardware/board.h"

#include <string.h>

const board_info_t *ui_board_info(void) {
    static board_info_t view;
    view=*board_info();
    lv_display_t *display=lv_display_get_default();
    if(display) {
        view.width=lv_display_get_horizontal_resolution(display);
        view.height=lv_display_get_vertical_resolution(display);
    }
    return &view;
}

#define ROW_HEIGHT      TK_ROW_H
#define CARD_RADIUS     TK_R_S
#define BUBBLE_SIZE     40
#define PAGE_PAD_X      TK_SP_3

/*
 * Round panels. The circle is narrow at the top and bottom, so a page:
 *   - centres its title low enough to be fully visible (~230 px of width at y = 36)
 *   - keeps rows away from the edge and leaves room at the end, so the last
 *     row can scroll up to the wide middle
 *   - shows its scroll position as a short arc along the right edge
 */
#define ROUND_PAD_TOP     TK_ROUND_PAD_TOP
#define ROUND_PAD_X       TK_ROUND_PAD_X
#define ROUND_PAD_BOTTOM  TK_ROUND_PAD_BOTTOM
#define ROUND_ARC_SPAN    56    /* degrees of the scroll track, centred on 3 o'clock */
#define ROUND_ARC_MIN     10    /* shortest thumb, degrees */

static lv_style_t s_card;
static lv_style_t s_card_pressed;
static lv_style_transition_dsc_t s_transition;
static bool s_styles_ready = false;

static void init_styles(void)
{
    if (s_styles_ready)
    {
        return;
    }

    /* Scaling a row forces a temporary compositing layer on every touch and
     * scroll. A colour transition gives feedback without allocating a layer. */
    static const lv_style_prop_t props[] = {LV_STYLE_BG_COLOR, 0};
    lv_style_transition_dsc_init(&s_transition, props, lv_anim_path_ease_out, TK_T_INSTANT, 0, NULL);

    /* HUD panel: near-black surface framed by a hairline; no shadow, no gradient. */
    lv_style_init(&s_card);
    lv_style_set_bg_color(&s_card, lv_color_hex(TK_SURFACE_1));
    lv_style_set_bg_opa(&s_card, LV_OPA_COVER);
    lv_style_set_radius(&s_card, CARD_RADIUS);
    lv_style_set_border_width(&s_card, TK_LW_HAIR);
    lv_style_set_border_color(&s_card, lv_color_hex(TK_LINE));
    lv_style_set_pad_hor(&s_card, TK_SP_3);
    lv_style_set_pad_ver(&s_card, TK_SP_2);
    lv_style_set_text_color(&s_card, lv_color_hex(TK_TEXT));
    lv_style_set_transition(&s_card, &s_transition);

    /* Pressed: colour flash plus a lit frame, visible within one frame. */
    lv_style_init(&s_card_pressed);
    lv_style_set_bg_color(&s_card_pressed, lv_color_hex(TK_SURFACE_2));
    lv_style_set_border_color(&s_card_pressed, lv_color_hex(TK_LINE_HI));

    s_styles_ready = true;
}

void ui_make_card(lv_obj_t *obj)
{
    init_styles();
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &s_card, 0);
    lv_obj_add_style(obj, &s_card_pressed, LV_STATE_PRESSED);
    lv_obj_set_scrollable(obj, false);
}

/* ------------------------------------------------------------- layout */

/* Round panels: move the thumb of the edge arc to the scroll position. */
static void scroll_arc_update(lv_event_t *e)
{
    lv_obj_t *page = lv_event_get_current_target_obj(e);
    lv_obj_t *arc = lv_event_get_user_data(e);
    int32_t top = lv_obj_get_scroll_top(page);
    int32_t bottom = lv_obj_get_scroll_bottom(page);
    int32_t total = top + bottom;

    top = top < 0 ? 0 : top;
    bottom = bottom < 0 ? 0 : bottom;
    total = top + bottom;

    lv_obj_set_hidden(arc, total <= 0);

    if (total <= 0)
    {
        return;
    }

    int32_t height = lv_obj_get_height(page);
    int32_t thumb = ROUND_ARC_SPAN * height / (height + total);
    thumb = thumb < ROUND_ARC_MIN ? ROUND_ARC_MIN : thumb;

    int32_t start = -ROUND_ARC_SPAN / 2 + (ROUND_ARC_SPAN - thumb) * top / total;
    lv_arc_set_angles(arc, (uint32_t)((start + 360) % 360), (uint32_t)((start + thumb + 360) % 360));
}

/*
 * The arc is a sibling drawn over the page, not a child: apps may empty
 * their page (lv_obj_clean) and the arc must outlive that.
 */
static void add_scroll_arc(lv_obj_t *page)
{
    lv_obj_t *arc = lv_arc_create(lv_obj_get_parent(page));
    int32_t size = ui_board_info()->width - 8;

    lv_obj_set_floating(arc, true);   /* outside any flex layout of the parent */
    lv_obj_set_size(arc, size, size);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(arc, false);
    lv_arc_set_bg_angles(arc, 360 - ROUND_ARC_SPAN / 2, ROUND_ARC_SPAN / 2);
    lv_obj_set_style_arc_width(arc, TK_LW_RING, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, TK_LW_RING, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(TK_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, ui_accent(), LV_PART_INDICATOR);
    lv_obj_set_hidden(arc, true);

    lv_obj_add_event_cb(page, scroll_arc_update, LV_EVENT_SCROLL, arc);
    lv_obj_add_event_cb(page, scroll_arc_update, LV_EVENT_SIZE_CHANGED, arc);
    lv_obj_add_event_cb(page, scroll_arc_update, LV_EVENT_LAYOUT_CHANGED, arc);
}

lv_obj_t *ui_page(lv_obj_t *screen, const char *title)
{
    const board_info_t *board = ui_board_info();

    lv_obj_t *page = lv_obj_create(screen);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(page, board->round ? ROUND_PAD_X : PAGE_PAD_X, 0);
    lv_obj_set_style_pad_top(page, board->round ? ROUND_PAD_TOP : 18, 0);
    lv_obj_set_style_pad_bottom(page, board->round ? ROUND_PAD_BOTTOM : 48, 0);
    lv_obj_set_style_pad_row(page, TK_SP_2, 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_momentum(page, true);
    lv_obj_set_scroll_elastic(page, true);

    /* Children inherit it: also labels made without an explicit font get the accents. */
    lv_obj_set_style_text_font(page, UI_FONT_BODY, 0);

    if (title != NULL)
    {
        ui_title(page, title);
    }

    if (board->round)
    {
        add_scroll_arc(page);
    }

    return page;
}

lv_obj_t *ui_title(lv_obj_t *page, const char *text)
{
    lv_obj_t *label = lv_label_create(page);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, ui_font_title, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(TK_TEXT), 0);
    lv_obj_set_style_pad_bottom(label, TK_SP_1, 0);

    if (ui_board_info()->round)
    {
        /* The top of a round panel is narrow: centred, and "..." rather than cut by the glass. */
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    }
    else
    {
        lv_obj_set_style_pad_left(label, 6, 0);
    }

    return label;
}

/*
 * "// SECTION": tracked uppercase in the accent, the cheapest cyberpunk cue.
 * The HUD font is ASCII only, so captions with accents ("Attività") keep
 * the text font and their own case.
 */
lv_obj_t *ui_section(lv_obj_t *page, const char *text)
{
    lv_obj_t *label = lv_label_create(page);
    char hud[48];
    bool ascii = strlen(text) < sizeof(hud) - 4;

    for (const char *c = text; ascii && *c != '\0'; c++)
    {
        ascii = (unsigned char)*c < 0x80;
    }

    if (ascii)
    {
        size_t n = 0;
        hud[n++] = '/';
        hud[n++] = '/';
        hud[n++] = ' ';

        for (const char *c = text; *c != '\0'; c++)
        {
            hud[n++] = (*c >= 'a' && *c <= 'z') ? (char)(*c - 'a' + 'A') : *c;
        }

        hud[n] = '\0';
        lv_label_set_text(label, hud);
        lv_obj_set_style_text_font(label, UI_FONT_HUD, 0);
        lv_obj_set_style_text_letter_space(label, UI_HUD_LETTER_SPACE, 0);
    }
    else
    {
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
    }

    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, ui_accent(), 0);
    lv_obj_set_style_pad_top(label, TK_SP_3, 0);

    if (ui_board_info()->round)
    {
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);   /* near the top the edge is close */
    }
    else
    {
        lv_obj_set_style_pad_left(label, TK_SP_1, 0);
    }

    return label;
}

lv_obj_t *ui_text(lv_obj_t *page, const char *text, bool dim)
{
    lv_obj_t *label = lv_label_create(page);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_pad_hor(label, TK_SP_2, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(dim ? TK_TEXT_2 : TK_TEXT), 0);
    lv_obj_set_style_text_font(label, dim ? UI_FONT_SMALL : UI_FONT_BODY, 0);

    if (ui_board_info()->round)
    {
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);   /* reads better inside a circle */
    }

    return label;
}

/* --------------------------------------------------------------- rows */

/*
 * HUD chip: a faint neon tint (G3) inside a thin neon ring (G1), glyph in
 * the full neon. Few lit pixels, one colour family, static: drawn once.
 */
void ui_icon_bubble_set_color(lv_obj_t *bubble, uint32_t color)
{
    if (bubble == NULL)
    {
        return;
    }

    lv_obj_set_style_bg_color(bubble, ui_glow(color, TK_GLOW_G3), 0);
    lv_obj_set_style_border_color(bubble, ui_glow(color, TK_GLOW_G1), 0);
    lv_obj_set_style_outline_color(bubble, ui_glow(color, TK_GLOW_G1), 0);

    lv_obj_t *glyph = lv_obj_get_child(bubble, 0);

    if (glyph != NULL && lv_obj_check_type(glyph, &lv_label_class))
    {
        lv_obj_set_style_text_color(glyph, lv_color_hex(color), 0);
    }
}

lv_obj_t *ui_icon_bubble(lv_obj_t *parent, const char *icon, uint32_t color, int32_t size)
{
    lv_obj_t *bubble = lv_obj_create(parent);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_size(bubble, size, size);
    lv_obj_set_style_radius(bubble, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bubble, ui_glow(color, TK_GLOW_G3), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, TK_LW, 0);
    lv_obj_set_style_border_color(bubble, ui_glow(color, TK_GLOW_G1), 0);
    lv_obj_set_clickable(bubble, false);
    lv_obj_set_scrollable(bubble, false);

    if (icon == NULL)
    {
        return bubble;
    }

    bool file_icon = strncmp(icon, "S:", 2) == 0 || strncmp(icon, "F:", 2) == 0;
    const void *image_src = file_icon ? ui_image_src(icon) : NULL;

    if (image_src != NULL)
    {
        /* Icon image: scaled to fill the bubble, clipped to the circle. */
        lv_obj_t *image = lv_image_create(bubble);
        lv_image_set_src(image, image_src);
        lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_set_size(image, size, size);
        lv_obj_center(image);
        lv_obj_set_style_clip_corner(bubble, true, 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_TRANSP, 0);

        /* The image covers the border: the ring goes just outside instead. */
        lv_obj_set_style_border_width(bubble, 0, 0);
        lv_obj_set_style_outline_width(bubble, TK_LW, 0);
        lv_obj_set_style_outline_pad(bubble, 0, 0);
        lv_obj_set_style_outline_color(bubble, ui_glow(color, TK_GLOW_G1), 0);
    }
    else if (file_icon)
    {
        lv_obj_t *label = lv_label_create(bubble);   /* unreadable image: generic symbol */
        lv_label_set_text(label, LV_SYMBOL_DIRECTORY);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_center(label);
    }
    else
    {
        lv_obj_t *label = lv_label_create(bubble);
        lv_label_set_text(label, icon);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_set_style_text_font(label, size >= 64 ? &lv_font_montserrat_32 : (size >= 48 ? UI_FONT_LARGE : UI_FONT_BODY), 0);
        lv_obj_center(label);
    }

    return bubble;
}

/* Width of the longest word of `text`: a row label is never narrower, so it wraps between words. */
static int32_t longest_word_width(const char *text, const lv_font_t *font)
{
    int32_t longest = 0;
    char word[48];

    while (*text != '\0')
    {
        size_t n = strcspn(text, " ");

        if (n > 0)
        {
            size_t copy = n < sizeof(word) - 1 ? n : sizeof(word) - 1;
            lv_point_t size;

            memcpy(word, text, copy);
            word[copy] = '\0';
            lv_text_get_size(&size, word, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            longest = size.x > longest ? size.x : longest;
        }

        text += n;
        text += *text == ' ' ? 1 : 0;
    }

    return longest;
}

static lv_obj_t *row_base(lv_obj_t *page, const char *icon, uint32_t color, const char *text)
{
    lv_obj_t *row = lv_obj_create(page);
    ui_make_card(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, ROW_HEIGHT, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, TK_SP_3, 0);

    if (icon != NULL)
    {
        ui_icon_bubble(row, icon, color, BUBBLE_SIZE);
    }

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_flex_grow(label, 1);

    /* Narrow rows (round 360 px panel): wrap between words, never inside one ("Spegniment-o"). */
    lv_obj_set_style_min_width(label, longest_word_width(text, lv_obj_get_style_text_font(label, LV_PART_MAIN)), 0);

    return row;
}

lv_obj_t *ui_row(lv_obj_t *page, const char *icon, uint32_t color, const char *text,
                 const char *value, lv_event_cb_t on_click, void *user_data)
{
    lv_obj_t *row = row_base(page, icon, color, text);

    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text(value_label, value != NULL ? value : "");
    lv_obj_set_style_text_color(value_label, lv_color_hex(TK_TEXT_2), 0);
    lv_obj_set_style_text_font(value_label, UI_FONT_SMALL, 0);

    /* One line, cut with "..." when the row is narrow: the name on the left matters more. */
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(value_label, LV_PCT(45), 0);
    lv_obj_set_height(value_label, lv_font_get_line_height(UI_FONT_SMALL));
    lv_obj_set_user_data(row, value_label);

    if (on_click != NULL)
    {
        lv_obj_set_clickable(row, true);
        lv_port_set_focusable(row);
        lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, user_data);

        lv_obj_t *chevron = lv_label_create(row);
        lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chevron, lv_color_hex(TK_TEXT_3), 0);
        lv_obj_set_style_text_font(chevron, UI_FONT_SMALL, 0);
    }
    else
    {
        lv_obj_set_clickable(row, false);
    }

    return row;
}

lv_obj_t *ui_row_value(lv_obj_t *row)
{
    return (lv_obj_t *)lv_obj_get_user_data(row);
}

lv_obj_t *ui_switch_row(lv_obj_t *page, const char *icon, uint32_t color, const char *text,
                        bool on, lv_event_cb_t on_change, void *user_data)
{
    lv_obj_t *row = row_base(page, icon, color, text);
    lv_obj_set_clickable(row, false);

    /* HUD switch: squared track with a hairline frame; ON = glow tint + neon knob. */
    lv_obj_t *sw = lv_switch_create(row);
    lv_color_t accent = ui_accent();
    lv_obj_set_size(sw, 60, 32);
    lv_obj_set_ext_click_area(sw, (TK_TOUCH - 32) / 2);
    lv_obj_set_style_radius(sw, TK_R_XS, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, TK_R_XS, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sw, TK_R_XS, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(TK_SURFACE_2), LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, TK_LW_HAIR, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, lv_color_hex(TK_LINE_HI), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_mix(accent, lv_color_black(), TK_GLOW_G1),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(TK_TEXT_2), LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, accent, LV_PART_KNOB | LV_STATE_CHECKED);

    if (on)
    {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }

    if (on_change != NULL)
    {
        lv_obj_add_event_cb(sw, on_change, LV_EVENT_VALUE_CHANGED, user_data);
    }

    return row;
}

lv_obj_t *ui_slider_row(lv_obj_t *page, const char *icon, const char *text,
                        int32_t min, int32_t max, int32_t value, lv_event_cb_t on_change, void *user_data)
{
    lv_obj_t *card = lv_obj_create(page);
    ui_make_card(card);
    lv_obj_set_clickable(card, false);
    lv_obj_remove_style(card, NULL, LV_STATE_PRESSED);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, TK_SP_3, 0);
    lv_obj_set_style_pad_ver(card, TK_SP_4, 0);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text_fmt(label, "%s  %s", icon != NULL ? icon : "", text);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_width(slider, LV_PCT(94));
    /* HUD slider: thin line track, neon fill, ring knob (black centre, neon frame). */
    lv_obj_set_height(slider, TK_SP_2);
    lv_obj_set_style_align(slider, LV_ALIGN_CENTER, 0);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_radius(slider, TK_R_XS, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, TK_R_XS, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(TK_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, ui_accent(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(TK_VOID), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, TK_LW + 1, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, ui_accent(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, TK_SP_2, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, (TK_TOUCH - TK_SP_2) / 2);

    if (on_change != NULL)
    {
        lv_obj_add_event_cb(slider, on_change, LV_EVENT_VALUE_CHANGED, user_data);
    }

    return slider;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t on_click, void *user_data)
{
    lv_obj_t *button = lv_obj_create(parent);
    ui_make_card(button);
    lv_obj_set_clickable(button, true);
    lv_port_set_focusable(button);
    /*
     * HUD button: neon frame and text over a faint tint of the same colour.
     * Dark colours (old "neutral" grey buttons) become a hairline frame with
     * plain text, so their label stays readable.
     */
    bool neutral = lv_color_brightness(lv_color_hex(color)) < 96;
    uint32_t ink = neutral ? TK_TEXT : color;
    lv_obj_set_style_bg_color(button, neutral ? lv_color_hex(TK_SURFACE_1) : ui_glow(color, TK_GLOW_G3), 0);
    lv_obj_set_style_bg_color(button, neutral ? lv_color_hex(TK_SURFACE_2) : ui_glow(color, TK_GLOW_G1),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, neutral ? TK_LW_HAIR : TK_LW, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(neutral ? TK_LINE_HI : color), 0);
    lv_obj_set_style_border_color(button, lv_color_hex(neutral ? TK_LINE_HI : color), LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_height(button, TK_ROW_H);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UI_FONT_LARGE, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(ink), 0);
    lv_obj_center(label);

    if (on_click != NULL)
    {
        lv_obj_add_event_cb(button, on_click, LV_EVENT_CLICKED, user_data);
    }

    return button;
}
