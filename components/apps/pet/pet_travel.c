/* Pet journeys between paired devices over ESP-NOW (docs/ORBI-PETS-ROADMAP.md,
 * phase 1). The steps are the store's own (reserve, receive, depart, arrive,
 * forget, cancel), carried by the Famiglia link as compact sealed frames:
 *
 *   step   [op][transfer 8][seed 8] + receive: [hunger][energy][joy][asleep]
 *          [xp 4][born 4][updated 4][origin length][origin]   (little endian)
 *   reply  [0 ok | 1 refused][error text]
 *
 * The sender drives the journey in a short-lived task. Once the pet has left
 * (depart) it only goes forward: the arrival is retried, and resumed from the
 * "trip" blob after a restart. */
#include "apps_internal.h"
#include "core/lv_port.h"
#include "core/sys.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "orbi/orbi_genome.h"
#include "orbi/pet_sprite.h"
#include "pet.h"
#include "services/ecos.h"
#include "ui/ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pet_travel";

enum { OP_RECEIVE = 1, OP_ARRIVE = 2, OP_CANCEL = 3, OP_INFO = 4 };

typedef struct
{
    uint32_t magic;
    char id[13];
    char seed[17];
    char transfer[17];
} trip_t;
#define TRIP_MAGIC 0x54524950u

static volatile bool s_busy;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void to_hex(const uint8_t *b, char out[17])
{
    for (int i = 0; i < 8; i++)
        snprintf(out + i * 2, 3, "%02x", b[i]);
}

static bool from_hex(const char *s, uint8_t b[8])
{
    uint64_t v;
    if (!orbi_parse(s, &v))
        return false;
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)(v >> (56 - i * 8));
    return true;
}

static void toast(const char *text)
{
    lv_port_lock();
    ui_toast(text);
    lv_port_unlock();
}

/* One store command; returns true on success. */
static bool local(const char *action, const char *seed, const char *transfer, cJSON *pet, const char **error)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "action", action);
    cJSON_AddStringToObject(j, "seed", seed);
    if (transfer)
        cJSON_AddStringToObject(j, "transfer", transfer);
    if (pet)
        cJSON_AddItemToObject(j, "pet", pet);
    const char *e = NULL;
    cJSON *out = pet_command(j, &e);
    if (error)
        *error = e;
    bool ok = out != NULL;
    cJSON_Delete(out);
    cJSON_Delete(j);
    return ok;
}

/* ------------------------------------------------------- receiving side */

static size_t on_step(const char *from_id, const char *from_name, const uint8_t *s, size_t n, uint8_t *reply,
                      size_t max)
{
    (void)from_id;
    if (n >= 1 && s[0] == OP_INFO)
    {
        /* Who lives here: [0][species][name]. */
        pet_record_t p;
        if (!pet_current(&p) || max < 4)
            return 0;
        uint64_t seed = 0;
        orbi_parse(p.seed, &seed);
        orbi_genome_t g;
        orbi_genome(seed, &g);
        pet_look_t l;
        pet_look(seed, pet_level(p.xp), &l);
        reply[0] = 0;
        reply[1] = l.species;
        return 2 + (size_t)snprintf((char *)reply + 2, max - 2, "%s", g.name);
    }
    if (n < 17 || max < 2)
        return 0;
    char tx[17], seed[17];
    to_hex(s + 1, tx);
    to_hex(s + 9, seed);
    const char *error = "Passo non valido";
    bool ok = false;
    if (s[0] == OP_RECEIVE && n >= 34 && n >= 34u + s[33])
    {
        cJSON *pet = cJSON_CreateObject();
        char origin[41];
        size_t len = s[33] < sizeof(origin) - 1 ? s[33] : sizeof(origin) - 1;
        memcpy(origin, s + 34, len);
        origin[len] = 0;
        cJSON_AddStringToObject(pet, "seed", seed);
        cJSON_AddStringToObject(pet, "origin", origin);
        cJSON_AddNumberToObject(pet, "hunger", s[17]);
        cJSON_AddNumberToObject(pet, "energy", s[18]);
        cJSON_AddNumberToObject(pet, "joy", s[19]);
        cJSON_AddBoolToObject(pet, "asleep", s[20] != 0);
        cJSON_AddNumberToObject(pet, "xp", get32(s + 21));
        cJSON_AddNumberToObject(pet, "born", get32(s + 25));
        cJSON_AddNumberToObject(pet, "updated", get32(s + 29));
        ok = local("receive", seed, tx, pet, &error);
    }
    else if (s[0] == OP_ARRIVE)
        ok = local("arrive", seed, tx, NULL, &error);
    else if (s[0] == OP_CANCEL)
        ok = local("cancel", seed, tx, NULL, &error);
    ESP_LOGI(TAG, "step %d from %s: %s", s[0], from_name, ok ? "ok" : error ? error : "refused");
    reply[0] = ok ? 0 : 1;
    size_t len = 1;
    if (!ok && error)
    {
        len += snprintf((char *)reply + 1, max - 1, "%s", error);
        if (len > max)
            len = max;
    }
    return len;
}

/* --------------------------------------------------------- sending side */

static bool trip_save(const trip_t *t)
{
    nvs_handle_t h;
    if (nvs_open("orbi_children", NVS_READWRITE, &h) != ESP_OK)
        return false;
    esp_err_t e = t ? nvs_set_blob(h, "trip", t, sizeof(*t)) : nvs_erase_key(h, "trip");
    if (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND)
        e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

static bool trip_load(trip_t *t)
{
    nvs_handle_t h;
    size_t size = sizeof(*t);
    if (nvs_open("orbi_children", NVS_READWRITE, &h) != ESP_OK)
        return false;
    esp_err_t e = nvs_get_blob(h, "trip", t, &size);
    nvs_close(h);
    return e == ESP_OK && size == sizeof(*t) && t->magic == TRIP_MAGIC;
}

/* Sends one step, a few attempts; returns 0 ok, 1 refused, -1 unreachable. */
static int send_step(const char *id, const uint8_t *step, size_t n, int attempts, char *why, size_t why_size)
{
    uint8_t reply[ECOS_PET_MAX];
    for (int i = 0; i < attempts; i++)
    {
        int r = ecos_family_pet(id, step, n, reply, sizeof(reply), 8000);
        if (r >= 1)
        {
            if (reply[0] != 0 && why)
                snprintf(why, why_size, "%.*s", r - 1, (const char *)reply + 1);
            return reply[0] == 0 ? 0 : 1;
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    return -1;
}

static void header(uint8_t *s, int op, const trip_t *t)
{
    s[0] = (uint8_t)op;
    from_hex(t->transfer, s + 1);
    from_hex(t->seed, s + 9);
}

/* After depart: arrive (retried), then forget the receipt here. */
static bool finish(const trip_t *t)
{
    uint8_t s[17];
    header(s, OP_ARRIVE, t);
    if (send_step(t->id, s, sizeof(s), 6, NULL, 0) != 0)
        return false;
    local("forget", t->seed, t->transfer, NULL, NULL);
    trip_save(NULL);
    return true;
}

static void travel_task(void *arg)
{
    trip_t *t = arg;
    const char *error = NULL;
    char why[64] = "";
    uint8_t s[ECOS_PET_MAX];
    pet_record_t list[8], p = {0};
    bool found = false;

    if (t->transfer[0])
    {
        /* Resume: the pet already left. */
        if (finish(t))
            toast("Viaggio completato: il child e arrivato");
        goto done;
    }
    uint8_t rnd[8];
    esp_fill_random(rnd, sizeof(rnd));
    to_hex(rnd, t->transfer);
    t->magic = TRIP_MAGIC;
    if (!local("reserve", t->seed, t->transfer, NULL, &error))
    {
        toast(error ? error : "Il child non puo partire ora");
        goto done;
    }
    /* reserve keeps the record listed (outgoing): read its state for the snapshot. */
    int n = pet_list_all(list, 8);
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].seed, t->seed))
            p = list[i], found = true;
    if (!found)
    {
        local("cancel", t->seed, t->transfer, NULL, NULL);
        toast("Child non trovato");
        goto done;
    }
    header(s, OP_RECEIVE, t);
    s[17] = (uint8_t)p.hunger, s[18] = (uint8_t)p.energy, s[19] = (uint8_t)p.joy, s[20] = p.asleep ? 1 : 0;
    put32(s + 21, (uint32_t)p.xp);
    put32(s + 25, (uint32_t)p.born);
    put32(s + 29, (uint32_t)p.updated);
    size_t olen = strnlen(p.origin, 40);
    s[33] = (uint8_t)olen;
    memcpy(s + 34, p.origin, olen);
    int r = send_step(t->id, s, 34 + olen, 3, why, sizeof(why));
    if (r != 0)
    {
        uint8_t c[17];
        header(c, OP_CANCEL, t);
        send_step(t->id, c, sizeof(c), 1, NULL, 0);
        local("cancel", t->seed, t->transfer, NULL, NULL);
        toast(r < 0 ? "Amico non raggiungibile: apri la pagina del child anche sull'altro device" : why[0] ? why
                                                                                                    : "Viaggio rifiutato");
        goto done;
    }
    /* From here on only forward: save the trip before the pet leaves. */
    if (!trip_save(t) || !local("depart", t->seed, t->transfer, NULL, &error))
    {
        uint8_t c[17];
        header(c, OP_CANCEL, t);
        send_step(t->id, c, sizeof(c), 1, NULL, 0);
        local("cancel", t->seed, t->transfer, NULL, NULL);
        trip_save(NULL);
        toast(error ? error : "Salvataggio fallito: viaggio annullato");
        goto done;
    }
    toast(finish(t) ? "Buon viaggio! Il child e arrivato dall'amico"
                    : "Viaggio in pausa: riavvicina i device e riapri la pagina del child");
done:
    free(t);
    s_busy = false;
    vTaskDelete(NULL);
}

static bool start(trip_t *t)
{
    if (s_busy)
    {
        free(t);
        return false;
    }
    s_busy = true;
    if (!sys_task_create(travel_task, "pet_travel", 6144, t, 4, 0))
    {
        s_busy = false;
        free(t);
        return false;
    }
    return true;
}

bool pet_travel_send(const char *id, const char *seed)
{
    trip_t *t = calloc(1, sizeof(*t));
    if (!t)
        return false;
    snprintf(t->id, sizeof(t->id), "%s", id);
    snprintf(t->seed, sizeof(t->seed), "%s", seed);
    return start(t);
}

void pet_travel_resume(void)
{
    trip_t *t = calloc(1, sizeof(*t));
    if (!t)
        return;
    if (!trip_load(t))
    {
        free(t);
        return;
    }
    start(t);
}

bool pet_travel_busy(void)
{
    return s_busy;
}

void pet_travel_init(void)
{
    ecos_family_set_pet_handler(on_step);
}

/* ------------------------------------------------------------- outing */

static pet_friend_t s_friends[PET_FRIENDS_MAX];
static int s_friend_count;
static pet_outing_cb_t s_outing_cb;

/* Radio on, search the paired devices, ask the reachable ones who lives there. */
static void outing_task(void *arg)
{
    (void)arg;
    ecos_family_hold(true);
    ecos_family_refresh();
    ecos_family_t *f = heap_caps_malloc(sizeof(*f), MALLOC_CAP_SPIRAM);
    int count = 0;
    if (f)
    {
        for (int i = 0; i < 40; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(300));
            ecos_family(f);
            if (i > 3 && !f->busy)
                break;
        }
        for (int i = 0; i < f->count && count < PET_FRIENDS_MAX; i++)
        {
            if (!f->members[i].reachable)
                continue;
            pet_friend_t *fr = &s_friends[count];
            memset(fr, 0, sizeof(*fr));
            snprintf(fr->id, sizeof(fr->id), "%s", f->members[i].id);
            snprintf(fr->device, sizeof(fr->device), "%s", f->members[i].name);
            uint8_t q = OP_INFO, reply[48];
            int r = ecos_family_pet(fr->id, &q, 1, reply, sizeof(reply) - 1, 4000);
            if (r >= 2 && reply[0] == 0)
            {
                reply[r] = 0;
                fr->species = reply[1];
                snprintf(fr->pet, sizeof(fr->pet), "%.15s", (const char *)reply + 2);
            }
            count++;
        }
        free(f);
    }
    s_friend_count = count;
    ecos_family_hold(false);
    s_busy = false;
    lv_port_lock();
    if (s_outing_cb)
        s_outing_cb(s_friends, count);
    lv_port_unlock();
    vTaskDelete(NULL);
}

bool pet_outing_search(pet_outing_cb_t done)
{
    if (s_busy)
        return false;
    s_busy = true;
    s_outing_cb = done;
    if (!sys_task_create(outing_task, "pet_outing", 6144, NULL, 4, 0))
    {
        s_busy = false;
        return false;
    }
    return true;
}

void pet_outing_cancel(void)
{
    s_outing_cb = NULL;
}
