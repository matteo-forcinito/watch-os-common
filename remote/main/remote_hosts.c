/*
 * Computer page: the Bluetooth hosts paired with Remote.
 *
 * Tap a computer to use it: Remote drops the current one and lets only that
 * one connect (hot switch, nothing to do on the PCs). "Qualsiasi" restores the
 * old behaviour, the first paired host that connects wins. "Aggiungi computer"
 * opens pairing for one new host, which becomes the chosen one. Long press a
 * computer to forget it. Names come from the Watch Deck agent on each PC.
 */
#include "remote.h"
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

typedef struct
{
    lv_obj_t *status, *list;
    lv_timer_t *timer;
    hid_link_host_t hosts[HID_LINK_HOSTS_MAX];
    int count;
    uint32_t shown; /* signature of what the list shows */
    bool long_pressed;
} hosts_ui_t;

static hosts_ui_t *s_ui;

static uint32_t signature(const hid_link_host_t *hosts, int count)
{
    uint32_t hash = 2166136261u ^ (uint32_t)count ^ (hid_link_pairing() ? 0x80000000u : 0) ^
                    (hid_link_state() == HID_LINK_CONNECTED ? 0x40000000u : 0);
    for (int i = 0; i < count; i++)
    {
        const uint8_t *bytes = (const uint8_t *)&hosts[i];
        for (size_t b = 0; b < sizeof(hosts[i]); b++)
            hash = (hash ^ bytes[b]) * 16777619u;
    }
    return hash;
}

static const char *chosen_name(const hosts_ui_t *ui)
{
    for (int i = 0; i < ui->count; i++)
        if (ui->hosts[i].chosen)
            return ui->hosts[i].name;
    return NULL;
}

static void host_pressed(lv_event_t *e)
{
    (void)e;
    if (s_ui != NULL)
        s_ui->long_pressed = false;
}

static void forget_confirmed(bool confirmed, void *user_data)
{
    int i = (int)(intptr_t)user_data;
    if (!confirmed || s_ui == NULL || i >= s_ui->count)
        return;
    hid_link_forget_host(&s_ui->hosts[i]);
    ui_toast("Computer dimenticato");
    s_ui->shown = 0;
}

static void host_long_pressed(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_ui == NULL || i >= s_ui->count)
        return;
    s_ui->long_pressed = true;
    static char text[96];
    snprintf(text, sizeof(text), "%s dovrà associare di nuovo Remote dalle impostazioni Bluetooth.",
             s_ui->hosts[i].name);
    ui_confirm("Dimentica computer", text, "Dimentica", UI_COLOR_RED, forget_confirmed, (void *)(intptr_t)i);
}

static void host_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_ui == NULL || s_ui->long_pressed)
        return;
    if (i < 0)
    {
        hid_link_choose_host(NULL);
        ui_toast("Si collega il primo computer disponibile");
    }
    else if (i < s_ui->count && !s_ui->hosts[i].chosen)
    {
        hid_link_choose_host(&s_ui->hosts[i]);
        char text[48];
        snprintf(text, sizeof(text), "Passo a %s", s_ui->hosts[i].name);
        ui_toast(text);
    }
    s_ui->shown = 0;
}

static void pair_clicked(lv_event_t *e)
{
    (void)e;
    bool open = !hid_link_pairing();
    hid_link_pair_host(open);
    if (s_ui != NULL)
        s_ui->shown = 0;
}

static void rebuild(hosts_ui_t *ui)
{
    lv_obj_clean(ui->list);
    bool pairing = hid_link_pairing();
    bool any = chosen_name(ui) == NULL;

    ui_section(ui->list, "SCEGLI IL COMPUTER");
    for (int i = 0; i < ui->count; i++)
    {
        const hid_link_host_t *host = &ui->hosts[i];
        lv_obj_t *row = ui_row(ui->list, host->chosen ? LV_SYMBOL_OK : LV_SYMBOL_BLUETOOTH,
                               host->connected ? UI_COLOR_GREEN : host->chosen ? UI_COLOR_BLUE : UI_COLOR_GRAY,
                               host->name, host->connected ? "Collegato" : host->chosen ? "Scelto" : NULL,
                               host_clicked, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, host_pressed, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, host_long_pressed, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
    ui_row(ui->list, LV_SYMBOL_SHUFFLE, any ? UI_COLOR_BLUE : UI_COLOR_GRAY, "Qualsiasi", any ? "Scelto" : NULL,
           host_clicked, (void *)(intptr_t)-1);

    ui_section(ui->list, "NUOVO COMPUTER");
    ui_row(ui->list, pairing ? LV_SYMBOL_CLOSE : LV_SYMBOL_PLUS, pairing ? UI_COLOR_RED : UI_COLOR_INDIGO,
           pairing ? "Annulla associazione" : "Aggiungi computer", NULL, pair_clicked, NULL);
    ui_text(ui->list,
            ui->count ? "Tocca un computer per passarci subito; tieni premuto per dimenticarlo."
                      : "Nessun computer associato.",
            true);
}

static void tick(lv_timer_t *timer)
{
    hosts_ui_t *ui = lv_timer_get_user_data(timer);
    if (hid_link_transport() != HID_LINK_BLE)
    {
        if (ui->shown != 1)
        {
            lv_label_set_text(ui->status, "Disponibile solo con il collegamento Bluetooth.");
            lv_obj_clean(ui->list);
            ui->shown = 1;
        }
        return;
    }
    ui->count = hid_link_hosts(ui->hosts, HID_LINK_HOSTS_MAX);
    uint32_t now = signature(ui->hosts, ui->count);
    if (now == ui->shown)
        return;
    ui->shown = now;

    static char text[128];
    const char *chosen = chosen_name(ui);
    const char *connected = NULL;
    for (int i = 0; i < ui->count; i++)
        if (ui->hosts[i].connected)
            connected = ui->hosts[i].name;
    if (hid_link_pairing())
        snprintf(text, sizeof(text), "Sul nuovo computer: Bluetooth > Aggiungi dispositivo > \"%s\".",
                 remote_device_name());
    else if (connected != NULL && hid_link_state() == HID_LINK_CONNECTED)
        snprintf(text, sizeof(text), "Collegato a %s", connected);
    else if (chosen != NULL)
        snprintf(text, sizeof(text), "In attesa di %s", chosen);
    else
        snprintf(text, sizeof(text), "In attesa di un computer");
    lv_label_set_text(ui->status, text);
    rebuild(ui);
}

static void deleted(lv_event_t *e)
{
    hosts_ui_t *ui = lv_event_get_user_data(e);
    lv_timer_delete(ui->timer);
    if (hid_link_pairing())
        hid_link_pair_host(false); /* pairing lasts while the page is open */
    if (s_ui == ui)
        s_ui = NULL;
    lv_free(ui);
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    hosts_ui_t *ui = lv_malloc_zeroed(sizeof(*ui));
    if (ui == NULL)
        return;
    s_ui = ui;
    lv_obj_t *page = ui_page(screen, "Computer");
    ui->status = ui_text(page, "", false);
    ui->list = lv_obj_create(page);
    lv_obj_remove_style_all(ui->list);
    lv_obj_set_size(ui->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ui->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui->list, 8, 0);
    ui->timer = lv_timer_create(tick, 400, ui);
    lv_obj_add_event_cb(screen, deleted, LV_EVENT_DELETE, ui);
    tick(ui->timer);
}

/* Home row value: the computer in use, or the chosen one. */
const char *remote_host_text(void)
{
    static hid_link_host_t hosts[HID_LINK_HOSTS_MAX];
    int count = hid_link_hosts(hosts, HID_LINK_HOSTS_MAX);
    for (int i = 0; i < count; i++)
        if (hosts[i].connected && hid_link_state() == HID_LINK_CONNECTED)
            return hosts[i].name;
    for (int i = 0; i < count; i++)
        if (hosts[i].chosen)
            return hosts[i].name;
    return "Qualsiasi";
}

const app_t remote_hosts_app = {
    .id = "remote.hosts",
    .name = "Computer",
    .icon = LV_SYMBOL_BLUETOOTH,
    .color = UI_COLOR_INDIGO,
    .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON,
    .create = create,
};
