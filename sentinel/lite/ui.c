/*
 * Sentinel Lite on the watch.
 *
 *   HOME     the network: name, devices online, new ones, last scan; actions
 *   DEVICES  who is connected (online first), new ones marked
 *   DEVICE   one device: name, kind, maker, IP, MAC, names heard; check its
 *            services; mark it as trusted
 *   CHECK    router / internet / DNS times, signal, security, a verdict
 *   AIR      nearby networks and the least busy channel
 *   DASH     QR code + code for the web dashboard
 */
#include "lite.h"

#include "appkit.h"
#include "core/lv_port.h"
#include "hardware/board.h"
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

#define HISTORY 8

enum { HOME, DEVICES, DEVICE, CHECK, AIR, DASH };

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

/* ---------------------------------------------------------------- HOME */

static void home(sl_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Sentinel");
    sl_status_t st;
    sl_status(&st);
    char text[96];
    lv_obj_t *net = label(page, ui_font_headline, C_ACCENT, st.ssid[0] ? st.ssid : "Rete");
    lv_obj_set_style_text_align(net, LV_TEXT_ALIGN_CENTER, 0);
    snprintf(text, sizeof(text), "%d", st.online);
    lv_obj_t *big = label(page, ui_font_big, C_INK, text);
    lv_obj_set_style_text_align(big, LV_TEXT_ALIGN_CENTER, 0);
    snprintf(text, sizeof(text), "dispositivi connessi%s", st.fresh ? "" : "");
    label(page, UI_FONT_SMALL, C_DIM, text);
    if (st.fresh > 0)
    {
        snprintf(text, sizeof(text), LV_SYMBOL_WARNING "  %d nuovi da quando l'hai vista", st.fresh);
        label(page, UI_FONT_BODY, C_WARN, text);
    }
    ui->status = ui_text(page, st.message, true);
    lv_obj_set_style_text_align(ui->status, LV_TEXT_ALIGN_CENTER, 0);
    ui_row(page, LV_SYMBOL_LIST, 0x2E7D78, "Dispositivi", NULL, open_view, (void *)(intptr_t)DEVICES);
    ui_row(page, LV_SYMBOL_REFRESH, 0x2E7D78, "Scansiona di nuovo", NULL, do_scan, NULL);
    ui_row(page, LV_SYMBOL_OK, 0x3D6BFF, "Verifica la rete", NULL, do_check, NULL);
    ui_row(page, LV_SYMBOL_WIFI, 0x8E5BFF, "Reti vicine", NULL, do_air, NULL);
    ui_row(page, LV_SYMBOL_EYE_OPEN, C_ACCENT, "Dashboard", NULL, open_view, (void *)(intptr_t)DASH);
    ui_text(page, "Sentinel Lite: inventario, verifica e reti vicine. I dispositivi restano salvati per ogni rete.", true);
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
    default: dash(ui); break;
    }
}

static void tick(lv_timer_t *timer)
{
    sl_ui_t *ui = lv_timer_get_user_data(timer);
    sl_status_t st;
    sl_status(&st);
    bool busy = st.phase != SL_IDLE && st.phase != SL_FAILED;
    if (ui->border != NULL)
    {
        lv_obj_set_hidden(ui->border, !busy);
        if (busy)
        {
            ui_border_progress_set(ui->border, st.percent, 100);
        }
    }
    if (ui->status != NULL && st.revision != ui->revision)
    {
        lv_label_set_text(ui->status, st.message);
    }
    bool ended = st.phase != ui->phase && (st.phase == SL_IDLE || st.phase == SL_FAILED);
    if (ended && ui->view != DASH)
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
