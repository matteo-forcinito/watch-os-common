/* Two on-demand internal apps. Form drafts and callbacks belong to the app UI;
 * file writes and scheduled reminders belong to life_store. */
#include "core/app.h"
#include "life_store.h"
#include "life_web.h"
#include "ui/ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    HOME,
    CITY,
    PET,
    FORM,
    DASHBOARD
};
enum
{
    OPEN_CITY,
    OPEN_PET,
    EDIT,
    NEW,
    SAVE,
    DELETE_ITEM,
    FIELD,
    TOGGLE,
    DAY,
    EXCEPTION_BIN,
    PREFERRED,
    HINT,
    DONE,
    POSTPONE,
    DASH,
    RETRY,
    PRESET,
    MORE_LOGS
};
typedef struct
{
    const char *key, *label;
    int type;
} field_t;
/* type: 0 text, 1 integer, 2 boolean, 3 HH:MM, 4 hexadecimal color */
static const field_t city_fields[] = {
    {"name", "Nome", 0}, {"zone", "Zona / utenza", 0}, {"source", "Fonte calendario", 0}, {NULL}};
static const field_t bin_fields[] = {{"name", "Materiali raggruppati", 0},
                                     {"every", "Ogni quante settimane (1-8)", 1},
                                     {"anchor", "Prima settimana: AAAA-MM-GG", 0},
                                     {"color", "Colore: RRGGBB", 4},
                                     {NULL}};
static const field_t pet_fields[] = {{"name", "Nome del cane", 0},
                                     {"birth", "Nascita: AAAA-MM-GG", 0},
                                     {"sex", "Sesso", 5},
                                     {"breed", "Razza", 0},
                                     {"relation", "Relazione familiare", 0},
                                     {"neutered", "Sterilizzazione", 2},
                                     {"vet", "Veterinario", 0},
                                     {"phone", "Telefono veterinario", 0},
                                     {"microchip", "Microchip", 0},
                                     {"food", "Alimentazione", 0},
                                     {"allergies", "Allergie / sensibilita", 0},
                                     {"notes", "Note private", 0},
                                     {NULL}};
static const field_t task_fields[] = {{"label", "Attivita", 0},
                                      {"kind", "Categoria", 0},
                                      {"due", "Scadenza: AAAA-MM-GG", 0},
                                      {"minute", "Ora: HH:MM", 3},
                                      {"repeat_days", "Ripeti ogni N giorni (0 = una volta)", 1},
                                      {"enabled", "Attiva", 2},
                                      {"notify", "Notifica anche ad app chiusa", 2},
                                      {"notes", "Piano / indicazioni del veterinario", 0},
                                      {NULL}};
static const field_t weight_fields[] = {{"date", "Data: AAAA-MM-GG", 0}, {"grams", "Peso in grammi", 1}, {NULL}};
static const field_t log_fields[] = {{"date", "Data: AAAA-MM-GG", 0},
                                     {"label", "Titolo", 0},
                                     {"kind", "Categoria", 0},
                                     {"note", "Nota privata", 0},
                                     {NULL}};
static const field_t exception_fields[] = {{"date", "Data: AAAA-MM-GG", 0}, {NULL}};
static const char *days[] = {"Lunedi", "Martedi", "Mercoledi", "Giovedi", "Venerdi", "Sabato", "Domenica"};
typedef struct
{
    life_kind_t kind;
    lv_obj_t *page;
    bool active, opened;
    int view, city, pet, entity, rendered_view, log_limit;
    cJSON *draft;
    const field_t *editing;
    unsigned generation, edit_generation;
} life_ui_t;
typedef struct
{
    life_ui_t *ui;
    int action, id;
    const char *entity;
    const field_t *field;
} action_t;
static life_ui_t screens[LIFE_COUNT];
static void render(life_ui_t *u);
static const cJSON *get(const cJSON *o, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(o, key);
}
static void put(cJSON *o, const char *key, cJSON *v)
{
    if (!cJSON_ReplaceItemInObjectCaseSensitive(o, key, v))
        cJSON_AddItemToObject(o, key, v);
}
static void num(cJSON *o, const char *key, int n)
{
    put(o, key, cJSON_CreateNumber(n));
}
static void str(cJSON *o, const char *key, const char *s)
{
    put(o, key, cJSON_CreateString(s));
}
static const char *name(life_ui_t *u)
{
    return u->kind == LIFE_ZAMPE ? "Zampe" : "Raccolta";
}
static uint32_t accent(life_ui_t *u)
{
    return u->kind == LIFE_ZAMPE ? 0xEDAD82 : 0x70D5B0;
}
static void clicked(lv_event_t *e);
static void dispose(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}
static void row(life_ui_t *u, const char *title, const char *value, int action, int id, const char *entity,
                const field_t *field)
{
    action_t *a = malloc(sizeof(*a));
    if (!a)
        return;
    *a = (action_t){u, action, id, entity, field};
    lv_obj_t *r = ui_row(u->page, LV_SYMBOL_RIGHT, accent(u), title, value, clicked, a);
    lv_obj_add_event_cb(r, dispose, LV_EVENT_DELETE, a);
}
static bool command(life_ui_t *u, cJSON *c)
{
    char error[128];
    bool ok = life_command(u->kind, c, error, sizeof(error));
    ui_toast(ok ? "Salvataggio in corso" : error);
    return ok;
}
static cJSON *op(const char *entity, const char *verb, int id, int city)
{
    cJSON *c = cJSON_CreateObject();
    char s[40];
    snprintf(s, sizeof(s), "%s.%s", entity, verb);
    str(c, "op", s);
    if (id)
        num(c, "id", id);
    if (city)
        num(c, "city", city);
    return c;
}
static void form(life_ui_t *u, const char *entity, int id)
{
    cJSON *snapshot = life_snapshot(u->kind);
    const cJSON *base = NULL;
    const cJSON *city = life_find(get(snapshot, "cities"), u->city);
    if (!strcmp(entity, "city"))
        base = life_find(get(snapshot, "cities"), id);
    if (!strcmp(entity, "bin"))
        base = life_find(get(city, "bins"), id);
    if (!strcmp(entity, "exception"))
        base = cJSON_GetArrayItem(get(city, "exceptions"), id - 1);
    if (!strcmp(entity, "pet"))
        base = life_find(get(snapshot, "pets"), id);
    if (!strcmp(entity, "task"))
        base = life_find(get(snapshot, "tasks"), id);
    if (!strcmp(entity, "weight"))
        base = life_find(get(snapshot, "weights"), id);
    if (!strcmp(entity, "log"))
        base = life_find(get(snapshot, "logs"), id);
    cJSON_Delete(u->draft);
    u->draft = base ? cJSON_Duplicate(base, true) : cJSON_CreateObject();
    char s[40];
    snprintf(s, sizeof(s), "%s.%s", entity, !strcmp(entity, "exception") ? "put" : base ? "edit" : "add");
    str(u->draft, "op", s);
    if (u->city)
        num(u->draft, "city", u->city);
    if (u->pet)
        num(u->draft, "pet", u->pet);
    if (!base)
    {
        char date[11];
        life_date(life_today(), date);
        if (!strcmp(entity, "task"))
        {
            str(u->draft, "due", date);
            num(u->draft, "minute", 540);
            put(u->draft, "enabled", cJSON_CreateTrue());
        }
        if (!strcmp(entity, "weight") || !strcmp(entity, "log") || !strcmp(entity, "exception"))
            str(u->draft, "date", date);
        if (!strcmp(entity, "exception"))
            put(u->draft, "bins", cJSON_CreateArray());
        if (!strcmp(entity, "bin"))
        {
            num(u->draft, "every", 1);
            str(u->draft, "anchor", date);
            num(u->draft, "color", 0x70D5B0);
        }
        if (!strcmp(entity, "pet"))
            str(u->draft, "sex", "unknown");
    }
    u->entity = !strcmp(entity, "city")        ? 0
                : !strcmp(entity, "bin")       ? 1
                : !strcmp(entity, "exception") ? 2
                : !strcmp(entity, "pet")       ? 3
                : !strcmp(entity, "task")      ? 4
                : !strcmp(entity, "weight")    ? 5
                                               : 6;
    u->view = FORM;
    u->generation++;
    cJSON_Delete(snapshot);
    render(u);
}
static const char *entities[] = {"city", "bin", "exception", "pet", "task", "weight", "log"};
static const field_t *fields[] = {city_fields, bin_fields,    exception_fields, pet_fields,
                                  task_fields, weight_fields, log_fields};
static void entered(const char *text, void *arg)
{
    life_ui_t *u = arg;
    if (!u->active || u->edit_generation != u->generation || !text || !u->draft || !u->editing)
        return;
    const field_t *f = u->editing;
    if (f->type == 1 || f->type == 4)
    {
        char *end;
        long v = strtol(text, &end, f->type == 4 ? 16 : 10);
        if (!text[0] || *end || v < 0 || v > 0x7fffffffL)
        {
            ui_toast("Numero non valido");
            return;
        }
        num(u->draft, f->key, (int)v);
    }
    else if (f->type == 3)
    {
        int h, m;
        char extra;
        if (sscanf(text, "%d:%d%c", &h, &m, &extra) != 2 || h < 0 || h > 23 || m < 0 || m > 59)
        {
            ui_toast("Usa HH:MM");
            return;
        }
        num(u->draft, f->key, h * 60 + m);
    }
    else
        str(u->draft, f->key, text);
    render(u);
}
static void confirmed(bool yes, void *arg)
{
    life_ui_t *u = arg;
    if (!yes || !u->active || !u->draft)
        return;
    const char *entity = entities[u->entity];
    cJSON *c = op(entity, "delete", life_number(u->draft, "id", 0), u->city);
    if (!strcmp(entity, "city"))
        num(c, "city", life_number(u->draft, "id", 0));
    if (!strcmp(entity, "exception"))
        str(c, "date", life_string(u->draft, "date"));
    if (command(u, c))
    {
        u->view = (!strcmp(entity, "city") || !strcmp(entity, "pet")) ? HOME : u->kind == LIFE_RACCOLTA ? CITY : PET;
        render(u);
    }
    cJSON_Delete(c);
}
static void clicked(lv_event_t *e)
{
    action_t a = *(action_t *)lv_event_get_user_data(e);
    life_ui_t *u = a.ui;
    if (!u->active)
        return;
    if (a.action == OPEN_CITY)
    {
        u->city = a.id;
        u->view = CITY;
        render(u);
        return;
    }
    if (a.action == OPEN_PET)
    {
        u->pet = a.id;
        u->view = PET;
        render(u);
        return;
    }
    if (a.action == NEW || a.action == EDIT)
    {
        form(u, a.entity, a.action == NEW ? 0 : a.id);
        return;
    }
    if (a.action == DASH)
    {
        appkit_web_start(life_web_config(u->kind));
        appkit_web_activate();
        u->view = DASHBOARD;
        render(u);
        return;
    }
    if (a.action == FIELD)
    {
        if (a.field->type == 5)
        {
            const char *v = life_string(u->draft, a.field->key);
            str(u->draft, a.field->key, !strcmp(v, "unknown") ? "female" : !strcmp(v, "female") ? "male" : "unknown");
            render(u);
            return;
        }
        u->editing = a.field;
        u->edit_generation = u->generation;
        char s[32];
        const char *initial = life_string(u->draft, a.field->key);
        int v = life_number(u->draft, a.field->key, 0);
        if (a.field->type == 1)
        {
            snprintf(s, sizeof(s), "%d", v);
            initial = s;
        }
        if (a.field->type == 4)
        {
            snprintf(s, sizeof(s), "%06X", v);
            initial = s;
        }
        if (a.field->type == 3)
        {
            snprintf(s, sizeof(s), "%02d:%02d", v / 60, v % 60);
            initial = s;
        }
        ui_text_input(a.field->label, initial, false, entered, u);
        return;
    }
    if (a.action == TOGGLE)
    {
        put(u->draft, a.field->key, cJSON_CreateBool(!cJSON_IsTrue(get(u->draft, a.field->key))));
        render(u);
        return;
    }
    if (a.action == DAY)
    {
        num(u->draft, "days", life_number(u->draft, "days", 0) ^ (1 << a.id));
        render(u);
        return;
    }
    if (a.action == EXCEPTION_BIN)
    {
        cJSON *bins = (cJSON *)get(u->draft, "bins");
        bool found = false;
        for (int i = cJSON_GetArraySize(bins) - 1; i >= 0; i--)
            if (cJSON_GetArrayItem(bins, i)->valueint == a.id)
            {
                cJSON_DeleteItemFromArray(bins, i);
                found = true;
            }
        if (!found)
            cJSON_AddItemToArray(bins, cJSON_CreateNumber(a.id));
        render(u);
        return;
    }
    if (a.action == DELETE_ITEM)
    {
        ui_confirm("Eliminare?",
                   u->entity == 3 ? "Rimuove il profilo e tutte le sue attivita, pesi e registrazioni."
                                  : "La rimozione e definitiva.",
                   "Elimina", UI_COLOR_RED, confirmed, u);
        return;
    }
    if (a.action == SAVE)
    {
        if (command(u, u->draft))
        {
            u->generation++;
            u->view = u->entity == 0 || u->entity == 3 ? HOME : u->kind == LIFE_RACCOLTA ? CITY : PET;
            render(u);
        }
        return;
    }
    if (a.action == MORE_LOGS)
    {
        u->log_limit += 20;
        render(u);
        return;
    }
    cJSON *c = cJSON_CreateObject();
    if (a.action == PREFERRED)
    {
        str(c, "op", "settings");
        num(c, "preferred", u->city);
    }
    if (a.action == HINT)
    {
        cJSON *s = life_snapshot(u->kind);
        str(c, "op", "settings");
        put(c, "menu_hint", cJSON_CreateBool(!cJSON_IsTrue(get(s, "menu_hint"))));
        cJSON_Delete(s);
    }
    if (a.action == DONE || a.action == POSTPONE)
    {
        str(c, "op", a.action == DONE ? "task.done" : "task.postpone");
        num(c, "id", a.id);
    }
    if (a.action == RETRY)
        str(c, "op", "save.retry");
    if (a.action == PRESET)
    {
        str(c, "op", "city.add");
        str(c, "name", "Civita Castellana");
        str(c, "preset", "civita-domestica");
    }
    if (command(u, c))
        render(u);
    cJSON_Delete(c);
}
static void render_form(life_ui_t *u, const cJSON *snapshot)
{
    ui_section(u->page, "Modifica sul dispositivo");
    for (const field_t *f = fields[u->entity]; f->key; f++)
    {
        char value[40];
        const char *s = life_string(u->draft, f->key);
        int v = life_number(u->draft, f->key, 0);
        if (f->type == 2)
            s = cJSON_IsTrue(get(u->draft, f->key)) ? "Si" : "No";
        else if (f->type == 1)
        {
            snprintf(value, sizeof(value), "%d", v);
            s = value;
        }
        else if (f->type == 3)
        {
            snprintf(value, sizeof(value), "%02d:%02d", v / 60, v % 60);
            s = value;
        }
        else if (f->type == 4)
        {
            snprintf(value, sizeof(value), "%06X", v);
            s = value;
        }
        else if (f->type == 5)
            s = !strcmp(s, "female") ? "Femmina" : !strcmp(s, "male") ? "Maschio" : "Non specificato";
        row(u, f->label, s, f->type == 2 ? TOGGLE : FIELD, 0, NULL, f);
    }
    if (u->entity == 1)
    {
        ui_section(u->page, "Giorni di passaggio");
        for (int i = 0; i < 7; i++)
            row(u, days[i], (life_number(u->draft, "days", 0) & (1 << i)) ? "Si" : "No", DAY, i, NULL, NULL);
    }
    if (u->entity == 2)
    {
        ui_text(u->page, "Nessuna selezione = nessun ritiro in questa data.", true);
        const cJSON *city = life_find(get(snapshot, "cities"), u->city), *b;
        cJSON_ArrayForEach(b, get(city, "bins"))
        {
            int id = life_number(b, "id", 0);
            bool yes = false;
            const cJSON *v;
            cJSON_ArrayForEach(v, get(u->draft, "bins")) if (v->valueint == id) yes = true;
            row(u, life_string(b, "name"), yes ? "Si" : "No", EXCEPTION_BIN, id, NULL, NULL);
        }
    }
    row(u, "Salva", NULL, SAVE, 0, NULL, NULL);
    if (life_number(u->draft, "id", 0) || u->entity == 2)
        row(u, "Elimina", NULL, DELETE_ITEM, 0, NULL, NULL);
}
static void collection_text(const cJSON *bins, char *out, size_t size)
{
    out[0] = 0;
    const cJSON *b;
    cJSON_ArrayForEach(b, bins)
    {
        size_t n = strlen(out);
        if (n < size)
            snprintf(out + n, size - n, "%s%s", n ? ", " : "", life_string(b, "name"));
    }
    if (!out[0])
        snprintf(out, size, "Nessun ritiro");
}
static void render(life_ui_t *u)
{
    if (!u->active)
        return;
    int old_y = u->rendered_view == u->view ? lv_obj_get_scroll_y(u->page) : 0;
    u->rendered_view = u->view;
    lv_obj_clean(u->page);
    ui_title(u->page, name(u));
    if (u->view == DASHBOARD)
    {
        ui_text(u->page, "Dashboard privata sulla rete locale.", true);
        appkit_dashboard_page(u->page, ui_color(accent(u)));
        return;
    }
    cJSON *s = life_snapshot(u->kind);
    if (!s)
    {
        ui_text(u->page, "Archivio non disponibile. Nessun dato sovrascritto.", true);
        return;
    }
    if (life_string(s, "save_error")[0])
    {
        ui_text(u->page, life_string(s, "save_error"), false);
        row(u, "Riprova salvataggio", NULL, RETRY, 0, NULL, NULL);
    }
    else if (cJSON_IsTrue(get(s, "saving")))
        ui_text(u->page, "Salvataggio in corso...", true);
    if (u->view == FORM)
    {
        render_form(u, s);
        cJSON_Delete(s);
        lv_obj_update_layout(u->page);
        lv_obj_scroll_to_y(u->page, old_y, LV_ANIM_OFF);
        return;
    }
    if (u->view == HOME)
    {
        if (u->kind == LIFE_RACCOLTA)
        {
            const cJSON *week = get(get(s, "runtime"), "week");
            char b[200];
            for (int i = 0; i < 2; i++)
            {
                const cJSON *d = cJSON_GetArrayItem(week, i);
                ui_section(u->page, i ? "Domani" : "Oggi");
                collection_text(get(d, "bins"), b, sizeof(b));
                ui_text(u->page, b, false);
            }
            ui_section(u->page, "Le tue citta");
            const cJSON *c;
            cJSON_ArrayForEach(c, get(s, "cities"))
                row(u, life_string(c, "name"),
                    life_number(c, "id", 0) == life_number(s, "preferred", 0) ? "Preferita" : NULL, OPEN_CITY,
                    life_number(c, "id", 0), NULL, NULL);
            row(u, "Aggiungi citta", NULL, NEW, 0, "city", NULL);
            row(u, "Calendario Civita Castellana", "Domestico SATE", PRESET, 0, NULL, NULL);
            row(u, "Domani nel menu", cJSON_IsTrue(get(s, "menu_hint")) ? "Si" : "No", HINT, 0, NULL, NULL);
        }
        else
        {
            int due = life_number(get(s, "runtime"), "due", 0);
            char n[64];
            snprintf(n, sizeof(n), "%d attivita da completare", due);
            ui_text(u->page, n, false);
            const cJSON *p;
            cJSON_ArrayForEach(p, get(s, "pets"))
                row(u, life_string(p, "name"), life_string(p, "breed"), OPEN_PET, life_number(p, "id", 0), NULL, NULL);
            if (!cJSON_GetArraySize(get(s, "pets")))
                ui_text(u->page, "Crea un profilo per organizzare cure, routine e ricordi.", true);
            row(u, "Aggiungi cane", NULL, NEW, 0, "pet", NULL);
        }
        row(u, "Dashboard", LV_SYMBOL_WIFI, DASH, 0, NULL, NULL);
    }
    else if (u->view == CITY)
    {
        const cJSON *c = life_find(get(s, "cities"), u->city);
        ui_section(u->page, life_string(c, "name"));
        ui_text(u->page, life_string(c, "zone"), true);
        row(u, "Modifica citta", NULL, EDIT, u->city, "city", NULL);
        row(u, "Usa come preferita", NULL, PREFERRED, 0, NULL, NULL);
        const cJSON *b;
        cJSON_ArrayForEach(b, get(c, "bins"))
            row(u, life_string(b, "name"), NULL, EDIT, life_number(b, "id", 0), "bin", NULL);
        row(u, "Aggiungi raccolta", NULL, NEW, 0, "bin", NULL);
        ui_section(u->page, "Eccezioni per data");
        int i = 0;
        cJSON_ArrayForEach(b, get(c, "exceptions")) row(u, life_string(b, "date"), NULL, EDIT, ++i, "exception", NULL);
        row(u, "Aggiungi eccezione", NULL, NEW, 0, "exception", NULL);
    }
    else if (u->view == PET)
    {
        const cJSON *p = life_find(get(s, "pets"), u->pet);
        ui_section(u->page, life_string(p, "name"));
        row(u, "Profilo e contatti", NULL, EDIT, u->pet, "pet", NULL);
        const cJSON *t;
        ui_section(u->page, "Routine e scadenze");
        cJSON_ArrayForEach(t, get(s, "tasks")) if (life_number(t, "pet", 0) == u->pet)
        {
            int id = life_number(t, "id", 0);
            row(u, life_string(t, "label"), life_string(t, "due"), EDIT, id, "task", NULL);
            if (cJSON_IsTrue(get(t, "enabled")))
            {
                row(u, "Segna completata", life_string(t, "label"), DONE, id, NULL, NULL);
                row(u, "Rimanda di un giorno", NULL, POSTPONE, id, NULL, NULL);
            }
        }
        row(u, "Aggiungi attivita", NULL, NEW, 0, "task", NULL);
        ui_section(u->page, "Peso");
        cJSON_ArrayForEach(t, get(s, "weights")) if (life_number(t, "pet", 0) == u->pet)
        {
            char v[40];
            snprintf(v, sizeof(v), "%d g", life_number(t, "grams", 0));
            row(u, life_string(t, "date"), v, EDIT, life_number(t, "id", 0), "weight", NULL);
        }
        row(u, "Registra peso", NULL, NEW, 0, "weight", NULL);
        ui_section(u->page, "Diario");
        int shown = 0;
        const cJSON *logs = get(s, "logs");
        for (int i = cJSON_GetArraySize(logs) - 1; i >= 0 && shown < u->log_limit; i--)
        {
            t = cJSON_GetArrayItem(logs, i);
            if (life_number(t, "pet", 0) == u->pet)
            {
                row(u, life_string(t, "label"), life_string(t, "date"), EDIT, life_number(t, "id", 0), "log", NULL);
                shown++;
            }
        }
        if (shown >= u->log_limit)
            row(u, "Mostra altre registrazioni", NULL, MORE_LOGS, 0, NULL, NULL);
        row(u, "Aggiungi nota", NULL, NEW, 0, "log", NULL);
        ui_text(u->page, "Il piano sanitario va concordato con il veterinario.", true);
    }
    cJSON_Delete(s);
    lv_obj_update_layout(u->page);
    lv_obj_scroll_to_y(u->page, old_y, LV_ANIM_OFF);
}
static void create_kind(life_kind_t kind, lv_obj_t *screen)
{
    life_ui_t *u = &screens[kind];
    u->kind = kind;
    u->active = true;
    u->view = HOME;
    u->rendered_view = -1;
    u->log_limit = 20;
    u->city = u->pet = 0;
    u->generation++;
    u->opened = life_open(kind) == ESP_OK;
    appkit_web_start(life_web_config(kind));
    u->page = ui_page(screen, NULL);
    render(u);
}
static void close_kind(life_kind_t kind)
{
    life_ui_t *u = &screens[kind];
    u->active = false;
    u->generation++;
    cJSON_Delete(u->draft);
    u->draft = NULL;
    u->editing = NULL;
    appkit_web_stop_if(life_web_config(kind));
    if (u->opened)
        life_close(kind);
    u->opened = false;
    u->page = NULL;
}
static bool back_kind(life_kind_t kind)
{
    life_ui_t *u = &screens[kind];
    if (u->view == HOME)
        return false;
    u->generation++;
    u->view = u->view == FORM && u->entity != 0 && u->entity != 3 ? (kind == LIFE_RACCOLTA ? CITY : PET) : HOME;
    render(u);
    return true;
}
static void waste_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    create_kind(LIFE_RACCOLTA, screen);
}
static void dogs_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    create_kind(LIFE_ZAMPE, screen);
}
static void waste_close(void)
{
    close_kind(LIFE_RACCOLTA);
}
static void dogs_close(void)
{
    close_kind(LIFE_ZAMPE);
}
static bool waste_back(void)
{
    return back_kind(LIFE_RACCOLTA);
}
static bool dogs_back(void)
{
    return back_kind(LIFE_ZAMPE);
}
const app_t raccolta_app = {
    .id = "raccolta",
    .name = "Raccolta",
    .icon = LV_SYMBOL_REFRESH,
    .color = 0x70D5B0,
    .create = waste_create,
    .destroy = waste_close,
    .back = waste_back,
    .about = "Calendari della raccolta per piu citta, materiali configurabili, eccezioni e promemoria nel menu."};
const app_t zampe_app = {
    .id = "zampe",
    .name = "Zampe",
    .icon = LV_SYMBOL_HOME,
    .color = 0xEDAD82,
    .create = dogs_create,
    .destroy = dogs_close,
    .back = dogs_back,
    .about = "Profili privati dei cani, routine, scadenze, peso e diario. Dashboard locale e notifiche facoltative."};
