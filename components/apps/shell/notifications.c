#include "apps_internal.h"

#include "core/clock.h"
#include "core/lv_port.h"
#include "core/state.h"
#include "services/notify.h"
#include "ui/ui.h"

#include <stdio.h>
#include <time.h>

/*
 * Notifications from the phone (swipe up from the watch face).
 * Tap a card to dismiss it. The list is rebuilt when it changes.
 */

static lv_obj_t *s_list = NULL;

static void dismiss(lv_event_t *e)
{
    notify_remove((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

static void clear_all(lv_event_t *e)
{
    (void)e;
    notify_clear();
}

static void rebuild(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    lv_obj_clean(s_list);

    ui_title(s_list, "Notifiche");

    int count = notify_count();

    if (count == 0)
    {
        lv_obj_t *empty = ui_text(s_list, "Nessuna notifica.\nCollega il telefono con Gadgetbridge per riceverle.", true);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, 60, 0);
        return;
    }

    for (int i = 0; i < count; i++)
    {
        notify_t n;

        if (!notify_get(i, &n))
        {
            break;
        }

        lv_obj_t *card = lv_obj_create(s_list);
        ui_make_card(card);
        lv_obj_set_clickable(card, true);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(card, 14, 0);
        lv_obj_set_style_pad_row(card, 4, 0);
        lv_port_set_focusable(card);
        lv_obj_add_event_cb(card, dismiss, LV_EVENT_CLICKED, (void *)(uintptr_t)n.id);

        struct tm t;
        char when[12];
        localtime_r(&n.time, &t);
        clock_format_hm(&t, when, sizeof(when));

        lv_obj_t *app = lv_label_create(card);
        lv_label_set_text_fmt(app, "%s  ·  %s", n.app[0] != '\0' ? n.app : "Telefono", when);
        lv_obj_set_style_text_font(app, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(app, ui_accent(), 0);

        if (n.title[0] != '\0')
        {
            lv_obj_t *title_label = lv_label_create(card);
            lv_label_set_text(title_label, n.title);
            lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(title_label, LV_PCT(100));
            lv_obj_set_style_text_font(title_label, UI_FONT_LARGE, 0);
        }

        lv_obj_t *body = lv_label_create(card);
        lv_label_set_text(body, n.body);
        lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(body, LV_PCT(100));
        lv_obj_set_style_text_color(body, lv_color_hex(0xD1D1D6), 0);
    }

    lv_obj_t *button = ui_button(s_list, "Cancella tutto", UI_COLOR_CARD_HI, clear_all, NULL);
    lv_obj_set_height(button, 56);
}

void notifications_create(lv_obj_t *parent)
{
    s_list = ui_page(parent, NULL);
    lv_subject_add_observer_obj(state_subject(STATE_NOTIF_VERSION), rebuild, s_list, NULL);
}
