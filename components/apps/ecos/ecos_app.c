#include "apps_internal.h"
#include "services/ecos.h"
#include "ui/ui.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct
{
    lv_timer_t *timer;
    lv_obj_t *status, *candidate, *code, *peers, *hint, *start, *confirm, *cancel;
    ecos_snapshot_t seen;
} ecos_ui_t;

static void refresh(lv_timer_t *timer)
{
    ecos_ui_t *ui = lv_timer_get_user_data(timer);
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
    lv_label_set_text_fmt(ui->candidate, "%s%s%s", s->candidate,
                          s->candidate[0] ? "\n" : "", s->candidate_id);
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
    char peers[ECOS_PEERS_MAX * (ECOS_NAME_SIZE + 16) + 32];
    size_t used = (size_t)snprintf(peers, sizeof(peers), "Dispositivi salvati: %d", s->count);
    for (int i = 0; i < s->count && used < sizeof(peers); i++)
    {
        used += (size_t)snprintf(peers + used, sizeof(peers) - used, "\n%s · %s", s->peers[i].name, s->peers[i].id);
    }
    lv_label_set_text(ui->peers, peers);
}

static void start(lv_event_t *e)
{
    (void)e;
    esp_err_t err = ecos_pair_start(NULL);
    if (err != ESP_OK)
    {
        ui_toast(err == ESP_ERR_INVALID_STATE ? "Abbinamento già attivo" : "Memoria insufficiente");
    }
}

static void confirm(lv_event_t *e)
{
    ecos_ui_t *ui = lv_event_get_user_data(e);
    if (ecos_pair_confirm(ui->seen.candidate_id, ui->seen.code) != ESP_OK)
    {
        ui_toast("Sessione cambiata: confronta di nuovo il codice");
    }
}

static void cancel(lv_event_t *e)
{
    (void)e;
    ecos_pair_cancel();
}

static void deleted(lv_event_t *e)
{
    ecos_ui_t *ui = lv_event_get_user_data(e);
    if (ui->timer != NULL)
    {
        lv_timer_delete(ui->timer);
    }
    free(ui);
}

static void pause(void)
{
    ecos_pair_cancel();
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_t *page = ui_page(screen, "Ecosistema");
    ecos_ui_t *ui = calloc(1, sizeof(*ui));
    if (ui == NULL)
    {
        ui_text(page, "Memoria insufficiente", true);
        return;
    }
    lv_obj_add_event_cb(screen, deleted, LV_EVENT_DELETE, ui);
    ui->status = ui_text(page, "", true);
    ui->candidate = ui_text(page, "", true);
    ui->code = ui_text(page, "", false);
    lv_obj_set_style_text_font(ui->code, ui_font_title, 0);
    ui->hint = ui_text(page, "Avvia su due dispositivi vicini: si parlano direttamente (ESP-NOW), senza router né internet. Conferma solo se i codici coincidono.", true);
    ui->start = ui_button(page, "Avvia abbinamento", UI_COLOR_GREEN, start, ui);
    ui->confirm = ui_button(page, "I codici coincidono", UI_COLOR_BLUE, confirm, ui);
    ui->cancel = ui_button(page, "Annulla", UI_COLOR_RED, cancel, ui);
    ui->peers = ui_text(page, "", true);
    ui_text(page, "Finestra di 3 minuti. Uscendo si annulla. Il collegamento Remote resta separato.", true);
    ui->timer = lv_timer_create(refresh, 500, ui);
    if (ui->timer != NULL)
    {
        refresh(ui->timer);
    }
    else
    {
        lv_obj_set_state(ui->start, LV_STATE_DISABLED, true);
        lv_obj_set_state(ui->confirm, LV_STATE_DISABLED, true);
        lv_obj_set_state(ui->cancel, LV_STATE_DISABLED, true);
        lv_label_set_text(ui->status, "Memoria insufficiente");
    }
}

const app_t ecos_app = {
    .id = "ecos", .name = "Ecosistema", .icon = LV_SYMBOL_LOOP,
    .color = UI_COLOR_GREEN, .create = create, .pause = pause, .destroy = pause,
};
