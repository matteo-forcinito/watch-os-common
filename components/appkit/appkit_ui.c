/*
 * Watch page that leads to the dashboard:
 *
 *        Dashboard
 *      ┌──────────┐
 *      │ QR code  │     the QR opens the page already paired (#code=...)
 *      └──────────┘
 *   http://192.168.1.20/
 *      pieno.local
 *     Codice 482 913
 */
#include "appkit.h"
#include "services/wifi.h"
#include "appkit_private.h"

#include "hardware/board.h"
#include "ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    lv_obj_t *state;
    lv_obj_t *frame;
    lv_obj_t *qr;
    lv_obj_t *url;
    lv_obj_t *code;
    lv_obj_t *info;
    lv_obj_t *usage;
    lv_timer_t *timer;
    char shown[64];
    unsigned hits;
    appkit_web_state_t last;
} page_t;

static void refresh(lv_timer_t *timer)
{
    page_t *p = lv_timer_get_user_data(timer);
    appkit_web_state_t state = appkit_web_state();
    if (state == APPKIT_WEB_FAILED)
    {
        appkit_web_activate();   /* internal apps: try again (external apps retry by themselves) */
    }
    char url[48];
    appkit_web_url(url, sizeof(url));

    if (state != p->last)
    {
        p->last = state;
        static const char *const text[] = {
            [APPKIT_WEB_OFF] = "Dashboard spenta",
            [APPKIT_WEB_JOINING] = "Connessione al Wi-Fi...",
            [APPKIT_WEB_ON] = "Inquadra con il telefono o apri dal PC",
            [APPKIT_WEB_FAILED] = "Wi-Fi non raggiungibile: riprovo tra poco",
            [APPKIT_WEB_NO_MEMORY] = "Memoria insufficiente: chiudi altre app o spegni il Bluetooth",
        };
        lv_label_set_text(p->state, text[state]);
        lv_obj_set_hidden(p->frame, state != APPKIT_WEB_ON);
        lv_obj_set_hidden(p->url, state != APPKIT_WEB_ON);
        lv_obj_set_hidden(p->code, state != APPKIT_WEB_ON);
        lv_obj_set_hidden(p->info, state != APPKIT_WEB_ON);
    }

    char target[64];
    snprintf(target, sizeof(target), "%s#code=%s", url, appkit_web_code());
    if (state == APPKIT_WEB_ON && strcmp(target, p->shown) != 0)
    {
        snprintf(p->shown, sizeof(p->shown), "%s", target);
        lv_qrcode_update(p->qr, target, (uint32_t)strlen(target));
        lv_label_set_text(p->url, url);
        const char *code = appkit_web_code();
        lv_label_set_text_fmt(p->code, "Codice  %.3s %.3s", code, code + 3);
        /* Everything needed to reach it without the QR: network, address, name. */
        char ssid[33] = "", ip[20] = "", host[64];
        wifi_current_ssid(ssid, sizeof(ssid));
        wifi_get_ip(ip, sizeof(ip));
        appkit_web_host(host, sizeof(host));
        lv_label_set_text_fmt(p->info, "Wi-Fi  %s\nIP  %s\noppure  %s", ssid[0] ? ssid : "–", ip[0] ? ip : "–", host);
    }

    unsigned hits = appkit_web_hits();
    if (hits != p->hits)
    {
        p->hits = hits;
        lv_label_set_text(p->usage, LV_SYMBOL_OK "  Dashboard collegata");
        lv_obj_set_style_text_color(p->usage, lv_color_hex(0x35D07F), 0);
    }
}

static void on_delete(lv_event_t *event)
{
    page_t *p = lv_event_get_user_data(event);
    lv_timer_delete(p->timer);
    free(p);
}

void appkit_dashboard_page(lv_obj_t *parent, lv_color_t accent)
{
    page_t *p = calloc(1, sizeof(*p));
    if (p == NULL)
    {
        return;
    }
    p->last = (appkit_web_state_t)-1;
    appkit_web_activate();
    const board_info_t *board = board_info();
    int32_t side = (board->width < board->height ? board->width : board->height) * 46 / 100;

    p->state = ui_text(parent, "", true);
    lv_obj_set_style_text_align(p->state, LV_TEXT_ALIGN_CENTER, 0);

    p->frame = lv_obj_create(parent);
    lv_obj_remove_style_all(p->frame);
    lv_obj_set_size(p->frame, side + 20, side + 20);
    lv_obj_set_style_bg_color(p->frame, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(p->frame, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p->frame, 18, 0);
    lv_obj_set_style_align(p->frame, LV_ALIGN_CENTER, 0);
    lv_obj_set_scrollable(p->frame, false);

    p->qr = lv_qrcode_create(p->frame);
    lv_qrcode_set_size(p->qr, side);
    lv_qrcode_set_dark_color(p->qr, lv_color_black());
    lv_qrcode_set_light_color(p->qr, lv_color_white());
    lv_obj_center(p->qr);

    p->url = lv_label_create(parent);
    lv_label_set_text(p->url, "");
    lv_obj_set_style_text_font(p->url, UI_FONT_LARGE, 0);
    lv_obj_set_style_text_color(p->url, accent, 0);

    p->code = lv_label_create(parent);
    lv_label_set_text(p->code, "");
    lv_obj_set_style_text_font(p->code, ui_font_title, 0);
    lv_obj_set_style_text_color(p->code, lv_color_hex(0xF4F6FA), 0);

    p->info = ui_text(parent, "", false);
    lv_obj_set_style_text_align(p->info, LV_TEXT_ALIGN_CENTER, 0);

    p->usage = ui_text(parent, "Il codice abbina un nuovo browser; quelli abbinati restano validi.", true);
    lv_obj_set_style_text_align(p->usage, LV_TEXT_ALIGN_CENTER, 0);

    p->timer = lv_timer_create(refresh, 1000, p);
    lv_obj_add_event_cb(parent, on_delete, LV_EVENT_DELETE, p);
    refresh(p->timer);
}
