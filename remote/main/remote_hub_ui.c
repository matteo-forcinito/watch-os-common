/*
 * Remote Hub pages: role, paired watches and the pairing procedure.
 *
 * Hub: this watch stays connected to the computer (USB or Bluetooth) and
 * accepts up to four satellites. Satellite: this watch sends its controls to a
 * hub by radio. Pairing: the hub opens a window, the satellite picks it, both
 * show the same 6-digit code; confirm on both. Role changes restart the app.
 */
#include "remote.h"
#include "hid_link/hid_hub.h"
#include "extapp_sdk.h"
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

static const char *role_name(hid_hub_role_t role)
{
    return role == HID_HUB_HUB ? "Hub" : role == HID_HUB_SATELLITE ? "Satellite" : "Spento";
}

const char *remote_hub_role_text(void)
{
    return role_name(hid_hub_role());
}

/* ------------------------------------------------------------ pairing page */

typedef struct
{
    lv_obj_t *page, *message, *code, *list, *actions;
    lv_timer_t *timer;
    hid_hub_pair_phase_t shown;
    int found_shown;
} pairing_ui_t;

static void pair_select(lv_event_t *e)
{
    if (hid_hub_pair_select((int)(intptr_t)lv_event_get_user_data(e)) != ESP_OK)
        ui_toast("Hub non piu disponibile");
}

static void pair_answer(lv_event_t *e)
{
    int answer = (int)(intptr_t)lv_event_get_user_data(e);
    if (answer == 2)
    {
        hid_hub_pair_open(); /* try again */
        return;
    }
    if (answer == 3)
    {
        app_back();
        return;
    }
    hid_hub_pair_confirm(answer == 1);
}

static void pair_actions(pairing_ui_t *ui, hid_hub_pair_phase_t phase)
{
    lv_obj_clean(ui->actions);
    if (phase == HID_HUB_PAIR_CODE)
    {
        ui_button(ui->actions, "Il codice coincide", UI_COLOR_GREEN, pair_answer, (void *)1);
        ui_button(ui->actions, "Rifiuta", UI_COLOR_RED, pair_answer, (void *)0);
    }
    else if (phase == HID_HUB_PAIR_FAILED)
        ui_button(ui->actions, "Riprova", UI_COLOR_BLUE, pair_answer, (void *)2);
    else if (phase == HID_HUB_PAIR_DONE)
        ui_button(ui->actions, "Fatto", UI_COLOR_GREEN, pair_answer, (void *)3);
}

static void pairing_tick(lv_timer_t *timer)
{
    pairing_ui_t *ui = lv_timer_get_user_data(timer);
    hid_hub_pairing_t state;
    hid_hub_pairing(&state);
    char text[96];
    if (state.phase == HID_HUB_PAIR_OPEN || state.phase == HID_HUB_PAIR_CODE || state.phase == HID_HUB_PAIR_CONFIRMED)
        snprintf(text, sizeof(text), "%s\n%d s", state.message, state.seconds_left);
    else
        snprintf(text, sizeof(text), "%s", state.message);
    lv_label_set_text(ui->message, text);
    if (state.code)
    {
        snprintf(text, sizeof(text), "%03lu %03lu", (unsigned long)(state.code / 1000), (unsigned long)(state.code % 1000));
        lv_label_set_text(ui->code, text);
    }
    else
        lv_label_set_text(ui->code, "");
    if (state.phase != ui->shown)
    {
        ui->shown = state.phase;
        pair_actions(ui, state.phase);
    }
    /* Satellite: hubs found while searching. */
    if (hid_hub_role() == HID_HUB_SATELLITE && state.phase == HID_HUB_PAIR_OPEN)
    {
        hid_hub_peer_info_t found[6];
        int count = hid_hub_pair_found(found, 6);
        if (count != ui->found_shown)
        {
            ui->found_shown = count;
            lv_obj_clean(ui->list);
            for (int i = 0; i < count; i++)
            {
                char value[16];
                snprintf(value, sizeof(value), "%d dBm", found[i].rssi);
                ui_row(ui->list, LV_SYMBOL_WIFI, UI_COLOR_TEAL, found[i].name, value, pair_select, (void *)(intptr_t)i);
            }
        }
    }
    else if (ui->found_shown)
    {
        ui->found_shown = 0;
        lv_obj_clean(ui->list);
    }
}

static void pairing_deleted(lv_event_t *e)
{
    pairing_ui_t *ui = lv_event_get_user_data(e);
    lv_timer_delete(ui->timer);
    hid_hub_pairing_t state;
    hid_hub_pairing(&state);
    if (state.phase != HID_HUB_PAIR_DONE)
        hid_hub_pair_cancel();
    lv_free(ui);
}

static lv_obj_t *column(lv_obj_t *page)
{
    lv_obj_t *box = lv_obj_create(page);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 8, 0);
    return box;
}

static void pairing_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    pairing_ui_t *ui = lv_malloc_zeroed(sizeof(*ui));
    if (ui == NULL)
        return;
    bool hub = hid_hub_role() == HID_HUB_HUB;
    ui->page = ui_page(screen, hub ? "Aggiungi dispositivo" : "Cerca hub");
    ui->message = ui_text(ui->page, "", true);
    ui->code = ui_text(ui->page, "", false);
    lv_obj_set_style_text_font(ui->code, ui_font_headline, 0);
    ui->list = column(ui->page);
    ui->actions = column(ui->page);
    ui_text(ui->page,
            hub ? "Sull'altro orologio apri Remote > Hub > Cerca hub. Confronta il codice e conferma su entrambi."
                : "Sull'hub apri Remote > Hub > Aggiungi dispositivo, poi scegli l'hub qui.",
            true);
    ui->shown = HID_HUB_PAIR_IDLE;
    ui->found_shown = 0;
    if (hid_hub_pair_open() != ESP_OK)
        ui_toast("Radio hub non attiva: riavvia l'app");
    ui->timer = lv_timer_create(pairing_tick, 200, ui);
    lv_obj_add_event_cb(screen, pairing_deleted, LV_EVENT_DELETE, ui);
}

static const app_t s_pairing_app = {
    .id = "remote.hub.pair", .name = "Abbinamento", .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON,
    .create = pairing_create};

/* ------------------------------------------------------------ hub page */

static void role_confirmed(bool confirmed, void *user_data)
{
    if (!confirmed)
        return;
    if (hid_hub_set_role((hid_hub_role_t)(uintptr_t)user_data) != ESP_OK)
    {
        ui_toast("Salvataggio non riuscito");
        return;
    }
    extapp_restart_self();
}

static void choose_role(lv_event_t *e)
{
    hid_hub_role_t role = (hid_hub_role_t)(uintptr_t)lv_event_get_user_data(e);
    if (role == hid_hub_role())
        return;
    static const char *texts[] = {
        "Remote torna un dispositivo singolo. Gli abbinamenti hub vengono cancellati.",
        "Questo orologio resta collegato al computer e accoglie fino a 4 satelliti. Remote si riavvia.",
        "Questo orologio invia i comandi a un hub via radio, senza Bluetooth verso il PC. Remote si riavvia.",
    };
    ui_confirm(role == HID_HUB_OFF ? "Disattiva hub" : role == HID_HUB_HUB ? "Diventa hub" : "Diventa satellite",
               texts[role], "Riavvia", UI_COLOR_BLUE, role_confirmed, (void *)(uintptr_t)role);
}

static void open_pairing(lv_event_t *e)
{
    (void)e;
    app_open_app(&s_pairing_app, NULL);
}

static void forget_confirmed(bool confirmed, void *user_data)
{
    if (confirmed && hid_hub_forget((int)(intptr_t)user_data) == ESP_OK)
    {
        ui_toast("Abbinamento rimosso");
        app_back();
    }
}

static void forget_peer(lv_event_t *e)
{
    ui_confirm("Rimuovi abbinamento", "Il dispositivo dovra essere abbinato di nuovo.", "Rimuovi", UI_COLOR_RED,
               forget_confirmed, lv_event_get_user_data(e));
}

static void hub_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_t *page = ui_page(screen, "Hub");
    hid_hub_role_t role = hid_hub_role();
    ui_text(page, "Piu orologi come un unico insieme di tastiera, mouse e gamepad sul computer.", true);
    ui_section(page, "RUOLO");
    for (unsigned r = HID_HUB_OFF; r <= HID_HUB_SATELLITE; r++)
        ui_row(page, r == role ? LV_SYMBOL_OK : LV_SYMBOL_RIGHT, r == role ? UI_COLOR_GREEN : UI_COLOR_GRAY,
               r == HID_HUB_OFF ? "Spento" : r == HID_HUB_HUB ? "Hub: collegato al PC" : "Satellite",
               r == role ? "attivo" : NULL, choose_role, (void *)(uintptr_t)r);
    if (role == HID_HUB_OFF)
        return;
    ui_section(page, role == HID_HUB_HUB ? "SATELLITI" : "HUB");
    hid_hub_peer_info_t peers[HID_HUB_PEERS];
    int count = hid_hub_peers(peers, HID_HUB_PEERS);
    for (int i = 0; i < count; i++)
        ui_row(page, LV_SYMBOL_WIFI, peers[i].connected ? UI_COLOR_GREEN : UI_COLOR_GRAY, peers[i].name,
               peers[i].connected ? "collegato" : "non raggiungibile", forget_peer, (void *)(intptr_t)i);
    if (count == 0)
        ui_text(page, role == HID_HUB_HUB ? "Nessun satellite abbinato." : "Nessun hub abbinato.", true);
    if (role == HID_HUB_HUB && count < HID_HUB_PEERS)
        ui_button(page, "Aggiungi dispositivo", UI_COLOR_BLUE, open_pairing, NULL);
    if (role == HID_HUB_SATELLITE)
        ui_button(page, count ? "Cambia hub" : "Cerca hub", UI_COLOR_BLUE, open_pairing, NULL);
    ui_text(page,
            role == HID_HUB_HUB ? "Collegamento al PC: quello scelto nella pagina Remote (USB o Bluetooth). Tocca un "
                                  "satellite per rimuoverlo."
                                : "Le modalita di Remote (tastiera, mouse, air mouse, gamepad) arrivano al PC tramite l'hub.",
            true);
}

const app_t remote_hub_app = {.id = "remote.hub", .name = "Hub", .flags = APP_FLAG_HIDDEN, .create = hub_create};

void remote_hub_register(void)
{
    app_register(&remote_hub_app);
    app_register(&s_pairing_app);
}
