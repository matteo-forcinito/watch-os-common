/*
 * Sentinel Lite on the watch.
 *
 *   HOME     like Copertura's LIVE: an arc = devices online (the progress while a
 *            job runs), the count, new ones, signal; round buttons
 *   MENU     check, nearby networks, Wi-Fi, dashboard
 *   DEVICES  who is connected (online first), new ones marked
 *   DEVICE   one device: name, kind, maker, IP, MAC, names heard; check its
 *            services; mark it as trusted
 *   CHECK    router / internet / DNS times, signal, security, a verdict
 *   AIR      nearby networks and the least busy channel
 *   FIND     hot and cold: an arc from blue to red with the device's signal
 *   WIFI     the known networks: tap one to move the watch there
 *   DASH     QR code + code for the web dashboard
 */
#include "lite.h"

#include "appkit.h"
#include "core/lv_port.h"
#include "hardware/board.h"
#include "services/wifi.h"
#include "ui/ui.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define C_BG      0x061A1B
#define C_CARD    0x0D2A2C
#define C_LINE    0x1C4A4B
#define C_INK     0xE3F4F5
#define C_DIM     0x92AEBA
#define C_ACCENT  0x49E2BC
#define C_WARN    0xFFC67B
#define C_BAD     0xFF6B6B
#define C_COLD    0x3C8CFF
#define C_HOT     0xFF5A3C
#define KNOWN     8

#define HISTORY 8

enum { HOME, DEVICES, DEVICE, CHECK, AIR, DASH, FIND, WIFI, MENU };

typedef struct
{
    lv_obj_t *screen;
    lv_obj_t *content;
    lv_obj_t *status;
    lv_obj_t *border;
    lv_timer_t *timer;
    int view;
    int history[HISTORY];
    int depth;
    uint32_t ip;                 /* DEVICE */
    sl_phase_t phase;
    uint32_t revision;
    lv_obj_t *find_arc, *find_value, *find_word, *find_info;   /* FIND, updated by tick() */
    lv_obj_t *home_arc, *home_net, *home_big, *home_sub, *home_info;   /* HOME, updated by tick() */
    char known[KNOWN][33];       /* WIFI */
} sl_ui_t;

static sl_ui_t *s_ui;
static void render(sl_ui_t *ui);

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_color(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static int card_width(void)
{
    const board_info_t *b = board_info();
    return b->round ? b->width * 74 / 100 : b->width - 24;
}

static void go(sl_ui_t *ui, int view)
{
    if (ui->depth < HISTORY)
    {
        ui->history[ui->depth++] = ui->view;
    }
    ui->view = view;
    render(ui);
}

static void open_view(lv_event_t *e) { go(s_ui, (int)(intptr_t)lv_event_get_user_data(e)); }
static void do_scan(lv_event_t *e) { (void)e; sl_scan(); ui_toast("Scansione della rete..."); }

static void do_check(lv_event_t *e)
{
    (void)e;
    sl_check();
    go(s_ui, CHECK);
}

static void do_air(lv_event_t *e)
{
    (void)e;
    sl_airwaves();
    go(s_ui, AIR);
}

static void open_device(lv_event_t *e)
{
    s_ui->ip = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    go(s_ui, DEVICE);
}

static void do_ports(lv_event_t *e)
{
    (void)e;
    sl_ports(s_ui->ip);
    ui_toast("Controllo i servizi...");
}

static void toggle_trust(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    sl_lock();
    int count;
    const sl_device_t *list = sl_devices(&count);
    uint8_t mac[6];
    bool has_mac = false;
    char label_text[32] = "";
    for (int i = 0; i < count; i++)
    {
        if (list[i].ip == s_ui->ip)
        {
            memcpy(mac, list[i].mac, 6);
            has_mac = list[i].has_mac;
            snprintf(label_text, sizeof(label_text), "%s", list[i].label);
        }
    }
    sl_unlock();
    sl_edit(has_mac ? mac : NULL, s_ui->ip, label_text, on);
}

/* A 270° arc, value 0..100, nothing clickable. */
static lv_obj_t *gauge(lv_obj_t *parent, int size, int width, uint32_t color, int value)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, ui_color(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, ui_color(color), LV_PART_INDICATOR);
    lv_arc_set_value(arc, value < 0 ? 0 : value > 100 ? 100 : value);
    lv_obj_set_clickable(arc, false);
    return arc;
}

/* A label centred inside `arc`. */
static lv_obj_t *inside(lv_obj_t *arc, const lv_font_t *font, uint32_t color, const char *text, int dy)
{
    lv_obj_t *l = label(arc, font, color, text);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
    return l;
}

static void open_find(lv_event_t *e)
{
    (void)e;
    sl_find(s_ui->ip);
    go(s_ui, FIND);
}

static void do_stop(lv_event_t *e)
{
    (void)e;
    sl_stop();
}

static void do_join(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (sl_join(s_ui->known[i], NULL))
    {
        ui_toast("Cambio rete...");
        go(s_ui, HOME);
    }
}

/* ---------------------------------------------------------------- HOME */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, uint32_t color, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 52, 52);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, ui_color(C_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, ui_color(C_LINE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_bg_color(b, ui_color(C_LINE), LV_STATE_PRESSED);
    lv_obj_t *l = label(b, UI_FONT_LARGE, color, symbol);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_port_set_focusable(b);
    return b;
}

/* The arc is the network (online / seen) when idle, the progress while a job runs. */
static void home_update(sl_ui_t *ui)
{
    sl_status_t st;
    sl_status(&st);
    bool busy = st.phase != SL_IDLE && st.phase != SL_FAILED && st.phase != SL_FINDING;
    char text[80];
    if (busy)
    {
        lv_arc_set_value(ui->home_arc, st.percent);
        lv_obj_set_style_arc_color(ui->home_arc, ui_color(C_ACCENT), LV_PART_INDICATOR);
        snprintf(text, sizeof(text), "%d%%", st.percent);
        lv_label_set_text(ui->home_big, text);
        lv_label_set_text(ui->home_sub, st.message);
        lv_obj_set_style_text_color(ui->home_sub, ui_color(C_ACCENT), 0);
    }
    else
    {
        lv_arc_set_value(ui->home_arc, st.devices ? st.online * 100 / st.devices : 0);
        lv_obj_set_style_arc_color(ui->home_arc, ui_color(st.fresh ? C_WARN : C_ACCENT), LV_PART_INDICATOR);
        snprintf(text, sizeof(text), "%d", st.online);
        lv_label_set_text(ui->home_big, text);
        if (st.phase == SL_FAILED)
        {
            snprintf(text, sizeof(text), "%s", st.message);
        }
        else if (st.fresh)
        {
            snprintf(text, sizeof(text), LV_SYMBOL_WARNING " %d %s", st.fresh, st.fresh == 1 ? "nuovo" : "nuovi");
        }
        else
        {
            snprintf(text, sizeof(text), "in linea su %d", st.devices);
        }
        lv_label_set_text(ui->home_sub, text);
        lv_obj_set_style_text_color(ui->home_sub, ui_color(st.phase == SL_FAILED ? C_BAD : st.fresh ? C_WARN : C_DIM), 0);
    }
    lv_label_set_text(ui->home_net, st.ssid[0] ? st.ssid : "Rete");
    sl_check_t c;
    sl_check_result(&c);
    if (c.done)
    {
        snprintf(text, sizeof(text), "%d dBm · router %d ms", c.rssi, c.gateway_avg);
    }
    else
    {
        snprintf(text, sizeof(text), "%s", st.ip[0] ? st.ip : "");
    }
    lv_label_set_text(ui->home_info, text);
}

static void home(sl_ui_t *ui)
{
    const board_info_t *b = board_info();
    int size = b->round ? (b->width < b->height ? b->width : b->height) - 20 : b->width - 70;
    ui->home_arc = gauge(ui->content, size, size / 16, C_ACCENT, 0);
    lv_obj_align(ui->home_arc, b->round ? LV_ALIGN_CENTER : LV_ALIGN_TOP_MID, 0, b->round ? 0 : 6);

    lv_obj_t *col = box(ui->content);
    lv_obj_set_size(col, size * 70 / 100, LV_SIZE_CONTENT);
    lv_obj_align_to(col, ui->home_arc, LV_ALIGN_CENTER, 0, -size / 14);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 2, 0);
    ui->home_net = label(col, UI_FONT_SMALL, C_ACCENT, "");
    lv_label_set_long_mode(ui->home_net, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(ui->home_net, size * 60 / 100, 0);
    ui->home_big = label(col, ui_font_big, C_INK, "–");
    ui->home_sub = label(col, UI_FONT_BODY, C_DIM, "");
    lv_label_set_long_mode(ui->home_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(ui->home_sub, size * 64 / 100, 0);
    ui->home_info = label(col, UI_FONT_SMALL, 0x5C7F86, "");
    lv_label_set_long_mode(ui->home_info, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(ui->home_info, size * 60 / 100, 0);

    lv_obj_t *buttons = box(ui->content);
    lv_obj_set_size(buttons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    round_button(buttons, LV_SYMBOL_LIST, C_ACCENT, open_view, (void *)(intptr_t)DEVICES);
    round_button(buttons, LV_SYMBOL_GPS, C_HOT, open_view, (void *)(intptr_t)DEVICES);
    round_button(buttons, LV_SYMBOL_REFRESH, 0x2EE39A, do_scan, NULL);
    round_button(buttons, LV_SYMBOL_SETTINGS, C_DIM, open_view, (void *)(intptr_t)MENU);
    lv_obj_align(buttons, LV_ALIGN_BOTTOM_MID, 0, b->round ? -8 : -10);
    home_update(ui);
}

static void menu(sl_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Sentinel");
    ui_row(page, LV_SYMBOL_OK, 0x3D6BFF, "Verifica la rete", NULL, do_check, NULL);
    ui_row(page, LV_SYMBOL_WIFI, 0x8E5BFF, "Reti vicine", NULL, do_air, NULL);
    ui_row(page, LV_SYMBOL_SHUFFLE, 0x3D6BFF, "Cambia Wi-Fi", NULL, open_view, (void *)(intptr_t)WIFI);
    ui_row(page, LV_SYMBOL_EYE_OPEN, C_ACCENT, "Dashboard", NULL, open_view, (void *)(intptr_t)DASH);
    ui_text(page, "Nomi e dispositivi fidati sono condivisi con l'app Sentinel completa.", true);
}

/* ------------------------------------------------------------- DEVICES */

static void devices(sl_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Dispositivi");
    int width = card_width();
    sl_status_t st;
    sl_status(&st);
    sl_lock();
    int count;
    const sl_device_t *list = sl_devices(&count);
    for (int i = 0; i < count; i++)
    {
        const sl_device_t *d = &list[i];
        char name[64], ip[16], meta[96];
        sl_display_name(d, name, sizeof(name));
        sl_ip_text(d->ip, ip, sizeof(ip));
        bool gateway = strcmp(ip, st.gateway) == 0;
        const char *kind = sl_kind(d, gateway);
        snprintf(meta, sizeof(meta), "%s%s%s%s", ip, kind[0] ? " · " : "", kind, d->online ? "" : " · non in linea");
        lv_obj_t *card = lv_button_create(page);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, width, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, ui_color(d->fresh ? 0x3A2E14 : C_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(card, ui_color(C_LINE), LV_STATE_PRESSED);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 4, 0);
        lv_obj_add_event_cb(card, open_device, LV_EVENT_CLICKED, (void *)(uintptr_t)d->ip);
        lv_port_set_focusable(card);
        char title[80];
        snprintf(title, sizeof(title), "%s%s%s", d->fresh ? LV_SYMBOL_WARNING " " : "", name, d->trusted ? "  " LV_SYMBOL_OK : "");
        lv_obj_t *t = label(card, UI_FONT_BODY, d->online ? C_INK : C_DIM, title);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(t, width - 24);
        lv_obj_t *m = label(card, UI_FONT_SMALL, C_DIM, meta);
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(m, width - 24);
    }
    sl_unlock();
    if (count == 0)
    {
        ui_text(page, "Nessun dispositivo ancora: la scansione è in corso o il Wi-Fi non è collegato.", true);
    }
    else
    {
        ui_text(page, "In giallo i dispositivi mai visti prima su questa rete.", true);
    }
}

/* -------------------------------------------------------------- DEVICE */

static void device(sl_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, NULL);
    sl_status_t st;
    sl_status(&st);
    sl_device_t d;
    bool found = false;
    sl_lock();
    int count;
    const sl_device_t *list = sl_devices(&count);
    for (int i = 0; i < count; i++)
    {
        if (list[i].ip == ui->ip)
        {
            d = list[i];
            found = true;
        }
    }
    sl_unlock();
    if (!found)
    {
        ui_text(page, "Dispositivo non più in elenco.", true);
        return;
    }
    char name[64], ip[16], mac[20], text[160];
    sl_display_name(&d, name, sizeof(name));
    sl_ip_text(d.ip, ip, sizeof(ip));
    lv_obj_t *t = label(page, ui_font_title, C_INK, name);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, card_width());
    const char *kind = sl_kind(&d, strcmp(ip, st.gateway) == 0);
    if (kind[0])
    {
        label(page, UI_FONT_BODY, C_ACCENT, kind);
    }
    if (d.has_mac)
    {
        sl_mac_text(d.mac, mac, sizeof(mac));
    }
    else
    {
        snprintf(mac, sizeof(mac), "–");
    }
    snprintf(text, sizeof(text), "IP %s\nMAC %s\nProduttore %s\nPing %s", ip, mac, d.vendor[0] ? d.vendor : "sconosciuto",
             d.rtt >= 0 ? "" : "nessuna risposta");
    if (d.rtt >= 0)
    {
        size_t n = strlen(text);
        snprintf(text + n, sizeof(text) - n, "%d ms", d.rtt);
    }
    ui_text(page, text, false);
    char names[200] = "";
    const char *sources[][2] = {{"mDNS", d.mdns}, {"NetBIOS", d.netbios}, {"UPnP", d.upnp}, {"DNS", d.dns}};
    for (int i = 0; i < 4; i++)
    {
        if (sources[i][1][0])
        {
            size_t n = strlen(names);
            snprintf(names + n, sizeof(names) - n, "%s%s: %s", n ? "\n" : "", sources[i][0], sources[i][1]);
        }
    }
    if (names[0])
    {
        ui_section(page, "Come si presenta");
        ui_text(page, names, true);
    }
    ui_switch_row(page, LV_SYMBOL_OK, C_ACCENT, "Dispositivo fidato", d.trusted, toggle_trust, NULL);
    ui_section(page, "Servizi aperti");
    if (!d.ports_checked)
    {
        ui_text(page, "Non ancora controllati.", true);
    }
    else if (d.open_ports == 0)
    {
        ui_text(page, "Nessun servizio comune aperto.", true);
    }
    else
    {
        int pcount;
        const uint16_t *ports = sl_port_list(&pcount);
        for (int i = 0; i < pcount; i++)
        {
            if (!(d.open_ports & (1u << i)))
            {
                continue;
            }
            const char *warning = sl_port_warning(i);
            snprintf(text, sizeof(text), "%u  %s%s%s", ports[i], sl_port_name(i), warning ? "\n" : "", warning ? warning : "");
            lv_obj_t *l = ui_text(page, text, false);
            if (warning)
            {
                lv_obj_set_style_text_color(l, ui_color(C_WARN), 0);
            }
        }
    }
    if (d.online && d.has_mac)
    {
        ui_button(page, "Trova (caldo/freddo)", C_HOT, open_find, NULL);
    }
    ui_button(page, "Controlla i servizi", 0x2E7D78, do_ports, NULL);
    ui_text(page, "Il nome si cambia dalla dashboard.", true);
}

/* --------------------------------------------------------------- CHECK */

static void check(sl_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Verifica rete");
    sl_check_t c;
    sl_check_result(&c);
    sl_status_t st;
    sl_status(&st);
    if (st.phase == SL_CHECKING || st.phase == SL_WIFI)
    {
        ui->status = ui_text(page, st.message, true);
        return;
    }
    if (!c.done)
    {
        ui_text(page, "Nessuna verifica ancora.", true);
        return;
    }
    lv_obj_t *v = label(page, UI_FONT_BODY, c.gateway_avg >= 0 && c.internet_ms >= 0 && c.gateway_loss < 20 ? C_ACCENT : C_WARN,
                        c.verdict);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(v, card_width());
    char text[220];
    snprintf(text, sizeof(text),
             "Segnale %d dBm · canale %d\nSicurezza %s\nRouter %d ms (max %d) · persi %d%%\nInternet %s\nDNS %s",
             c.rssi, c.channel, c.security, c.gateway_avg, c.gateway_max, c.gateway_loss, c.internet_ms >= 0 ? "" : "non raggiungibile",
             c.dns_ms >= 0 ? "" : "non risponde");
    ui_text(page, text, false);
    if (c.internet_ms >= 0 || c.dns_ms >= 0)
    {
        snprintf(text, sizeof(text), "Internet %d ms · DNS %d ms", c.internet_ms, c.dns_ms);
        ui_text(page, text, true);
    }
    ui_button(page, "Ripeti", 0x3D6BFF, do_check, NULL);
}

/* ----------------------------------------------------------------- AIR */

static void air(sl_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Reti vicine");
    sl_status_t st;
    sl_status(&st);
    if (st.phase == SL_AIR)
    {
        ui->status = ui_text(page, st.message, true);
        return;
    }
    int load[14] = {0};
    sl_lock();
    int count;
    const sl_ap_t *aps = sl_aps(&count);
    for (int i = 0; i < count; i++)
    {
        for (int c = 1; c <= 13; c++)
        {
            load[c] += aps[i].channel && abs(aps[i].channel - c) <= 2;
        }
        char text[96];
        snprintf(text, sizeof(text), "%s%s\nch %d · %d dBm · %s", aps[i].ssid[0] ? aps[i].ssid : "(nascosta)",
                 aps[i].current ? "  " LV_SYMBOL_OK : "", aps[i].channel, aps[i].rssi, aps[i].security);
        lv_obj_t *l = ui_text(page, text, false);
        if (aps[i].current)
        {
            lv_obj_set_style_text_color(l, ui_color(C_ACCENT), 0);
        }
    }
    sl_unlock();
    if (count > 0)
    {
        int best = 1;
        for (int c = 6; c <= 11; c += 5)
        {
            best = load[c] < load[best] ? c : best;
        }
        char text[96];
        snprintf(text, sizeof(text), "Canale più libero tra 1, 6 e 11: %d (%d reti vicine lo usano)", best, load[best]);
        ui_section(page, text);
    }
    else
    {
        ui_text(page, "Nessuna rete trovata.", true);
    }
    ui_button(page, "Ascolta di nuovo", 0x8E5BFF, do_air, NULL);
}

/* ---------------------------------------------------------------- FIND */

static void find_update(sl_ui_t *ui)
{
    sl_find_t f;
    sl_find_state(&f);
    int level = f.frames ? (f.smooth + 90) * 100 / 55 : 0;   /* -90 dBm cold .. -35 dBm hot */
    level = level < 0 ? 0 : level > 100 ? 100 : level;
    lv_color_t color = lv_color_mix(ui_color(C_HOT), ui_color(C_COLD), (uint8_t)(level * 255 / 100));
    static const char *const words[] = {"Gelido", "Freddo", "Tiepido", "Caldo", "Bollente"};
    char text[96];
    lv_arc_set_value(ui->find_arc, level);
    lv_obj_set_style_arc_color(ui->find_arc, color, LV_PART_INDICATOR);
    snprintf(text, sizeof(text), f.frames ? "%d" : "–", f.smooth);
    lv_label_set_text(ui->find_value, text);
    lv_label_set_text(ui->find_word, f.frames ? words[level >= 100 ? 4 : level / 20] : f.running ? "In ascolto" : "Nessun segnale");
    lv_obj_set_style_text_color(ui->find_word, f.frames ? color : ui_color(C_DIM), 0);
    if (!f.frames)
    {
        snprintf(text, sizeof(text), "%s", f.running ? "Aspetto le risposte..." : "Forse è a 5 GHz, via cavo\no su un altro access point");
    }
    else
    {
        snprintf(text, sizeof(text), "%s\nmeglio %d dBm · %d s", f.trend > 0 ? "Ti avvicini" : f.trend < 0 ? "Ti allontani" : "Stabile",
                 f.best, f.seconds);
    }
    lv_label_set_text(ui->find_info, text);
}

static void find_view(sl_ui_t *ui)
{
    sl_find_t f;
    sl_find_state(&f);
    lv_obj_t *page = ui_page(ui->content, f.name[0] ? f.name : "Trova");
    int size = card_width() * 4 / 5;
    ui->find_arc = gauge(page, size, size / 10, C_COLD, 0);
    ui->find_value = inside(ui->find_arc, ui_font_big, C_INK, "–", -size / 12);
    ui->find_word = inside(ui->find_arc, ui_font_headline, C_DIM, "", size / 6);
    ui->find_info = label(page, UI_FONT_SMALL, C_DIM, "");
    lv_obj_set_style_text_align(ui->find_info, LV_TEXT_ALIGN_CENTER, 0);
    find_update(ui);
    if (f.running)
    {
        ui_button(page, "Ferma la ricerca", C_BAD, do_stop, NULL);
    }
    else if (f.ip != 0)
    {
        ui->ip = f.ip;
        ui_button(page, "Cerca di nuovo", C_HOT, open_find, NULL);
    }
    ui_text(page, "Cammina: il segnale delle risposte sale quando ti avvicini. Massimo 3 minuti.", true);
}

/* ---------------------------------------------------------------- WIFI */

static void wifi_view(sl_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Cambia Wi-Fi");
    wifi_known_t *known = heap_caps_calloc(KNOWN, sizeof(wifi_known_t), MALLOC_CAP_SPIRAM);
    int n = known != NULL ? wifi_known_list(known, KNOWN) : 0;
    char current[33] = "";
    wifi_current_ssid(current, sizeof(current));
    for (int i = 0; i < n; i++)
    {
        snprintf(ui->known[i], sizeof(ui->known[i]), "%s", known[i].ssid);
        bool here = current[0] && strcmp(current, known[i].ssid) == 0;
        ui_row(page, LV_SYMBOL_WIFI, here ? C_ACCENT : 0x3D6BFF, ui->known[i], here ? "in uso" : NULL, here ? NULL : do_join,
               (void *)(intptr_t)i);
    }
    if (known != NULL)
    {
        memset(known, 0, KNOWN * sizeof(wifi_known_t));
        free(known);
    }
    ui_text(page, n ? "Tocca una rete per spostare l'orologio. Le reti nuove si aggiungono dalla dashboard."
                    : "Nessuna rete salvata.", true);
}

static void dash(sl_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Dashboard");
    appkit_dashboard_page(page, ui_color(C_ACCENT));
}

static void render(sl_ui_t *ui)
{
    if (ui->content != NULL)
    {
        lv_obj_delete(ui->content);
    }
    ui->status = NULL;
    ui->find_arc = NULL;
    ui->home_arc = NULL;
    ui->content = lv_obj_create(ui->screen);
    lv_obj_remove_style_all(ui->content);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->content, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->content, LV_OPA_COVER, 0);
    lv_obj_move_to_index(ui->content, 0);
    sl_status_t st;
    sl_status(&st);
    ui->phase = st.phase;
    ui->revision = st.revision;
    switch (ui->view)
    {
    case HOME: home(ui); break;
    case DEVICES: devices(ui); break;
    case DEVICE: device(ui); break;
    case CHECK: check(ui); break;
    case AIR: air(ui); break;
    case FIND: find_view(ui); break;
    case WIFI: wifi_view(ui); break;
    case MENU: menu(ui); break;
    default: dash(ui); break;
    }
}

static void tick(lv_timer_t *timer)
{
    sl_ui_t *ui = lv_timer_get_user_data(timer);
    sl_status_t st;
    sl_status(&st);
    bool busy = st.phase != SL_IDLE && st.phase != SL_FAILED && st.phase != SL_FINDING;
    if (ui->border != NULL)
    {
        lv_obj_set_hidden(ui->border, !busy || ui->view == HOME);   /* on HOME the arc is the progress */
        if (busy)
        {
            ui_border_progress_set(ui->border, st.percent, 100);
        }
    }
    if (ui->status != NULL && st.revision != ui->revision)
    {
        lv_label_set_text(ui->status, st.message);
    }
    if (ui->find_arc != NULL && st.revision != ui->revision)
    {
        find_update(ui);
    }
    if (ui->home_arc != NULL && st.revision != ui->revision)
    {
        home_update(ui);
    }
    bool ended = st.phase != ui->phase && (st.phase == SL_IDLE || st.phase == SL_FAILED);
    if (ended && ui->view != DASH && ui->view != WIFI && ui->view != HOME)
    {
        render(ui);
    }
    ui->phase = st.phase;
    ui->revision = st.revision;
}

bool sentinel_ui_back(void)
{
    sl_ui_t *ui = s_ui;
    if (ui == NULL || ui->depth == 0)
    {
        return false;
    }
    ui->view = ui->history[--ui->depth];
    render(ui);
    return true;
}

void sentinel_ui_create(lv_obj_t *screen)
{
    sl_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    if (ui == NULL)
    {
        ui_text(ui_page(screen, "Sentinel"), "Memoria insufficiente.", true);
        return;
    }
    s_ui = ui;
    ui->screen = screen;
    lv_obj_set_style_bg_color(screen, ui_color(C_BG), 0);
    render(ui);
    ui->border = ui_border_progress_create(screen, C_ACCENT);
    if (ui->border != NULL)
    {
        lv_obj_set_hidden(ui->border, true);
    }
    ui->timer = lv_timer_create(tick, 500, ui);
}

void sentinel_ui_destroy(void)
{
    sl_ui_t *ui = s_ui;
    if (ui == NULL)
    {
        return;
    }
    s_ui = NULL;
    if (ui->timer != NULL)
    {
        lv_timer_delete(ui->timer);
    }
    free(ui);
}
