#include "apps_internal.h"

#include "core/lv_port.h"
#include "core/sys.h"
#include "services/web_server.h"
#include "services/wifi.h"
#include "ui/ui.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

/*
 * Setup web page. While this app is open the watch serves the page and
 * shows its address as text and QR code (scan it with the phone camera).
 * Closing the app stops the server and lets Wi-Fi switch off.
 */

static lv_obj_t *s_page = NULL;
static lv_obj_t *s_wifi = NULL;       /* live Wi-Fi line: connected / why not */
static lv_timer_t *s_timer = NULL;
static volatile int s_generation = 0;

static void wifi_tick(lv_timer_t *timer)
{
    (void)timer;

    if (s_wifi == NULL)
    {
        return;
    }

    char ssid[33];
    char ip[16];

    if (wifi_is_connected())
    {
        wifi_current_ssid(ssid, sizeof(ssid));
        wifi_get_ip(ip, sizeof(ip));
        lv_label_set_text_fmt(s_wifi, "Wi-Fi: %s (%s)", ssid, ip);
        lv_obj_set_style_text_color(s_wifi, lv_color_hex(UI_COLOR_GREEN), 0);
    }
    else if (wifi_last_reason() != 0)
    {
        lv_label_set_text_fmt(s_wifi, "Wi-Fi: %s", wifi_reason_text(wifi_last_reason()));
        lv_obj_set_style_text_color(s_wifi, lv_color_hex(UI_COLOR_ORANGE), 0);
    }
    else
    {
        lv_label_set_text(s_wifi, "");
    }
}

static void show_ready(void *arg)
{
    if ((int)(intptr_t)arg != s_generation || s_page == NULL)
    {
        return;
    }

    lv_obj_clean(s_page);

    ui_title(s_page, "Pagina web");

    if (!web_server_running())
    {
        ui_text(s_page, web_server_last_error(), true);
        return;
    }

    char url[48];
    char ap[33];
    web_server_url(url, sizeof(url));

    if (web_server_ap_name(ap, sizeof(ap)))
    {
        char text[96];
        snprintf(text, sizeof(text), "1. Collegati alla rete Wi-Fi\n\"%s\"", ap);
        ui_text(s_page, text, false);
    }

    lv_obj_t *frame = lv_obj_create(s_page);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, 220, 220);
    lv_obj_set_style_bg_color(frame, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(frame, 20, 0);

    lv_obj_t *qr = lv_qrcode_create(frame);
    lv_qrcode_set_size(qr, 190);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    char target[72];
    snprintf(target, sizeof(target), "%s#code=%s", url, web_server_code());   /* the QR pairs the phone too */
    lv_qrcode_update(qr, target, (uint32_t)strlen(target));
    lv_obj_center(qr);

    lv_obj_t *link = lv_label_create(s_page);
    lv_label_set_text(link, url);
    lv_obj_set_style_text_font(link, UI_FONT_LARGE, 0);
    lv_obj_set_style_text_color(link, ui_accent(), 0);

    lv_obj_t *code = lv_label_create(s_page);
    lv_label_set_text_fmt(code, "Codice  %.3s %.3s", web_server_code(), web_server_code() + 3);
    lv_obj_set_style_text_font(code, ui_font_title, 0);

    char hint[96];
    snprintf(hint, sizeof(hint), "oppure http://%s.local/  ·  chiudi l'app per spegnere il Wi-Fi", sys_hostname());
    ui_text(s_page, hint, true);

    s_wifi = ui_text(s_page, "", false);
    lv_obj_set_style_text_align(s_wifi, LV_TEXT_ALIGN_CENTER, 0);
    wifi_tick(NULL);
}

/* The server is shared (internal apps' dashboards use it too): every successful
 * start is paired with one stop, done here if the app closed while starting. */
static SemaphoreHandle_t s_hold_lock;
static bool s_holds_server;
static bool s_open;

static void start_task(void *arg)
{
    bool ok = web_server_start() == ESP_OK;
    xSemaphoreTake(s_hold_lock, portMAX_DELAY);
    bool keep = ok && s_open && (int)(intptr_t)arg == s_generation;
    s_holds_server = s_holds_server || keep;
    xSemaphoreGive(s_hold_lock);
    if (ok && !keep)
    {
        web_server_stop();
    }
    ui_async(show_ready, arg);
    vTaskDelete(NULL);
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    s_page = ui_page(screen, "Pagina web");
    lv_obj_set_flex_align(s_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_text(s_page, "Avvio Wi-Fi...", true);

    if (s_hold_lock == NULL)
    {
        s_hold_lock = xSemaphoreCreateMutex();
    }
    s_open = true;
    if (!sys_task_create(start_task, "web_start", 4096, (void *)(intptr_t)s_generation, 3, SYS_CORE_ANY))
    {
        ui_text(s_page, "Memoria insufficiente per avviare il server.", true);
    }

    s_timer = lv_timer_create(wifi_tick, 1000, NULL);
}

/* Closing must never need memory: the stop runs on the sys worker queue
 * (a new task could fail exactly when RAM is short and leave Wi-Fi on). */
static void stop_job(void *arg)
{
    (void)arg;
    web_server_stop();
}

static void destroy(void)
{
    s_generation++;
    s_page = NULL;
    s_wifi = NULL;

    if (s_timer != NULL)
    {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    /* the mutex is only held for a few instructions by start_task */
    xSemaphoreTake(s_hold_lock, portMAX_DELAY);
    s_open = false;
    bool held = s_holds_server;
    s_holds_server = false;
    xSemaphoreGive(s_hold_lock);
    if (held)
    {
        while (!sys_post(stop_job, NULL))
        {
            vTaskDelay(pdMS_TO_TICKS(20));   /* queue full: it drains in a moment */
        }
    }
}

const app_t web_app = {
    .id = "web",
    .name = "Pagina web",
    .icon = LV_SYMBOL_UPLOAD,
    .color = UI_COLOR_GREEN,
    .flags = APP_FLAG_KEEP_SCREEN_ON,
    .create = create,
    .destroy = destroy,
};
