#include "apps_internal.h"

#include "core/lv_port.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/sys.h"
#include "services/fw_update.h"
#include "services/fw_version.h"
#include "ui/ui.h"

#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Settings > Aggiornamento: version, channel, update from the PC (web page)
 * or online, and "Tutte le versioni": every release online plus the
 * firmwares saved on the SD card, to install any of them (older ones too).
 *
 * The screens only observe STATE_UPDATE: transfers run in the web server or
 * in fw_update tasks, never here. While writing, apps.c covers everything
 * with a still screen (no progress bar: see on_update_phase there).
 */

typedef struct
{
    lv_obj_t *status;
    lv_obj_t *install_row;
} update_ui_t;

static update_ui_t *s_ui = NULL;

static const char *channel_name(uint8_t channel)
{
    return channel == FW_CHANNEL_DEV ? "Dev" : "Stabile";
}

static void on_phase(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;

    if (s_ui == NULL)
    {
        return;
    }

    fw_update_phase_t phase = (fw_update_phase_t)lv_subject_get_int(subject);
    const char *message = fw_update_message();
    uint32_t color = UI_COLOR_TEXT_DIM;

    lv_obj_set_hidden(s_ui->install_row, phase != FW_UPDATE_AVAILABLE);

    switch (phase)
    {
        case FW_UPDATE_CHECKING:
            lv_label_set_text(s_ui->status, "Controllo in corso...");
            break;
        case FW_UPDATE_UP_TO_DATE:
            lv_label_set_text(s_ui->status, "Il firmware è aggiornato.");
            color = UI_COLOR_GREEN;
            break;
        case FW_UPDATE_AVAILABLE:
            lv_label_set_text_fmt(s_ui->status, "Disponibile la versione %s.", fw_update_new_version());
            color = UI_COLOR_BLUE;
            lv_label_set_text_fmt(ui_row_value(s_ui->install_row), "%s", fw_update_new_version());
            break;
        case FW_UPDATE_WRITING:
            lv_label_set_text(s_ui->status, "Installazione: non spegnere l'orologio.");
            color = UI_COLOR_ORANGE;
            break;
        case FW_UPDATE_RESTARTING:
            lv_label_set_text(s_ui->status, "Verificato. Riavvio...");
            color = UI_COLOR_GREEN;
            break;
        case FW_UPDATE_FAILED:
            lv_label_set_text(s_ui->status, message[0] != '\0' ? message : "Aggiornamento non riuscito.");
            color = UI_COLOR_RED;
            break;
        default:
            lv_label_set_text(s_ui->status, "");
            break;
    }

    lv_obj_set_style_text_color(s_ui->status, ui_color(color), 0);
}

static void check(lv_event_t *e)
{
    (void)e;
    fw_update_check_online(false);
}

static void install_confirmed(bool confirmed, void *user_data)
{
    (void)user_data;

    if (confirmed)
    {
        fw_update_check_online(true);
    }
}

static void install(lv_event_t *e)
{
    (void)e;
    ui_confirm("Aggiorna", "Il firmware viene scaricato, verificato e installato; poi l'orologio si riavvia.",
               "Installa", UI_COLOR_BLUE, install_confirmed, NULL);
}

static void cycle_channel(lv_event_t *e)
{
    settings_t settings = *settings_get();
    settings.update_channel = settings.update_channel == FW_CHANNEL_DEV ? FW_CHANNEL_STABLE : FW_CHANNEL_DEV;
    settings_save(&settings);

    lv_label_set_text(ui_row_value(lv_event_get_current_target_obj(e)), channel_name(settings.update_channel));
    ui_toast(settings.update_channel == FW_CHANNEL_DEV ? "Canale dev: versioni di prova, anche instabili"
                                                       : "Canale stabile");
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    s_ui = calloc(1, sizeof(update_ui_t));

    if (s_ui == NULL)
    {
        return;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    lv_obj_t *page = ui_page(screen, "Aggiornamento");

    lv_obj_t *version = ui_text(page, "", false);
    lv_label_set_text_fmt(version, "Versione %s  ·  %s", fw_update_running_version(),
                          running != NULL ? running->label : "?");

    s_ui->status = ui_text(page, "", false);

    ui_section(page, "ONLINE");
    ui_row(page, LV_SYMBOL_SHUFFLE, UI_COLOR_ORANGE, "Canale", channel_name(settings_get()->update_channel),
           cycle_channel, NULL);
    ui_row(page, LV_SYMBOL_REFRESH, UI_COLOR_TEAL, "Controlla aggiornamenti", NULL, check, NULL);
    s_ui->install_row = ui_row(page, LV_SYMBOL_DOWNLOAD, UI_COLOR_GREEN, "Installa", "", install, NULL);
    ui_row(page, LV_SYMBOL_LIST, UI_COLOR_INDIGO, "Tutte le versioni", NULL, apps_open_cb,
           (void *)"settings.update.versions");
    ui_text(page, fw_update_manifest_url()[0] != '\0'
                      ? "Le versioni arrivano dalle release ufficiali; prima di ogni aggiornamento il firmware "
                        "in uso viene salvato sulla scheda SD (se presente)."
                      : "L'indirizzo del file di versione si imposta dalla pagina web.", true);

    ui_section(page, "DAL COMPUTER");
    ui_text(page, "Apri la pagina web dell'orologio, sezione \"Aggiornamento firmware\", e scegli "
                  "il file .bin del firmware (cartella build).", true);
    ui_row(page, LV_SYMBOL_UPLOAD, UI_COLOR_BLUE, "Apri pagina web", NULL, apps_open_cb, (void *)"web");

    /* Observers die with their objects: nothing to remove by hand. */
    lv_subject_add_observer_obj(state_subject(STATE_UPDATE), on_phase, s_ui->status, NULL);
}

static void destroy(void)
{
    free(s_ui);
    s_ui = NULL;
}

const app_t update_app = {
    .id = "settings.update",
    .name = "Aggiornamento",
    .icon = LV_SYMBOL_DOWNLOAD,
    .color = UI_COLOR_GREEN,
    .flags = APP_FLAG_HIDDEN | APP_FLAG_STAY_ON_WAKE,
    .create = create,
    .destroy = destroy,
};

/* ========================================================= every version */

/*
 * Lists come from a task (network + SD card, seconds); the page shows
 * "loading" meanwhile. A list arriving after the page was closed is just
 * freed (generation counter).
 */

typedef struct
{
    int generation;
    esp_err_t online_err;
    int online_count;
    int local_count;
    fw_release_t online[FW_RELEASES_MAX];
    fw_local_t local[FW_LOCAL_MAX];
} versions_t;

static lv_obj_t *s_list = NULL;
static versions_t *s_versions = NULL;
static volatile int s_generation = 0;

static void install_release_confirmed(bool confirmed, void *user_data)
{
    int index = (int)(intptr_t)user_data;

    if (confirmed && s_versions != NULL && index >= 0 && index < s_versions->online_count)
    {
        fw_update_install_release(&s_versions->online[index]);
    }
}

static void install_file_confirmed(bool confirmed, void *user_data)
{
    int index = (int)(intptr_t)user_data;

    if (confirmed && s_versions != NULL && index >= 0 && index < s_versions->local_count)
    {
        fw_update_install_file(s_versions->local[index].path);
    }
}

/* "Installare 1.2.0?" - with a warning when it goes back in time. */
static void confirm_version(const char *version, const char *where, ui_confirm_cb_t cb, intptr_t index)
{
    static char text[160];
    int order = fw_version_compare(version, fw_update_running_version());

    if (order == 0)
    {
        snprintf(text, sizeof(text), "È la versione in uso: viene reinstallata (%s).", where);
    }
    else if (order < 0)
    {
        snprintf(text, sizeof(text), "È più vecchia della %s in uso. Le impostazioni restano. (%s)",
                 fw_update_running_version(), where);
    }
    else
    {
        snprintf(text, sizeof(text), "Viene installata e l'orologio si riavvia. (%s)", where);
    }

    static char title[48];
    snprintf(title, sizeof(title), "Installare la %s?", version);
    ui_confirm(title, text, "Installa", order < 0 ? UI_COLOR_ORANGE : UI_COLOR_BLUE, cb, (void *)index);
}

static void on_online(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    if (s_versions != NULL && index < s_versions->online_count)
    {
        confirm_version(s_versions->online[index].version, "da internet", install_release_confirmed, index);
    }
}

static void on_local(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    if (s_versions != NULL && index < s_versions->local_count)
    {
        confirm_version(s_versions->local[index].version, "dalla scheda SD", install_file_confirmed, index);
    }
}

static void show_versions(void *arg)
{
    versions_t *v = arg;

    if (v->generation != s_generation || s_list == NULL)
    {
        heap_caps_free(v);   /* the page was closed meanwhile */
        return;
    }

    s_versions = v;

    while (lv_obj_get_child_count(s_list) > 1)   /* keep the title */
    {
        lv_obj_delete(lv_obj_get_child(s_list, 1));
    }

    const char *running = fw_update_running_version();
    char value[40];

    ui_section(s_list, "ONLINE");

    if (v->online_err != ESP_OK)
    {
        ui_text(s_list, v->online_err == ESP_ERR_TIMEOUT ? "Wi-Fi non disponibile." : "Elenco non leggibile.", true);
    }
    else if (v->online_count == 0)
    {
        ui_text(s_list, "Nessuna release pubblicata.", true);
    }

    for (int i = 0; i < v->online_count; i++)
    {
        const fw_release_t *r = &v->online[i];
        bool dev = strcmp(r->channel, "dev") == 0;
        bool current = strcmp(r->version, running) == 0;

        snprintf(value, sizeof(value), "%s%s", current ? "in uso · " : "", r->date[0] ? r->date : r->channel);
        ui_row(s_list, dev ? LV_SYMBOL_WARNING : LV_SYMBOL_OK, dev ? UI_COLOR_ORANGE : UI_COLOR_GREEN, r->version,
               value, on_online, (void *)(intptr_t)i);
    }

    ui_section(s_list, "SULLA SCHEDA SD");

    if (v->local_count == 0)
    {
        ui_text(s_list, "Nessun firmware salvato (cartella /firmware).", true);
    }

    for (int i = 0; i < v->local_count; i++)
    {
        bool current = strcmp(v->local[i].version, running) == 0;
        ui_row(s_list, LV_SYMBOL_SD_CARD, UI_COLOR_TEAL, v->local[i].version, current ? "in uso" : "",
               on_local, (void *)(intptr_t)i);
    }

    ui_text(s_list, "Arancione = canale dev (versioni di prova).", true);
}

static void versions_task(void *arg)
{
    versions_t *v = arg;

    v->online_err = fw_update_fetch_releases(v->online, FW_RELEASES_MAX, &v->online_count);
    v->local_count = fw_update_list_sd(v->local, FW_LOCAL_MAX);

    if (!ui_async(show_versions, v))
    {
        heap_caps_free(v);
    }

    vTaskDelete(NULL);
}

static void versions_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    s_list = ui_page(screen, "Versioni");
    ui_text(s_list, "Caricamento...", true);

    versions_t *v = heap_caps_calloc(1, sizeof(versions_t), MALLOC_CAP_SPIRAM);

    if (v == NULL)
    {
        ui_text(s_list, "Memoria insufficiente.", true);
        return;
    }

    v->generation = ++s_generation;

    /* TLS needs a deep stack. */
    if (!sys_task_create(versions_task, "fw_versions", 8192, v, 3, SYS_CORE_ANY))
    {
        heap_caps_free(v);
        ui_text(s_list, "Memoria insufficiente.", true);
    }
}

static void versions_destroy(void)
{
    s_generation++;
    s_list = NULL;

    if (s_versions != NULL)
    {
        heap_caps_free(s_versions);
        s_versions = NULL;
    }
}

const app_t versions_app = {
    .id = "settings.update.versions",
    .name = "Versioni",
    .icon = LV_SYMBOL_LIST,
    .color = UI_COLOR_INDIGO,
    .flags = APP_FLAG_HIDDEN,
    .create = versions_create,
    .destroy = versions_destroy,
};
