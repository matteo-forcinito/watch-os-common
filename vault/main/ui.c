/* A fullscreen safe as the entrance; account controls live one gesture inside. */
#include "vault.h"
#include "ui/ui.h"
#include "core/app.h"
#include "core/clock.h"
#include "core/power.h"
#include "core/lv_port.h"
#include "hardware/board.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

#define GOLD 0xE6BC73
#define TEAL 0x72DEC3
typedef struct
{
    lv_obj_t *screen, *content, *border, *status_label, *pin_label, *otp_label, *otp_expiry;
    lv_timer_t *timer;
    vault_status_t status;
    unsigned page, field, offset, selected, edit_index, edit_field;
    bool grouped, was_open, revealing, pending_save, editing_preview;
    char value[257], id[33], creation[129], preview[33];
    uint64_t edit_generation;
    vault_entry_t draft;
    vault_resolution_t choices[VAULT_ENTRIES];
    int64_t reveal_until;
    char filename[97];
    unsigned selection_count;
    char selection[VAULT_ENTRIES][33];
    time_t otp_counter;
    unsigned otp_period;
} vault_ui_t;
static lv_obj_t *vault_text(lv_obj_t *page, const char *text)
{
    return ui_text(page, text, true);
}

static vault_ui_t *s_ui;
static void render(void);
static void action(lv_event_t *event);
static void input_field(const char *text, void *unused);

static void notify(esp_err_t result)
{
    if (result != ESP_OK)
        ui_toast("Attendi, sblocca o riprova");
}

static vault_job_t *new_job(vault_operation_t operation)
{
    vault_job_t *job = vault_secure_alloc(sizeof(*job));
    if (job != NULL)
    {
        memset(job, 0, sizeof(*job));
        job->operation = operation;
    }
    return job;
}

static esp_err_t submit(vault_job_t *job)
{
    if (job == NULL)
    {
        ui_toast("Memoria insufficiente");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = vault_submit(job);
    if (result != ESP_OK)
        vault_job_free(job);
    notify(result);
    return result;
}

static void row(lv_obj_t *page, const char *text, const char *value, unsigned code)
{
    ui_row(page, LV_SYMBOL_RIGHT, GOLD, text, value, action, (void *)(uintptr_t)code);
}

static void confirmed_create(const char *text, void *unused)
{
    (void)unused;
    if (text == NULL || strlen(text) != strlen(s_ui->creation) ||
        !vault_equal(text, s_ui->creation, strlen(s_ui->creation) + 1))
    {
        vault_wipe(s_ui->creation, sizeof(s_ui->creation));
        ui_toast("Conferma non corrispondente");
        return;
    }
    vault_job_t *job = new_job(VAULT_CREATE);
    if (job != NULL)
        memcpy(job->password, s_ui->creation, strlen(s_ui->creation) + 1);
    vault_wipe(s_ui->creation, sizeof(s_ui->creation));
    submit(job);
}

static void unlock_input(const char *text, void *unused)
{
    (void)unused;
    if (text == NULL)
        return;
    if (!vault_text_valid(text, 129, false) || (!s_ui->status.exists && strlen(text) < 12))
    {
        ui_toast("Password principale: 12–128 byte");
        return;
    }
    if (!s_ui->status.exists)
    {
        memcpy(s_ui->creation, text, strlen(text) + 1);
        vault_input("Conferma password", "", true, confirmed_create, NULL);
        return;
    }
    vault_job_t *job = new_job(VAULT_UNLOCK);
    if (job != NULL)
        memcpy(job->password, text, strlen(text) + 1);
    submit(job);
}

static void hero_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_GESTURE)
    {
        lv_indev_t *indev = lv_indev_active();
        if (indev == NULL || lv_indev_get_gesture_dir(indev) != LV_DIR_TOP)
            return;
        lv_indev_wait_release(indev);
    }
    if (s_ui->status.busy || !s_ui->status.storage)
        return;
    if (vault_unlocked())
    {
        s_ui->page = 1;
        vault_activity();
        render();
    }
    else if (s_ui->status.cooldown)
        ui_toast("Attendi prima di riprovare");
    else
        vault_input(s_ui->status.exists ? "Password principale" : "Crea password principale", "",
                    true, unlock_input, NULL);
}

static bool selected(vault_entry_t *entry, unsigned *index)
{
    return vault_find_entry(s_ui->id, entry, index);
}

static void type_confirmed(bool confirmed, void *argument)
{
    if (!confirmed || !vault_unlocked())
        return;
    vault_job_t *job = new_job(VAULT_TYPE);
    if (job == NULL)
        return;
    job->field = (unsigned)(uintptr_t)argument;
    if (!selected(&job->entry, &job->index))
    {
        vault_job_free(job);
        return;
    }
    submit(job);
}

static void delete_confirmed(bool confirmed, void *unused)
{
    (void)unused;
    if (!confirmed || !vault_unlocked())
        return;
    vault_job_t *job = new_job(VAULT_DELETE);
    if (job == NULL)
        return;
    if (!selected(&job->entry, &job->index))
    {
        vault_job_free(job);
        return;
    }
    job->expected_generation = s_ui->status.generation;
    submit(job);
    s_ui->page = 2;
    render();
}

static char *edit_pointer(unsigned field, size_t *capacity)
{
    vault_entry_t *entry = &s_ui->draft;
    if (field == 0)
    {
        *capacity = sizeof(entry->name);
        return entry->name;
    }
    if (field == 1)
    {
        *capacity = sizeof(entry->email);
        return entry->email;
    }
    if (field == 2)
    {
        *capacity = sizeof(entry->username);
        return entry->username;
    }
    if (field == 3)
    {
        *capacity = sizeof(entry->password);
        return entry->password;
    }
    if (field == 4)
    {
        *capacity = sizeof(entry->website);
        return entry->website;
    }
    if (field == 5)
    {
        *capacity = sizeof(entry->note);
        return entry->note;
    }
    *capacity = sizeof(entry->otp);
    return entry->otp;
}

static void input_field(const char *text, void *unused)
{
    (void)unused;
    if (text == NULL || !vault_unlocked())
        return;
    size_t capacity;
    char *field = edit_pointer(s_ui->edit_field, &capacity);
    if (!vault_text_valid(text, capacity, s_ui->edit_field != 0))
    {
        ui_toast("Campo troppo lungo o non valido");
        return;
    }
    vault_wipe(field, capacity);
    memcpy(field, text, strlen(text) + 1);
    vault_activity();
    render();
}

static void export_selection(unsigned mode)
{
    vault_job_t *job = new_job(VAULT_EXPORT);
    if (job == NULL)
        return;
    job->selection = vault_secure_alloc(sizeof(*job->selection));
    if (job->selection == NULL)
    {
        vault_job_free(job);
        return;
    }
    memset(job->selection, 0, sizeof(*job->selection));
    if (mode == 1)
    {
        job->selection->id_count = 1;
        memcpy(job->selection->ids[0], s_ui->id, 33);
    }
    else if (mode == 2)
    {
        if (!s_ui->selection_count)
        {
            vault_job_free(job);
            ui_toast("Seleziona almeno una voce");
            return;
        }
        job->selection->id_count = s_ui->selection_count;
        memcpy(job->selection->ids, s_ui->selection, s_ui->selection_count * 33);
    }
    else if (s_ui->grouped)
    {
        job->selection->grouped = true;
        job->selection->field = s_ui->field;
        memcpy(job->selection->value, s_ui->value, sizeof(s_ui->value));
    }
    submit(job);
    ui_toast("Backup cifrato in vault/exports");
}

static void import_confirmed(bool confirmed, void *unused)
{
    (void)unused;
    if (!confirmed || !vault_unlocked())
        return;
    vault_job_t *job = new_job(VAULT_IMPORT);
    if (job == NULL)
        return;
    job->choices = vault_secure_alloc(sizeof(s_ui->choices));
    if (job->choices == NULL)
    {
        vault_job_free(job);
        return;
    }
    memcpy(job->choices, s_ui->choices, sizeof(s_ui->choices));
    memcpy(job->preview, s_ui->preview, 33);
    job->enabled = true;
    submit(job);
}

static void import_password(const char *text, void *unused)
{
    (void)unused;
    if (text == NULL || !vault_unlocked() || !vault_text_valid(text, 129, false))
        return;
    vault_job_t *job = new_job(VAULT_IMPORT_FILE);
    if (job == NULL)
        return;
    memcpy(job->filename, s_ui->filename, sizeof(job->filename));
    memcpy(job->password, text, strlen(text) + 1);
    if (submit(job) == ESP_OK)
    {
        s_ui->page = 7;
        s_ui->offset = 0;
        render();
    }
}

static void import_name(const char *text, void *unused)
{
    (void)unused;
    if (text == NULL || !vault_unlocked() || !vault_text_valid(text, sizeof(s_ui->filename), false))
        return;
    memcpy(s_ui->filename, text, strlen(text) + 1);
    vault_input("Password del backup", "", true, import_password, NULL);
}

static void interface_confirmed(bool confirmed, void *argument)
{
    if (!confirmed || !vault_unlocked())
        return;
    vault_job_t *job = new_job(VAULT_INTERFACE);
    if (job == NULL)
        return;
    unsigned code = (unsigned)(uintptr_t)argument;
    job->index = code == 27 ? HID_LINK_BLE : code == 28 ? HID_LINK_USB : hid_link_transport();
    job->field = code == 29 ? HID_HOST_IT : code == 30 ? HID_HOST_US : hid_link_host_layout();
    submit(job);
}

static void action(lv_event_t *event)
{
    if (s_ui == NULL || !vault_unlocked())
        return;
    vault_activity();
    unsigned code = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (code == 1)
    {
        s_ui->page = 2;
        s_ui->field = 0;
        s_ui->grouped = false;
        s_ui->offset = 0;
    }
    else if (code >= 2 && code <= 4)
    {
        s_ui->page = 3;
        s_ui->field = code - 1;
        s_ui->grouped = false;
        s_ui->offset = 0;
    }
    else if (code == 5)
    {
        vault_wipe(&s_ui->draft, sizeof(s_ui->draft));
        s_ui->draft.otp_digits = 6;
        s_ui->draft.otp_period = 30;
        s_ui->draft.otp_hash = 1;
        snprintf(s_ui->draft.name, sizeof(s_ui->draft.name), "Nuova credenziale");
        uint8_t id[16];
        if (vault_random(id, 16) != ESP_OK)
            return;
        for (unsigned i = 0; i < 16; i++)
            snprintf(s_ui->draft.id + 2 * i, 3, "%02x", id[i]);
        vault_wipe(id, 16);
        s_ui->edit_index = s_ui->status.count;
        s_ui->edit_generation = s_ui->status.generation;
        s_ui->editing_preview = false;
        s_ui->page = 5;
    }
    else if (code == 6)
        s_ui->page = 6;
    else if (code == 7)
    {
        vault_lock();
        return;
    }
    else if (code == 8)
    {
        s_ui->page = 1;
    }
    else if (code == 9)
    {
        if (s_ui->offset >= 8)
            s_ui->offset -= 8;
    }
    else if (code == 10)
        s_ui->offset += 8;
    else if (code == 11)
    {
        export_selection(false);
        return;
    }
    else if (code == 12)
    {
        export_selection(true);
        return;
    }
    else if (code == 13)
    {
        if (!selected(&s_ui->draft, &s_ui->edit_index))
            return;
        s_ui->edit_generation = s_ui->status.generation;
        s_ui->editing_preview = false;
        s_ui->page = 5;
    }
    else if (code == 14)
    {
        s_ui->revealing = !s_ui->revealing;
        s_ui->reveal_until = esp_timer_get_time() + 10000000;
    }
    else if (code == 15)
    {
        ui_confirm("Elimina credenziale?", "La voce verrà rimossa dall'archivio.", "Elimina",
                   UI_COLOR_RED, delete_confirmed, NULL);
        return;
    }
    else if (code == 16)
    {
        vault_job_t *job = new_job(s_ui->editing_preview ? VAULT_PREVIEW_EDIT : VAULT_EDIT);
        if (job == NULL)
            return;
        job->entry = s_ui->draft;
        job->index = s_ui->edit_index;
        job->expected_generation = s_ui->edit_generation;
        memcpy(job->preview, s_ui->preview, 33);
        if (!vault_entry_valid(&job->entry))
        {
            vault_job_free(job);
            ui_toast("Controlla nome e segreto TOTP");
            return;
        }
        if (submit(job) == ESP_OK)
            s_ui->pending_save = true;
        return;
    }
    else if (code == 17)
    {
        notify(vault_generate(s_ui->draft.password, 24, true));
    }
    else if (code == 18)
        s_ui->draft.otp_digits = s_ui->draft.otp_digits == 6 ? 8 : 6;
    else if (code == 19)
        s_ui->draft.otp_period = s_ui->draft.otp_period == 30 ? 60 : 30;
    else if (code == 20)
        s_ui->draft.otp_hash = s_ui->draft.otp_hash == 1     ? 256
                               : s_ui->draft.otp_hash == 256 ? 512
                                                             : 1;
    else if (code == 21 || code == 22)
    {
        vault_job_t *job = new_job(VAULT_WEB);
        if (job != NULL)
        {
            job->enabled = code == 21;
            submit(job);
        }
        return;
    }
    else if (code == 23)
    {
        vault_hid_pair();
        ui_toast("Abbina Watch Vault entro 60 secondi");
        return;
    }
    else if (code == 24)
    {
        s_ui->page = 7;
        s_ui->offset = 0;
    }
    else if (code == 25)
    {
        ui_confirm("Applica importazione?", "Verranno applicate le scelte indicate per ogni voce.",
                   "Importa", GOLD, import_confirmed, NULL);
        return;
    }
    else if (code == 26)
        s_ui->page = 8;
    else if (code >= 27 && code <= 30)
    {
        ui_confirm("Cambia connessione?",
                   "Vault si blocca e si riavvia con la nuova configurazione.", "Cambia", GOLD,
                   interface_confirmed, (void *)(uintptr_t)code);
        return;
    }
    else if (code == 31)
    {
        vault_input("File in vault/imports", "backup.wvault", false, import_name, NULL);
        return;
    }
    else if (code == 32)
    {
        unsigned i;
        for (i = 0; i < s_ui->selection_count; i++)
            if (strcmp(s_ui->selection[i], s_ui->id) == 0)
                break;
        if (i < s_ui->selection_count)
        {
            memmove(&s_ui->selection[i], &s_ui->selection[i + 1],
                    (s_ui->selection_count - i - 1) * 33);
            vault_wipe(s_ui->selection[--s_ui->selection_count], 33);
            ui_toast("Voce deselezionata");
        }
        else if (s_ui->selection_count < VAULT_ENTRIES)
        {
            memcpy(s_ui->selection[s_ui->selection_count++], s_ui->id, 33);
            ui_toast("Voce selezionata");
        }
    }
    else if (code == 33)
    {
        export_selection(2);
        return;
    }
    else if ((code >> 16) == 1)
    {
        vault_entry_t entry;
        if (!vault_entry(code & 65535, &entry))
            return;
        memcpy(s_ui->id, entry.id, 33);
        vault_wipe(&entry, sizeof(entry));
        s_ui->page = 4;
        s_ui->revealing = false;
    }
    else if ((code >> 16) == 2)
    {
        vault_entry_t entry;
        if (!vault_entry(code & 65535, &entry))
            return;
        snprintf(s_ui->value, sizeof(s_ui->value), "%s", vault_field(&entry, s_ui->field));
        vault_wipe(&entry, sizeof(entry));
        s_ui->grouped = true;
        s_ui->offset = 0;
        s_ui->page = 2;
    }
    else if ((code >> 16) == 3)
    {
        unsigned field = code & 65535;
        static const char *names[] = {"email", "username", "password", "sito"};
        if (field > 3)
            return;
        char message[160];
        snprintf(message, sizeof(message),
                 "Posiziona il cursore nel campo corretto sul PC. Scrivere %s?", names[field]);
        ui_confirm("Conferma scrittura", message, "Scrivi", GOLD, type_confirmed,
                   (void *)(uintptr_t)field);
        return;
    }
    else if ((code >> 16) == 4)
    {
        unsigned field = code & 65535;
        if (field > 6)
            return;
        static const char *names[] = {"Nome",     "Email", "Username",    "Password",
                                      "Sito web", "Note",  "Segreto TOTP"};
        size_t capacity;
        s_ui->edit_field = field;
        char *text = edit_pointer(field, &capacity);
        (void)capacity;
        vault_input(names[field], text, field == 3 || field == 6, input_field, NULL);
        return;
    }
    else if ((code >> 16) == 5)
    {
        unsigned index = code & 65535;
        if (index >= s_ui->status.preview_count)
            return;
        s_ui->choices[index] = (s_ui->choices[index] + 1) % 4;
    }
    else if ((code >> 16) == 6)
    {
        int conflict;
        s_ui->edit_index = code & 65535;
        if (!vault_preview_entry(s_ui->edit_index, &s_ui->draft, &conflict, s_ui->preview))
            return;
        s_ui->editing_preview = true;
        s_ui->page = 5;
    }
    render();
}

static lv_obj_t *hero_label(const char *text, int y, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(s_ui->content);
    lv_obj_set_width(label, LV_PCT(76));
    lv_label_set_text(label, text);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, ui_color(color), 0);
    lv_obj_set_clickable(label, false);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static void render(void)
{
    if (s_ui == NULL)
        return;
    if (s_ui->content != NULL)
        lv_obj_delete(s_ui->content);
    s_ui->status_label = NULL;
    s_ui->otp_label = s_ui->otp_expiry = NULL;
    if (!vault_unlocked())
        s_ui->page = 0;
    if (s_ui->page == 0)
    {
        s_ui->content = lv_obj_create(s_ui->screen);
        lv_obj_remove_style_all(s_ui->content);
        lv_obj_set_size(s_ui->content, LV_PCT(100), LV_PCT(100));
        lv_obj_set_scrollable(s_ui->content, false);
        lv_obj_set_clickable(s_ui->content, true);
        lv_port_set_focusable(s_ui->content);
        lv_obj_add_event_cb(s_ui->content, hero_clicked, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(s_ui->content, hero_clicked, LV_EVENT_GESTURE, NULL);
        int height = lv_obj_get_height(s_ui->screen), width = lv_obj_get_width(s_ui->screen);
        int size = (width < height ? width : height) * 58 / 100;
        hero_label("VAULT", height * 13 / 100, ui_font_title, GOLD);
        lv_obj_t *door = lv_obj_create(s_ui->content);
        lv_obj_remove_style_all(door);
        lv_obj_set_size(door, size, size);
        lv_obj_align(door, LV_ALIGN_CENTER, 0, -height * 5 / 100);
        lv_obj_set_style_radius(door, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(door, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(door, ui_color(0x354355), 0);
        lv_obj_set_style_bg_grad_color(door, ui_color(0x0E1521), 0);
        lv_obj_set_style_bg_grad_dir(door, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_color(door, ui_color(GOLD), 0);
        lv_obj_set_style_border_width(door, 3, 0);
        lv_obj_set_style_shadow_width(door, 36, 0);
        lv_obj_set_style_shadow_color(door, ui_color(0x746047), 0);
        lv_obj_set_style_shadow_opa(door, LV_OPA_30, 0);
        lv_obj_set_clickable(door, false);
        lv_obj_t *key = lv_obj_create(door);
        lv_obj_remove_style_all(key);
        lv_obj_set_size(key, 42, 42);
        lv_obj_set_style_radius(key, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(key, ui_color(vault_unlocked() ? TEAL : GOLD), 0);
        lv_obj_align(key, LV_ALIGN_CENTER, 0, -14);
        lv_obj_set_clickable(key, false);
        lv_obj_t *stem = lv_obj_create(door);
        lv_obj_remove_style_all(stem);
        lv_obj_set_size(stem, 18, 38);
        lv_obj_set_style_radius(stem, 5, 0);
        lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(stem, ui_color(vault_unlocked() ? TEAL : GOLD), 0);
        lv_obj_align(stem, LV_ALIGN_CENTER, 0, 16);
        lv_obj_set_clickable(stem, false);
        char caption[80];
        snprintf(caption, sizeof(caption),
                 vault_unlocked() ? "%u credenziali · spazio privato" : "Il tuo archivio, protetto",
                 s_ui->status.count);
        hero_label(caption, height * 76 / 100, UI_FONT_SMALL, 0xBAC4D7);
        s_ui->status_label =
            hero_label(s_ui->status.busy  ? "Sblocco in corso..."
                       : vault_unlocked() ? "Tocca per esplorare"
                                          : "Tocca per sbloccare",
                       height * 85 / 100, UI_FONT_SMALL, vault_unlocked() ? TEAL : GOLD);
        return;
    }
    static const char *titles[] = {"Vault",           "Spazio privato", "Credenziali",
                                   "Gruppi",          "Credenziale",    "Modifica",
                                   "Dashboard HTTPS", "Importazione",   "Connessione"};
    s_ui->content = ui_page(s_ui->screen, titles[s_ui->page]);
    lv_obj_t *page = s_ui->content;
    if (s_ui->page == 1)
    {
        row(page, "Tutte le credenziali", "", 1);
        row(page, "Per email", "", 2);
        row(page, "Per username", "", 3);
        row(page, "Per sito web", "", 4);
        row(page, "Nuova credenziale", LV_SYMBOL_PLUS, 5);
        row(page, "Dashboard PC", "HTTPS", 6);
        row(page, "Esporta tutte", "Cifrato su SD", 11);
        row(page, "Importa dalla SD", "File cifrato", 31);
        row(page, "Anteprima importazione", "", 24);
        row(page, "Esporta selezionate", "Cifrato su SD", 33);
        row(page, "Connessione e layout", "Bluetooth / USB", 26);
        row(page, "Abbina Bluetooth", "PIN sul dispositivo", 23);
        row(page, "Blocca archivio", "", 7);
    }
    else if (s_ui->page == 2 || s_ui->page == 3)
    {
        if (s_ui->grouped)
            vault_text(page, s_ui->value[0] ? s_ui->value : "Non specificato");
        unsigned indices[8], total = 0;
        unsigned count = vault_query(s_ui->field, s_ui->page == 3, s_ui->grouped, s_ui->value,
                                     s_ui->offset, indices, 8, &total);
        for (unsigned i = 0; i < count; i++)
        {
            vault_entry_t entry;
            if (!vault_entry(indices[i], &entry))
                continue;
            const char *text = s_ui->page == 3 ? vault_field(&entry, s_ui->field) : entry.name;
            row(page, text[0] ? text : "Non specificato",
                s_ui->page == 3  ? "Apri gruppo"
                : entry.email[0] ? entry.email
                                 : entry.username,
                ((s_ui->page == 3 ? 2U : 1U) << 16) | indices[i]);
            vault_wipe(&entry, sizeof(entry));
        }
        if (total == 0)
            vault_text(page, "Nessuna credenziale. Aggiungila qui o dalla dashboard.");
        if (s_ui->offset)
            row(page, "Precedenti", "", 9);
        if (s_ui->offset + count < total)
            row(page, "Successive", "", 10);
        if (s_ui->grouped)
            row(page, "Esporta gruppo", "Cifrato su SD", 11);
    }
    else if (s_ui->page == 4)
    {
        vault_entry_t entry;
        unsigned index;
        if (!selected(&entry, &index))
        {
            vault_text(page, "Voce non più disponibile");
            return;
        }
        vault_text(page, entry.name);
        row(page, "Email", entry.email, (3U << 16) | 0);
        row(page, "Username", entry.username, (3U << 16) | 1);
        row(page, "Password", "Scrivi sul PC", (3U << 16) | 2);
        row(page, "Sito web", entry.website, (3U << 16) | 3);
        row(page, "Mostra password", "Per 10 secondi", 14);
        if (s_ui->revealing)
            vault_text(page, entry.password);
        if (entry.otp[0])
        {
            char code[9];
            time_t now = time(NULL);
            if (clock_is_valid() && now >= 1704067200 &&
                vault_totp(entry.otp, now, entry.otp_digits, entry.otp_period, entry.otp_hash,
                           code) == ESP_OK)
            {
                s_ui->otp_label = vault_text(page, code);
                s_ui->otp_expiry = vault_text(page, "");
                s_ui->otp_counter = now / entry.otp_period;
                s_ui->otp_period = entry.otp_period;
                vault_wipe(code, sizeof(code));
            }
            else
                vault_text(page, "TOTP: verifica l'orologio");
        }
        if (entry.note[0])
            vault_text(page, entry.note);
        row(page, "Modifica", "", 13);
        row(page, "Esporta voce", "Cifrato", 12);
        row(page, "Elimina", "", 15);
        row(page, "Seleziona / deseleziona", "Per export multiplo", 32);
        vault_wipe(&entry, sizeof(entry));
    }
    else if (s_ui->page == 5)
    {
        static const char *names[] = {"Nome",     "Email", "Username",    "Password",
                                      "Sito web", "Note",  "Segreto TOTP"};
        for (unsigned i = 0; i < 7; i++)
        {
            size_t capacity;
            char *value = edit_pointer(i, &capacity);
            (void)capacity;
            row(page, names[i], i == 3 || i == 6 ? value[0] ? "Impostato" : "Vuoto" : value,
                (4U << 16) | i);
        }
        row(page, "Genera password", "24 caratteri", 17);
        char value[24];
        snprintf(value, sizeof(value), "%u", s_ui->draft.otp_digits);
        row(page, "Cifre TOTP", value, 18);
        snprintf(value, sizeof(value), "%u s", s_ui->draft.otp_period);
        row(page, "Periodo TOTP", value, 19);
        snprintf(value, sizeof(value), "SHA-%u",
                 s_ui->draft.otp_hash == 1 ? 1 : s_ui->draft.otp_hash);
        row(page, "Algoritmo TOTP", value, 20);
        ui_button(page, "Salva", GOLD, action, (void *)(uintptr_t)16);
    }
    else if (s_ui->page == 6)
    {
        char url[64], code[9], fingerprint[96];
        vault_web_info(url, sizeof(url), code, sizeof(code), fingerprint, sizeof(fingerprint));
        if (url[0])
        {
            vault_text(page, url);
            vault_text(page, code[0] ? code : "PC abbinato");
            ui_section(page, "Impronta SHA-256 del certificato");
            vault_text(page, fingerprint);
            vault_text(page, "Confrontala sul PC prima di inserire il codice.");
            row(page, "Ferma dashboard", "Revoca sessione", 22);
        }
        else
        {
            vault_text(page, "HTTPS locale, attivato solo qui. Nessun invio al cloud.");
            ui_button(page, "Avvia dashboard", GOLD, action, (void *)(uintptr_t)21);
        }
    }
    else if (s_ui->page == 7)
    {
        if (!s_ui->status.preview_count)
            vault_text(page, "Importa un file .wvault dalla dashboard HTTPS. Le scelte saranno "
                             "disponibili qui.");
        for (unsigned i = s_ui->offset; i < s_ui->status.preview_count && i < s_ui->offset + 8; i++)
        {
            vault_entry_t entry;
            int conflict;
            char token[33];
            if (!vault_preview_entry(i, &entry, &conflict, token))
                continue;
            if (memcmp(token, s_ui->preview, 33) != 0)
            {
                memcpy(s_ui->preview, token, 33);
                memset(s_ui->choices, 0, sizeof(s_ui->choices));
            }
            static const char *choices[] = {"Salta", "Mantieni / aggiungi",
                                            "Sostituisci / aggiungi", "Duplica"};
            row(page, entry.name, choices[s_ui->choices[i]], (5U << 16) | i);
            row(page, "Modifica voce importata", "Prima del salvataggio", (6U << 16) | i);
            if (conflict >= 0)
                vault_text(page, "Conflitto con voce esistente");
            vault_wipe(&entry, sizeof(entry));
        }
        if (s_ui->offset)
            row(page, "Precedenti", "", 9);
        if (s_ui->offset + 8 < s_ui->status.preview_count)
            row(page, "Successive", "", 10);
        if (s_ui->status.preview_count)
            ui_button(page, "Applica scelte", GOLD, action, (void *)(uintptr_t)25);
    }
    else if (s_ui->page == 8)
    {
        vault_text(page,
                   hid_link_transport() == HID_LINK_USB ? "USB HID" : "Bluetooth autenticato");
        vault_text(page, hid_link_host_layout() == HID_HOST_IT ? "Layout host italiano"
                                                               : "Layout host US");
        row(page, "Bluetooth", "PIN sul dispositivo", 27);
        if (board_info()->has_usb_otg)
            row(page, "USB HID", "Porta USB nativa", 28);
        else
            vault_text(page, "Questo connettore non espone USB HID.");
        row(page, "Layout italiano", "IT", 29);
        row(page, "Layout americano", "US", 30);
        vault_text(page, "Il layout deve corrispondere a quello del PC. Nessun Invio automatico.");
    }
    row(page, "Menu", "", 8);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    vault_status_t status;
    if (!vault_status(&status))
        return;
    if (status.unlocked && lv_display_get_inactive_time(lv_display_get_default()) < 1000)
        vault_activity();
    bool changed = status.revision != s_ui->status.revision || status.unlocked != s_ui->was_open;
    if (status.busy && s_ui->status.busy && s_ui->page == 0)
        changed = false;
    s_ui->status = status;
    if (s_ui->was_open && !status.unlocked)
    {
        vault_input_cancel();
        vault_wipe(&s_ui->draft, sizeof(s_ui->draft));
        vault_wipe(s_ui->creation, sizeof(s_ui->creation));
        vault_wipe(s_ui->id, sizeof(s_ui->id));
        vault_wipe(s_ui->value, sizeof(s_ui->value));
        s_ui->page = 0;
        s_ui->revealing = false;
        s_ui->pending_save = false;
    }
    else if (!s_ui->was_open && status.unlocked)
        s_ui->page = 1;
    s_ui->was_open = status.unlocked;
    if (s_ui->revealing && esp_timer_get_time() > s_ui->reveal_until)
    {
        s_ui->revealing = false;
        changed = true;
    }
    if (s_ui->pending_save && !status.busy)
    {
        s_ui->pending_save = false;
        if (status.last_error == ESP_OK)
        {
            s_ui->page = s_ui->editing_preview ? 7 : 2;
            vault_wipe(&s_ui->draft, sizeof(s_ui->draft));
        }
        changed = true;
    }
    /* Do not rebuild the form on every KDF progress update. */
    if (changed && !(status.busy && s_ui->page == 5))
        render();
    bool kdf = status.operation == VAULT_CREATE || status.operation == VAULT_UNLOCK ||
               status.operation == VAULT_PREVIEW || status.operation == VAULT_IMPORT_FILE;
    ui_border_progress_color(s_ui->border, status.busy ? GOLD : TEAL);
    ui_border_progress_set(s_ui->border,
                           status.busy ? (kdf ? status.progress : 0) : status.remaining,
                           status.busy ? 100 : 120);
    if (s_ui->status_label != NULL && s_ui->page == 0)
    {
        char text[128];
        if (status.busy)
            snprintf(text, sizeof(text), "Derivazione chiave · %u%%", status.progress);
        else if (status.cooldown)
            snprintf(text, sizeof(text), "Riprova tra %u secondi", status.cooldown);
        else
            snprintf(text, sizeof(text), "%s",
                     status.storage ? status.message : "Inserisci una microSD");
        lv_label_set_text(s_ui->status_label, text);
    }
    unsigned pin = vault_hid_passkey();
    lv_obj_set_hidden(s_ui->pin_label, !pin || !status.unlocked);
    if (pin && status.unlocked)
    {
        char text[64];
        snprintf(text, sizeof(text), "PIN Bluetooth\n%06u", pin - 1);
        lv_label_set_text(s_ui->pin_label, text);
        lv_obj_move_foreground(s_ui->pin_label);
    }
    if (status.unlocked && s_ui->page == 4 && s_ui->otp_label != NULL && s_ui->otp_period)
    {
        time_t now = time(NULL);
        char remaining[32];
        snprintf(remaining, sizeof(remaining), "Scade tra %u s",
                 s_ui->otp_period - (unsigned)(now % s_ui->otp_period));
        lv_label_set_text(s_ui->otp_expiry, remaining);
        if (now / s_ui->otp_period != s_ui->otp_counter)
        {
            vault_entry_t entry;
            unsigned index;
            char code[9];
            if (selected(&entry, &index) && clock_is_valid() && now >= 1704067200 &&
                vault_totp(entry.otp, now, entry.otp_digits, entry.otp_period, entry.otp_hash,
                           code) == ESP_OK)
            {
                lv_label_set_text(s_ui->otp_label, code);
                s_ui->otp_counter = now / s_ui->otp_period;
            }
            else
                lv_label_set_text(s_ui->otp_label, "Verifica l'orologio");
            vault_wipe(&entry, sizeof(entry));
            vault_wipe(code, sizeof(code));
        }
    }
}

void vault_ui_create(lv_obj_t *screen)
{
    s_ui = vault_secure_alloc(sizeof(*s_ui));
    if (s_ui == NULL)
    {
        vault_text(screen, "Memoria insufficiente");
        return;
    }
    memset(s_ui, 0, sizeof(*s_ui));
    s_ui->screen = screen;
    s_ui->border = ui_border_progress_create(screen, GOLD);
    s_ui->pin_label = lv_label_create(screen);
    lv_obj_set_width(s_ui->pin_label, LV_PCT(74));
    lv_obj_set_style_text_align(s_ui->pin_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui->pin_label, UI_FONT_LARGE, 0);
    lv_obj_set_style_bg_color(s_ui->pin_label, ui_color(0x141B28), 0);
    lv_obj_set_style_bg_opa(s_ui->pin_label, LV_OPA_COVER, 0);
    lv_obj_align(s_ui->pin_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_hidden(s_ui->pin_label, true);
    vault_status(&s_ui->status);
    render();
    s_ui->timer = lv_timer_create(tick, 200, NULL);
}

bool vault_ui_back(void)
{
    if (s_ui == NULL)
        return false;
    if (s_ui->page == 5)
        vault_wipe(&s_ui->draft, sizeof(s_ui->draft));
    if (s_ui->page != 0)
    {
        s_ui->page = 0;
        render();
        return true;
    }
    vault_exit();
    return true;
}

void vault_ui_scrub(void)
{
    if (s_ui == NULL)
        return;
    vault_input_scrub();
    vault_wipe(&s_ui->draft, sizeof(s_ui->draft));
    vault_wipe(s_ui->creation, sizeof(s_ui->creation));
    vault_wipe(s_ui->id, sizeof(s_ui->id));
    vault_wipe(s_ui->value, sizeof(s_ui->value));
    vault_wipe(s_ui->preview, sizeof(s_ui->preview));
    memset(s_ui->choices, 0, sizeof(s_ui->choices));
    vault_wipe(s_ui->selection, sizeof(s_ui->selection));
    s_ui->selection_count = 0;
    s_ui->page = 0;
    s_ui->was_open = false;
    s_ui->status.unlocked = false;
    s_ui->status.count = 0;
    s_ui->revealing = false;
    s_ui->pending_save = false;
    lv_obj_set_hidden(s_ui->pin_label, true);
    render();
}
