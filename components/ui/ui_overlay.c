#include "ui/ui.h"

#include "core/app.h"

#include <stdlib.h>
#include <string.h>

/* =============================================================== toast */

static lv_obj_t *s_toast = NULL;

static void toast_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s_toast)
    {
        s_toast = NULL;
    }
}

static void anim_set_y(void *obj, int32_t value)
{
    lv_obj_set_y(obj, value);
}

static void anim_delete_ready(lv_anim_t *anim)
{
    lv_obj_delete((lv_obj_t *)anim->var);
}

void ui_toast(const char *text)
{
    if (s_toast != NULL)
    {
        lv_obj_delete(s_toast);
    }

    lv_obj_t *toast = lv_obj_create(lv_layer_top());
    s_toast = toast;
    lv_obj_remove_style_all(toast);
    lv_obj_set_style_bg_color(toast, lv_color_hex(UI_COLOR_CARD_HI), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(toast, 22, 0);
    lv_obj_set_style_pad_ver(toast, 12, 0);
    lv_obj_set_style_shadow_width(toast, 24, 0);
    lv_obj_set_style_shadow_opa(toast, LV_OPA_50, 0);
    lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(toast, LV_PCT(90), 0);
    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, -80);
    lv_obj_set_clickable(toast, false);
    lv_obj_add_event_cb(toast, toast_deleted, LV_EVENT_DELETE, NULL);

    lv_obj_t *label = lv_label_create(toast);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(label, 280, 0);

    /* Slide down, wait, slide up and delete itself. */
    lv_anim_t in;
    lv_anim_init(&in);
    lv_anim_set_var(&in, toast);
    lv_anim_set_exec_cb(&in, anim_set_y);
    lv_anim_set_values(&in, -80, 14);
    lv_anim_set_duration(&in, 260);
    lv_anim_set_path_cb(&in, lv_anim_path_ease_out);
    lv_anim_start(&in);

    lv_anim_t out;
    lv_anim_init(&out);
    lv_anim_set_var(&out, toast);
    lv_anim_set_exec_cb(&out, anim_set_y);
    lv_anim_set_values(&out, 14, -100);
    lv_anim_set_duration(&out, 260);
    lv_anim_set_delay(&out, 2200);
    lv_anim_set_path_cb(&out, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&out, anim_delete_ready);
    lv_anim_start(&out);
}

/* ============================================================= confirm */

typedef struct
{
    lv_obj_t *overlay;
    ui_confirm_cb_t cb;
    void *user_data;
} confirm_t;

static void confirm_answer(lv_event_t *e, bool yes)
{
    confirm_t *c = lv_event_get_user_data(e);
    ui_confirm_cb_t cb = c->cb;
    void *user_data = c->user_data;

    lv_obj_delete_async(c->overlay);   /* we are inside one of its buttons */
    free(c);

    if (cb != NULL)
    {
        cb(yes, user_data);
    }
}

static void confirm_yes(lv_event_t *e)
{
    confirm_answer(e, true);
}

static void confirm_no(lv_event_t *e)
{
    confirm_answer(e, false);
}

void ui_confirm(const char *title, const char *text, const char *ok_text, uint32_t ok_color,
                ui_confirm_cb_t cb, void *user_data)
{
    confirm_t *c = calloc(1, sizeof(confirm_t));

    if (c == NULL)
    {
        return;
    }

    c->cb = cb;
    c->user_data = user_data;

    /* Dark veil over everything: catches taps outside the card. */
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    c->overlay = overlay;
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
    lv_obj_set_clickable(overlay, true);

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_width(card, LV_PCT(88));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(card);

    lv_obj_t *title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, UI_FONT_LARGE, 0);

    if (text != NULL)
    {
        lv_obj_t *text_label = lv_label_create(card);
        lv_label_set_text(text_label, text);
        lv_label_set_long_mode(text_label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(text_label, LV_PCT(100));
        lv_obj_set_style_text_align(text_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(text_label, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    }

    ui_button(card, ok_text, ok_color, confirm_yes, c);
    lv_obj_t *cancel = ui_button(card, "Annulla", UI_COLOR_CARD_HI, confirm_no, c);
    lv_obj_set_height(cancel, 56);

    lv_obj_fade_in(overlay, 150, 0);
}

/* ========================================================== text input */

typedef struct
{
    char title[40];
    char initial[96];
    bool password;
    ui_text_cb_t cb;
    void *user_data;
} text_request_t;

static text_request_t s_request;
static lv_obj_t *s_textarea = NULL;

static void keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL)
    {
        return;
    }

    static char result[96];
    bool ok = code == LV_EVENT_READY;

    if (ok)
    {
        strncpy(result, lv_textarea_get_text(s_textarea), sizeof(result) - 1);
        result[sizeof(result) - 1] = '\0';
    }

    ui_text_cb_t cb = s_request.cb;
    void *user_data = s_request.user_data;

    app_back();

    if (cb != NULL)
    {
        cb(ok ? result : NULL, user_data);
    }
}

static void show_password_toggle(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    lv_textarea_set_password_mode(s_textarea, !lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void keyboard_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(screen, 14, 0);
    lv_obj_set_style_pad_row(screen, 8, 0);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, s_request.title);
    lv_obj_set_style_text_font(title, UI_FONT_LARGE, 0);

    s_textarea = lv_textarea_create(screen);
    lv_textarea_set_one_line(s_textarea, true);
    lv_textarea_set_text(s_textarea, s_request.initial);
    lv_textarea_set_password_mode(s_textarea, s_request.password);
    lv_obj_set_width(s_textarea, LV_PCT(92));
    lv_obj_set_style_radius(s_textarea, 16, 0);
    lv_obj_set_style_bg_color(s_textarea, lv_color_hex(UI_COLOR_CARD), 0);
    lv_obj_set_style_border_width(s_textarea, 0, 0);
    lv_obj_add_state(s_textarea, LV_STATE_FOCUSED);

    if (s_request.password)
    {
        lv_obj_t *row = lv_obj_create(screen);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(92), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, "Mostra password");
        lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);

        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 54, 30);
        lv_obj_add_event_cb(sw, show_password_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    }

    lv_obj_t *keyboard = lv_keyboard_create(screen);
    lv_obj_set_width(keyboard, LV_PCT(100));
    lv_obj_set_flex_grow(keyboard, 1);
    lv_obj_set_style_bg_color(keyboard, lv_color_black(), 0);
    lv_obj_set_style_text_font(keyboard, UI_FONT_BODY, LV_PART_ITEMS);
    lv_keyboard_set_textarea(keyboard, s_textarea);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_ALL, NULL);
}

static void keyboard_destroy(void)
{
    s_textarea = NULL;
}

static const app_t s_keyboard_app = {
    .id = "system.keyboard",
    .name = "Tastiera",
    .icon = LV_SYMBOL_KEYBOARD,
    .color = UI_COLOR_GRAY,
    .flags = APP_FLAG_HIDDEN | APP_FLAG_NO_BACK_GESTURE,
    .create = keyboard_create,
    .destroy = keyboard_destroy,
};

void ui_register_keyboard_app(void)
{
    app_register(&s_keyboard_app);
}

void ui_text_input(const char *title, const char *initial, bool password, ui_text_cb_t cb, void *user_data)
{
    memset(&s_request, 0, sizeof(s_request));
    strncpy(s_request.title, title != NULL ? title : "", sizeof(s_request.title) - 1);
    strncpy(s_request.initial, initial != NULL ? initial : "", sizeof(s_request.initial) - 1);
    s_request.password = password;
    s_request.cb = cb;
    s_request.user_data = user_data;

    app_open_app(&s_keyboard_app, NULL);
}
