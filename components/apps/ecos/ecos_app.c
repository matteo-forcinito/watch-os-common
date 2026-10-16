/* Ecosistema: the family of paired devices.
 *
 *   ecos         the list: who is there, battery, last contact (radio on while open)
 *   ecos.device  one device: its info, ring it, send a quick message, forget it
 *   ecos.pair    pairing a new device (code compared on both screens)
 *   ecos.find    shown here when another device rings this one */
#include "apps_internal.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/sys.h"
#include "esp_heap_caps.h"
#include "services/ecos.h"
#include "services/sound.h"
#include "ui/ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIND_SECONDS 60

static void ago_text(uint32_t seconds, char *out, size_t size)
{
    if (seconds == UINT32_MAX)
    {
        snprintf(out, size, "mai visto");
    }
    else if (seconds < 60)
    {
        snprintf(out, size, "ora");
    }
    else if (seconds < 3600)
    {
        snprintf(out, size, "%lu min fa", (unsigned long)(seconds / 60));
    }
    else
    {
        snprintf(out, size, "%lu h fa", (unsigned long)(seconds / 3600));
    }
}

/* ------------------------------------------------------------ the list */

typedef struct
{
    lv_timer_t *timer;
    lv_obj_t *status, *list, *rows[ECOS_PEERS_MAX], *search;
    char ids[ECOS_PEERS_MAX][13];
    int shown;
    uint32_t ticks;
    ecos_family_t family;
} list_ui_t;

static void open_device(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    char *copy = malloc(13);   /* the open may be queued: create() frees it */
    if (copy != NULL)
    {
        snprintf(copy, 13, "%s", id);
        if (!app_open_app(&ecos_device_app, copy))
        {
            free(copy);
        }
    }
}

static void member_value(const ecos_member_t *m, char *out, size_t size)
{
    if (m->reachable && m->battery >= 0)
    {
        snprintf(out, size, "%d%%%s", m->battery, m->charging ? " +" : "");
    }
    else if (m->reachable)
    {
        snprintf(out, size, "vicino");
    }
    else
    {
        ago_text(m->seen_ago, out, size);
    }
}

static void list_refresh(list_ui_t *ui)
{
    ecos_family(&ui->family);
    const ecos_family_t *f = &ui->family;
    bool same = f->count == ui->shown;
    for (int i = 0; same && i < f->count; i++)
    {
        same = strcmp(ui->ids[i], f->members[i].id) == 0;
    }
    if (!same)
    {
        lv_obj_clean(ui->list);
        ui->shown = 0;
        for (int i = 0; i < f->count; i++)
        {
            snprintf(ui->ids[i], sizeof(ui->ids[i]), "%s", f->members[i].id);
            ui->rows[i] = ui_row(ui->list, LV_SYMBOL_LOOP, UI_COLOR_GRAY, f->members[i].name, "", open_device,
                                 ui->ids[i]);
            ui->shown++;
        }
        if (f->count == 0)
        {
            ui_text(ui->list, "Nessun dispositivo abbinato. Abbinane uno qui sotto.", true);
        }
    }
    for (int i = 0; i < ui->shown; i++)
    {
        const ecos_member_t *m = &f->members[i];
        char value[24];
        member_value(m, value, sizeof(value));
        lv_label_set_text(ui_row_value(ui->rows[i]), value);
        lv_obj_t *bubble = lv_obj_get_child(ui->rows[i], 0);
        if (bubble != NULL)
        {
            lv_obj_set_style_bg_color(bubble, lv_color_hex(m->reachable ? UI_COLOR_GREEN : UI_COLOR_GRAY), 0);
        }
    }
    lv_label_set_text(ui->status, f->busy ? "Cerco i dispositivi..." : f->message[0] ? f->message
                                                                                    : "Famiglia di dispositivi");
    lv_obj_set_state(ui->search, LV_STATE_DISABLED, f->busy);
}

static void list_tick(lv_timer_t *timer)
{
    list_ui_t *ui = lv_timer_get_user_data(timer);
    /* A new search every 30 s while the list is on screen. */
    if (++ui->ticks % 30 == 0)
    {
        ecos_family_refresh();
    }
    list_refresh(ui);
}

static void search_click(lv_event_t *e)
{
    (void)e;
    if (ecos_family_refresh() != ESP_OK)
    {
        ui_toast("Abbinamento in corso: riprova dopo");
    }
}

static void list_deleted(lv_event_t *e)
{
    list_ui_t *ui = lv_event_get_user_data(e);
    if (ui->timer != NULL)
    {
        lv_timer_delete(ui->timer);
    }
    ecos_family_hold(false);
    free(ui);
}

static void list_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_t *page = ui_page(screen, "Ecosistema");
    list_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    if (ui == NULL)
    {
        ui_text(page, "Memoria insufficiente", true);
        return;
    }
    ecos_family_hold(true);   /* released on delete */
    lv_obj_add_event_cb(screen, list_deleted, LV_EVENT_DELETE, ui);
    ui->status = ui_text(page, "", true);
    ui->list = lv_obj_create(page);
    lv_obj_remove_style_all(ui->list);
    lv_obj_set_size(ui->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ui->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui->list, 10, 0);
    ui->shown = -1;
    ui->search = ui_button(page, "Cerca di nuovo", UI_COLOR_BLUE, search_click, ui);
    ui_row(page, LV_SYMBOL_PLUS, UI_COLOR_GREEN, "Abbina un dispositivo", NULL, apps_open_cb, (void *)"ecos.pair");
    ui_text(page, "Rispondono i dispositivi in carica a schermo spento (Watcher, Knob, un orologio sul caricatore) "
                  "o con Ecosistema aperto. Radio diretta e cifrata, senza router.", true);
    ecos_family_refresh();
    ui->timer = lv_timer_create(list_tick, 1000, ui);
    list_refresh(ui);
}

const app_t ecos_app = {
    .id = "ecos", .name = "Ecosistema", .icon = LV_SYMBOL_LOOP, .color = UI_COLOR_GREEN, .create = list_create,
    .about = "I tuoi dispositivi abbinati: chi c'è, batteria, e cosa farci (fai suonare, messaggi).",
};

/* ------------------------------------------------------------ one device */

static const char *const s_quick[] = {"Vieni qui", "A tavola!", "Arrivo", "Chiamami", "Tutto ok?"};

typedef struct
{
    char id[13];
    lv_timer_t *timer;
    lv_obj_t *info, *status, *ring, *access, *grants;
    bool ringing;
    ecos_family_t family;
} device_ui_t;

static const ecos_member_t *device_member(device_ui_t *ui)
{
    for (int i = 0; i < ui->family.count; i++)
    {
        if (strcmp(ui->family.members[i].id, ui->id) == 0)
        {
            return &ui->family.members[i];
        }
    }
    return NULL;
}

static void device_refresh(device_ui_t *ui)
{
    ecos_family(&ui->family);
    const ecos_member_t *m = device_member(ui);
    if (m == NULL)
    {
        lv_label_set_text(ui->info, "Dispositivo non più abbinato");
        return;
    }
    char ago[16], battery[24], network[64], text[320];
    ago_text(m->seen_ago, ago, sizeof(ago));
    if (m->battery >= 0)
    {
        snprintf(battery, sizeof(battery), "%d%%%s", m->battery,
                 m->charging ? ", in carica" : m->usb ? ", al cavo" : "");
    }
    else
    {
        snprintf(battery, sizeof(battery), "%s", m->known ? "alimentato" : "-");
    }
    if (m->wifi)
    {
        snprintf(network, sizeof(network), "%s\n%s", m->ssid, m->ip);
    }
    else
    {
        snprintf(network, sizeof(network), "%s", m->known ? "non collegato" : "-");
    }
    if (m->known)
    {
        snprintf(text, sizeof(text),
                 "%s\nVersione %s\nBatteria: %s\nRete: %s\nSegnale: %d dBm, canale %u\n"
                 "Acceso da %lu h, RAM libera %u KB\nUltimo contatto: %s%s",
                 m->product, m->version, battery, network, m->rssi, m->channel, (unsigned long)(m->uptime_min / 60),
                 m->ram_kb, ago, m->reachable ? "" : " (ora non risponde)");
    }
    else
    {
        snprintf(text, sizeof(text), "ID %s\nUltimo contatto: %s\nNessuna informazione ancora: deve essere in carica "
                 "a schermo spento, o con Ecosistema aperto.", m->id, ago);
    }
    lv_label_set_text_fmt(ui->grants, "Questo membro puo chiedere a me:\nCodice web: %s - Risorse: %s",
        m->permissions & ECOS_PERMISSION_WEB_CODE ? "si" : "no",
        m->permissions & ECOS_PERMISSION_RESOURCES ? "si" : "no");
    const char *result = "Richiedi informazioni al membro con i pulsanti qui sotto.";
    if (m->access_pending) result = "In attesa del membro...";
    else if (m->access_known)
    {
        if (m->access_result == 1) result = "Permesso non concesso dal membro";
        else if (m->access_result == 2) result = "Servizio non disponibile sul membro";
        else if (m->access_result == 3) result = "Nessuna risposta: offline o firmware precedente";
        else if (m->access_operation == 1)
        {
            if (m->resource_flags & 1) result = "Recovery: dashboard senza codice, accesso persistente";
            else { lv_label_set_text_fmt(ui->access, "Codice web del membro: %s", m->web_code); result = NULL; }
        }
        else
        {
            lv_label_set_text_fmt(ui->access, "Risorse di %lu s fa\nBatteria %d%% - %u mV\nRAM %u KB\n%s\n%s",
                (unsigned long)m->access_age_s, m->resource_battery, m->battery_mv, m->resource_ram_kb,
                m->resource_flags & 128 ? "Webserver disponibile" : "Webserver non attivo",
                m->resource_flags & 64 ? "Slot microSD presente" : "Senza slot microSD"); result = NULL;
        }
    }
    if (result != NULL) lv_label_set_text(ui->access, result);
    lv_label_set_text(ui->info, text);
    lv_label_set_text(ui->status, ui->family.busy ? "Invio..." : ui->family.message);
    if (ui->ringing != m->ringing)
    {
        ui->ringing = m->ringing;
        lv_label_set_text(lv_obj_get_child(ui->ring, 0), m->ringing ? "Smetti di suonare" : "Fai suonare");
    }
}

static void device_tick(lv_timer_t *timer)
{
    device_refresh(lv_timer_get_user_data(timer));
}

static void ring_click(lv_event_t *e)
{
    device_ui_t *ui = lv_event_get_user_data(e);
    bool on = !ui->ringing;
    if (ecos_family_ring(ui->id, on) != ESP_OK)
    {
        ui_toast("Attendi: un invio è in corso");
        return;
    }
    /* Optimistic: the device's answer corrects it. */
    ui->ringing = on;
    lv_label_set_text(lv_obj_get_child(ui->ring, 0), on ? "Smetti di suonare" : "Fai suonare");
}

static void quick_click(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target(e);
    device_ui_t *ui = lv_event_get_user_data(e);
    const char *text = lv_obj_get_user_data(row);
    if (ecos_family_message(ui->id, text) != ESP_OK)
    {
        ui_toast("Attendi: un invio è in corso");
    }
}

static void request_web_click(lv_event_t *e)
{
    device_ui_t *ui = lv_event_get_user_data(e);
    if (ecos_family_request(ui->id, 1) != ESP_OK) ui_toast("Attendi: richiesta in corso");
}
static void request_resources_click(lv_event_t *e)
{
    device_ui_t *ui = lv_event_get_user_data(e);
    if (ecos_family_request(ui->id, 2) != ESP_OK) ui_toast("Attendi: richiesta in corso");
}
typedef struct { char id[13]; uint32_t permissions; } grant_job_t;
static void save_grant(void *arg)
{
    grant_job_t *job = arg;
    ecos_permissions_set(job->id, job->permissions);
    free(job);
}
static void toggle_grant(lv_event_t *e, uint32_t bit)
{
    device_ui_t *ui = lv_event_get_user_data(e);
    const ecos_member_t *m = device_member(ui);
    grant_job_t *job = m != NULL ? malloc(sizeof(*job)) : NULL;
    if (job == NULL) return;
    snprintf(job->id, sizeof(job->id), "%s", ui->id);
    job->permissions = m->permissions ^ bit;
    if (!sys_post(save_grant, job)) { free(job); ui_toast("Impossibile salvare ora"); }
}
static void grant_web_click(lv_event_t *e) { toggle_grant(e, ECOS_PERMISSION_WEB_CODE); }
static void grant_resources_click(lv_event_t *e) { toggle_grant(e, ECOS_PERMISSION_RESOURCES); }

static void forget_answer(bool confirmed, void *user_data)
{
    char *id = user_data;
    if (confirmed)
    {
        if (ecos_forget(id) == ESP_OK)
        {
            ui_toast("Dimenticato");
            app_back();
        }
        else
        {
            ui_toast("Non ora: abbinamento in corso");
        }
    }
    free(id);
}

static void forget_click(lv_event_t *e)
{
    device_ui_t *ui = lv_event_get_user_data(e);
    char *id = malloc(13);
    if (id != NULL)
    {
        snprintf(id, 13, "%s", ui->id);
        ui_confirm("Dimenticare?", "Per riabbinarlo servirà di nuovo il codice. Fallo anche sull'altro dispositivo.",
                   "Dimentica", UI_COLOR_RED, forget_answer, id);
    }
}

static void device_deleted(lv_event_t *e)
{
    device_ui_t *ui = lv_event_get_user_data(e);
    if (ui->timer != NULL)
    {
        lv_timer_delete(ui->timer);
    }
    free(ui);
}

static void device_create(lv_obj_t *screen, void *arg)
{
    device_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    if (ui != NULL && arg != NULL)
    {
        snprintf(ui->id, sizeof(ui->id), "%s", (const char *)arg);
    }
    free(arg);
    if (ui == NULL)
    {
        ui_text(ui_page(screen, "Ecosistema"), "Memoria insufficiente", true);
        return;
    }
    lv_obj_add_event_cb(screen, device_deleted, LV_EVENT_DELETE, ui);
    ecos_family(&ui->family);
    const ecos_member_t *m = device_member(ui);
    lv_obj_t *page = ui_page(screen, m != NULL ? m->name : "Dispositivo");
    ui->info = ui_text(page, "", false);
    ui->status = ui_text(page, "", true);
    ui_section(page, "Usa le risorse del membro");
    ui->access = ui_text(page, "", true);
    ui_button(page, "Richiedi codice web", UI_COLOR_BLUE, request_web_click, ui);
    ui_button(page, "Leggi risorse", UI_COLOR_BLUE, request_resources_click, ui);
    ui_section(page, "Permessi concessi al membro");
    ui->grants = ui_text(page, "", true);
    ui_text(page, "Disattivare la richiesta codici non revoca accessi web gia abbinati.", true);
    ui_button(page, "Cambia permesso codice web", UI_COLOR_ORANGE, grant_web_click, ui);
    ui_button(page, "Cambia permesso risorse", UI_COLOR_ORANGE, grant_resources_click, ui);
    ui->ring = ui_button(page, "Fai suonare", UI_COLOR_ORANGE, ring_click, ui);
    ui_section(page, "Messaggio rapido");
    for (size_t i = 0; i < sizeof(s_quick) / sizeof(s_quick[0]); i++)
    {
        lv_obj_t *row = ui_row(page, LV_SYMBOL_ENVELOPE, UI_COLOR_BLUE, s_quick[i], NULL, quick_click, ui);
        lv_obj_set_user_data(row, (void *)s_quick[i]);
    }
    ui_button(page, "Dimentica", UI_COLOR_RED, forget_click, ui);
    ui->timer = lv_timer_create(device_tick, 1000, ui);
    device_refresh(ui);
}

const app_t ecos_device_app = {
    .id = "ecos.device", .name = "Dispositivo", .icon = LV_SYMBOL_LOOP, .color = UI_COLOR_GREEN,
    .flags = APP_FLAG_HIDDEN, .create = device_create,
};

/* ------------------------------------------------------------ pairing */

typedef struct
{
    lv_timer_t *timer;
    lv_obj_t *status, *candidate, *code, *hint, *start, *confirm, *cancel;
    ecos_snapshot_t seen;
} pair_ui_t;

static void pair_refresh(lv_timer_t *timer)
{
    pair_ui_t *ui = lv_timer_get_user_data(timer);
    ecos_snapshot(&ui->seen);
    const ecos_snapshot_t *s = &ui->seen;
    if (s->running)
    {
        lv_label_set_text_fmt(ui->status, "%s\n%u secondi", s->message, s->seconds_left);
    }
    else
    {
        lv_label_set_text(ui->status, s->message);
    }
    lv_label_set_text_fmt(ui->candidate, "%s%s%s", s->candidate, s->candidate[0] ? "\n" : "", s->candidate_id);
    if (s->comparing)
    {
        lv_label_set_text_fmt(ui->code, "%06lu", (unsigned long)s->code);
    }
    else
    {
        lv_label_set_text(ui->code, "");
    }
    lv_obj_set_state(ui->start, LV_STATE_DISABLED, s->running);
    lv_obj_set_state(ui->confirm, LV_STATE_DISABLED, !s->comparing || s->local_ok);
    lv_obj_set_state(ui->cancel, LV_STATE_DISABLED, !s->running);
    lv_obj_set_hidden(ui->candidate, !s->comparing);
    lv_obj_set_hidden(ui->code, !s->comparing);
    lv_obj_set_hidden(ui->hint, s->running);
    lv_obj_set_hidden(ui->start, s->running);
    lv_obj_set_hidden(ui->confirm, !s->comparing || s->local_ok);
    lv_obj_set_hidden(ui->cancel, !s->running);
}

static void pair_start(lv_event_t *e)
{
    (void)e;
    esp_err_t err = ecos_pair_start(NULL);
    if (err != ESP_OK)
    {
        ui_toast(err == ESP_ERR_INVALID_STATE ? "Abbinamento già attivo" : "Memoria insufficiente");
    }
}

static void pair_confirm(lv_event_t *e)
{
    pair_ui_t *ui = lv_event_get_user_data(e);
    if (ecos_pair_confirm(ui->seen.candidate_id, ui->seen.code) != ESP_OK)
    {
        ui_toast("Sessione cambiata: confronta di nuovo il codice");
    }
}

static void pair_cancel(lv_event_t *e)
{
    (void)e;
    ecos_pair_cancel();
}

static void pair_deleted(lv_event_t *e)
{
    pair_ui_t *ui = lv_event_get_user_data(e);
    if (ui->timer != NULL)
    {
        lv_timer_delete(ui->timer);
    }
    free(ui);
}

static void pair_leave(void)
{
    ecos_pair_cancel();
}

static void pair_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_t *page = ui_page(screen, "Abbinamento");
    pair_ui_t *ui = calloc(1, sizeof(*ui));
    if (ui == NULL)
    {
        ui_text(page, "Memoria insufficiente", true);
        return;
    }
    lv_obj_add_event_cb(screen, pair_deleted, LV_EVENT_DELETE, ui);
    ui->status = ui_text(page, "", true);
    ui->candidate = ui_text(page, "", true);
    ui->code = ui_text(page, "", false);
    lv_obj_set_style_text_font(ui->code, ui_font_title, 0);
    ui->hint = ui_text(page, "Avvia su due dispositivi vicini: si parlano direttamente (ESP-NOW), senza router né "
                             "internet. Conferma solo se i codici coincidono.", true);
    ui->start = ui_button(page, "Avvia abbinamento", UI_COLOR_GREEN, pair_start, ui);
    ui->confirm = ui_button(page, "I codici coincidono", UI_COLOR_BLUE, pair_confirm, ui);
    ui->cancel = ui_button(page, "Annulla", UI_COLOR_RED, pair_cancel, ui);
    ui_text(page, "Finestra di 3 minuti. Uscendo si annulla.", true);
    ui->timer = lv_timer_create(pair_refresh, 500, ui);
    if (ui->timer != NULL)
    {
        pair_refresh(ui->timer);
    }
}

const app_t ecos_pair_app = {
    .id = "ecos.pair", .name = "Abbinamento", .icon = LV_SYMBOL_LOOP, .color = UI_COLOR_GREEN,
    .flags = APP_FLAG_HIDDEN, .create = pair_create, .pause = pair_leave, .destroy = pair_leave,
};

/* ------------------------------------------------------------ rung by another device */

static bool s_find_sound;            /* the melody was started by this page */
static lv_timer_t *s_find_timer;

static void find_found(lv_event_t *e)
{
    (void)e;
    if (app_current() == &ecos_find_app)
    {
        app_back();
    }
}

static void find_timeout(lv_timer_t *timer)
{
    (void)timer;
    s_find_timer = NULL;   /* repeat count 1: LVGL deletes it */
    if (app_current() == &ecos_find_app)
    {
        app_back();
    }
}

static void find_create(lv_obj_t *screen, void *arg)
{
    char from[ECOS_NAME_SIZE] = "Un dispositivo";
    if (arg != NULL)
    {
        snprintf(from, sizeof(from), "%s", (const char *)arg);
        free(arg);
    }
    lv_obj_t *page = ui_page(screen, NULL);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_icon_bubble(page, LV_SYMBOL_BELL, UI_COLOR_ORANGE, 72);
    lv_obj_t *title = ui_text(page, from, false);
    lv_obj_set_style_text_font(title, ui_font_title, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *text = ui_text(page, "ti sta cercando", true);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    ui_button(page, "Trovato!", UI_COLOR_GREEN, find_found, NULL);
    ecos_family_hold(true);   /* hear "stop" from the other device */
    s_find_timer = lv_timer_create(find_timeout, FIND_SECONDS * 1000, NULL);
    if (s_find_timer != NULL)
    {
        lv_timer_set_repeat_count(s_find_timer, 1);
        s_find_sound = !sound_alarm_playing();   /* never stop a real alarm */
        if (s_find_sound)
        {
            sound_alarm_start();
        }
    }
    else
    {
        /* Never start a melody without its bounded lifetime. */
        ecos_family_ring_done();
    }
}

/* Stop our melody before another app (especially a real alarm) takes over. */
static void find_pause(void)
{
    if (s_find_sound)
    {
        sound_alarm_stop();
        s_find_sound = false;
    }
    ecos_family_ring_done();
}

static void find_destroy(void)
{
    if (s_find_timer != NULL)
    {
        lv_timer_delete(s_find_timer);
        s_find_timer = NULL;
    }
    find_pause();
    ecos_family_hold(false);
}

const app_t ecos_find_app = {
    .id = "ecos.find", .name = "Trova", .icon = LV_SYMBOL_BELL, .color = UI_COLOR_ORANGE,
    .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON | APP_FLAG_STAY_ON_WAKE, .create = find_create,
    .pause = find_pause, .destroy = find_destroy,
};

static void open_find(void *arg)
{
    if (app_current() == &ring_app)
    {
        free(arg);
        ecos_family_ring_done();
        return;
    }
    if (app_current() == &ecos_find_app)
    {
        free(arg);   /* already ringing */
        return;
    }
    power_wake(POWER_WAKE_ALARM);
    if (!app_open_app(&ecos_find_app, arg))
    {
        free(arg);
        ecos_family_ring_done();
    }
}

static void close_find(void *arg)
{
    (void)arg;
    if (app_current() == &ecos_find_app)
    {
        app_back();
    }
}

/* Link task: only post to the lvgl task. */
static void on_family_event(const ecos_event_t *event)
{
    if (event->kind == ECOS_EVENT_RING)
    {
        char *from = malloc(ECOS_NAME_SIZE);
        if (from == NULL)
        {
            return;
        }
        snprintf(from, ECOS_NAME_SIZE, "%s", event->from);
        if (!ui_async(open_find, from))
        {
            free(from);
        }
    }
    else
    {
        ui_async(close_find, NULL);
    }
}

void ecos_apps_init(void)
{
    ecos_family_set_handler(on_family_event);
}
