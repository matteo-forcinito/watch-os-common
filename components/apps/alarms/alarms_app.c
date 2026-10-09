#include "apps_internal.h"

#include "core/clock.h"
#include "core/lv_port.h"
#include "core/state.h"
#include "services/alarm.h"
#include "ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================== alarm list */

static lv_obj_t *s_list = NULL;

static void on_toggle(lv_event_t *e)
{
    uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    alarm_set_enabled(id, lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}

static void on_edit(lv_event_t *e)
{
    app_open_app(&alarm_edit_app, lv_event_get_user_data(e));
}

static void rebuild(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    lv_obj_clean(s_list);

    ui_title(s_list, "Sveglie");

    alarm_t *alarms = calloc(ALARM_MAX, sizeof(alarm_t));
    int count = alarms != NULL ? alarm_list(alarms, ALARM_MAX) : 0;

    for (int i = 0; i < count; i++)
    {
        const alarm_t *a = &alarms[i];
        struct tm t = {.tm_hour = a->hour, .tm_min = a->minute};
        char time_text[12];
        char days[40];
        clock_format_hm(&t, time_text, sizeof(time_text));
        alarm_days_text(a->days, days, sizeof(days));

        lv_obj_t *card = lv_obj_create(s_list);
        ui_make_card(card);
        lv_obj_set_clickable(card, true);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(card, 14, 0);
        lv_port_set_focusable(card);
        lv_obj_add_event_cb(card, on_edit, LV_EVENT_CLICKED, (void *)(uintptr_t)a->id);

        lv_obj_t *texts = lv_obj_create(card);
        lv_obj_remove_style_all(texts);
        lv_obj_set_flex_grow(texts, 1);
        lv_obj_set_height(texts, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_clickable(texts, false);

        lv_obj_t *time_label = lv_label_create(texts);
        lv_label_set_text(time_label, time_text);
        lv_obj_set_style_text_font(time_label, ui_font_big, 0);
        lv_obj_set_style_text_color(time_label, lv_color_hex(a->enabled ? UI_COLOR_TEXT : UI_COLOR_GRAY), 0);

        lv_obj_t *info = lv_label_create(texts);
        lv_label_set_text_fmt(info, "%s · %s", a->label[0] != '\0' ? a->label : "Sveglia", days);
        lv_obj_set_style_text_font(info, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(info, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

        lv_obj_t *sw = lv_switch_create(card);
        lv_obj_set_size(sw, 64, 36);
        lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_ORANGE), LV_PART_INDICATOR | LV_STATE_CHECKED);

        if (a->enabled)
        {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }

        lv_obj_add_event_cb(sw, on_toggle, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)a->id);
    }

    free(alarms);

    if (count == 0)
    {
        ui_text(s_list, "Nessuna sveglia. Aggiungine una qui o dalla pagina web.", true);
    }

    if (count < ALARM_MAX)
    {
        ui_button(s_list, LV_SYMBOL_PLUS "  Nuova sveglia", UI_COLOR_ORANGE, on_edit, (void *)0);
    }
}

static void list_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    s_list = ui_page(screen, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_ALARM_VERSION), rebuild, s_list, NULL);
}

const app_t alarms_app = {
    .id = "alarms",
    .name = "Sveglie",
    .icon = LV_SYMBOL_BELL,
    .color = UI_COLOR_ORANGE,
    .create = list_create,
};

/* ====================================================== alarm editor */

typedef struct
{
    alarm_t alarm;
    lv_obj_t *hour;
    lv_obj_t *minute;
    lv_obj_t *days[7];
    lv_obj_t *label_row;
} editor_t;

static editor_t *s_ed = NULL;

static void build_numbers(char *out, size_t size, int count)
{
    size_t used = 0;

    for (int i = 0; i < count && used + 4 < size; i++)
    {
        used += (size_t)snprintf(out + used, size - used, i == 0 ? "%02d" : "\n%02d", i);
    }
}

static void label_entered(const char *text, void *user_data)
{
    (void)user_data;

    if (text != NULL && s_ed != NULL)
    {
        snprintf(s_ed->alarm.label, sizeof(s_ed->alarm.label), "%s", text);
        lv_label_set_text(ui_row_value(s_ed->label_row), s_ed->alarm.label);
    }
}

static void on_label(lv_event_t *e)
{
    (void)e;
    ui_text_input("Nome sveglia", s_ed->alarm.label, false, label_entered, NULL);
}

static void on_day(lv_event_t *e)
{
    int day = (int)(intptr_t)lv_event_get_user_data(e);
    s_ed->alarm.days ^= (uint8_t)(1 << day);
    lv_obj_set_style_bg_color(s_ed->days[day],
                              lv_color_hex((s_ed->alarm.days & (1 << day)) ? UI_COLOR_ORANGE : UI_COLOR_CARD_HI), 0);
}

static void on_save(lv_event_t *e)
{
    (void)e;

    s_ed->alarm.hour = (uint8_t)lv_roller_get_selected(s_ed->hour);
    s_ed->alarm.minute = (uint8_t)lv_roller_get_selected(s_ed->minute);
    s_ed->alarm.enabled = true;

    if (alarm_save(&s_ed->alarm) == ESP_OK)
    {
        ui_toast("Sveglia salvata");
        app_back();
    }
    else
    {
        ui_toast("Troppe sveglie");
    }
}

static void delete_confirmed(bool yes, void *user_data)
{
    (void)user_data;

    if (yes && s_ed != NULL)
    {
        alarm_delete(s_ed->alarm.id);
        app_back();
    }
}

static void on_delete(lv_event_t *e)
{
    (void)e;
    ui_confirm("Eliminare la sveglia?", NULL, "Elimina", UI_COLOR_RED, delete_confirmed, NULL);
}

static lv_obj_t *roller(lv_obj_t *parent, const char *options, int selected)
{
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, options, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(r, 3);
    lv_roller_set_selected(r, (uint32_t)selected, LV_ANIM_OFF);
    lv_obj_set_width(r, 120);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_40, LV_PART_SELECTED);
    lv_obj_set_style_text_font(r, UI_FONT_LARGE, 0);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(UI_COLOR_CARD), LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_hex(UI_COLOR_ORANGE), LV_PART_SELECTED);
    return r;
}

static void edit_create(lv_obj_t *screen, void *arg)
{
    s_ed = calloc(1, sizeof(editor_t));

    if (s_ed == NULL)
    {
        return;
    }

    uint32_t id = (uint32_t)(uintptr_t)arg;

    if (id == 0 || !alarm_get(id, &s_ed->alarm))
    {
        s_ed->alarm = (alarm_t){.hour = 7, .minute = 30, .days = ALARM_DAYS_WEEKDAYS, .enabled = true, .radio = -1};
        snprintf(s_ed->alarm.label, sizeof(s_ed->alarm.label), "Sveglia");
    }

    lv_obj_t *page = ui_page(screen, id == 0 ? "Nuova sveglia" : "Modifica");

    lv_obj_t *wheels = lv_obj_create(page);
    lv_obj_remove_style_all(wheels);
    lv_obj_set_size(wheels, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wheels, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wheels, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wheels, 8, 0);

    static char hours[24 * 3 + 1];
    static char minutes[60 * 3 + 1];
    build_numbers(hours, sizeof(hours), 24);
    build_numbers(minutes, sizeof(minutes), 60);

    s_ed->hour = roller(wheels, hours, s_ed->alarm.hour);
    lv_obj_t *colon = lv_label_create(wheels);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_40, 0);
    s_ed->minute = roller(wheels, minutes, s_ed->alarm.minute);

    /* days */
    lv_obj_t *days = lv_obj_create(page);
    lv_obj_remove_style_all(days);
    lv_obj_set_size(days, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(days, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(days, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *const letters[] = {"L", "M", "M", "G", "V", "S", "D"};

    for (int d = 0; d < 7; d++)
    {
        lv_obj_t *chip = lv_obj_create(days);
        ui_make_card(chip);
        lv_obj_set_clickable(chip, true);
        lv_obj_set_size(chip, 44, 44);
        lv_obj_set_style_pad_all(chip, 0, 0);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(chip, lv_color_hex((s_ed->alarm.days & (1 << d)) ? UI_COLOR_ORANGE : UI_COLOR_CARD_HI), 0);
        lv_port_set_focusable(chip);
        lv_obj_add_event_cb(chip, on_day, LV_EVENT_CLICKED, (void *)(intptr_t)d);
        lv_obj_t *letter = lv_label_create(chip);
        lv_label_set_text(letter, letters[d]);
        lv_obj_center(letter);
        s_ed->days[d] = chip;
    }

    ui_text(page, "Nessun giorno = suona una volta sola.", true);

    s_ed->label_row = ui_row(page, LV_SYMBOL_EDIT, UI_COLOR_GRAY, "Nome", s_ed->alarm.label, on_label, NULL);

    ui_button(page, "Salva", UI_COLOR_ORANGE, on_save, NULL);

    if (id != 0)
    {
        ui_button(page, "Elimina", UI_COLOR_CARD_HI, on_delete, NULL);
    }
}

static void edit_destroy(void)
{
    free(s_ed);
    s_ed = NULL;
}

const app_t alarm_edit_app = {
    .id = "alarms.edit",
    .name = "Sveglia",
    .icon = LV_SYMBOL_BELL,
    .color = UI_COLOR_ORANGE,
    .flags = APP_FLAG_HIDDEN,
    .create = edit_create,
    .destroy = edit_destroy,
};
