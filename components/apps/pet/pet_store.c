/* Durable device-owned pets. No background task, no periodic flash writes.
 * Transfers: reserve source -> receive inactive destination -> depart source ->
 * arrive destination -> acknowledge source. Every step is durable and idempotent
 * by transaction id. A departure receipt is not reused until acknowledged. */
#include "core/sys.h"
#include "esp_attr.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hardware/board.h"
#include "hardware/imu.h"
#include "hardware/microphone.h"
#include "nvs.h"
#include "orbi/orbi_genome.h"
#include "orbi/pet_sprite.h"
#include "pet.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PET_SLOTS 8
#define PET_MAGIC 0x50455401u
typedef struct
{
    uint32_t magic, company;
    char native[17], selected[17];
    pet_record_t pets[PET_SLOTS];
} pet_store_t;
/* PSRAM: internal RAM is kept for the Bluetooth controller. */
static EXT_RAM_BSS_ATTR pet_store_t s_store;
static SemaphoreHandle_t s_lock;
/* Discovered breeds (docs/ORBI-PETS.md), a separate blob: the store format is unchanged. */
#define DEX_MAX 64
#define DEX_MAGIC 0x44455831u
typedef struct
{
    uint32_t magic;
    uint16_t count;
    uint16_t keys[DEX_MAX];
} dex_t;
static EXT_RAM_BSS_ATTR dex_t s_dex;
static char s_arrived[4][17];   /* arrivals the page has not animated yet */
static bool s_ready;

static int64_t clock_now(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (int64_t)t : 0;
}
static bool save(void)
{
    nvs_handle_t h;
    if (nvs_open("orbi_children", NVS_READWRITE, &h) != ESP_OK)
        return false;
    esp_err_t e = nvs_set_blob(h, "state", &s_store, sizeof(s_store));
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}
static pet_look_t look_of(const pet_record_t *p)
{
    uint64_t seed = 0;
    orbi_parse(p->seed, &seed);
    pet_look_t l;
    pet_look(seed, pet_level(p->xp), &l);
    return l;
}
/* Lock held. Saves only when the breed is new. */
static void dex_add(const pet_record_t *p)
{
    if (!p->seed[0])
        return;
    pet_look_t l = look_of(p);
    uint16_t key = pet_breed_key(&l);
    for (int i = 0; i < s_dex.count; i++)
        if (s_dex.keys[i] == key)
            return;
    if (s_dex.count >= DEX_MAX)
        return;
    s_dex.keys[s_dex.count++] = key;
    nvs_handle_t h;
    if (nvs_open("orbi_children", NVS_READWRITE, &h) == ESP_OK)
    {
        if (nvs_set_blob(h, "dex", &s_dex, sizeof(s_dex)) == ESP_OK)
            nvs_commit(h);
        nvs_close(h);
    }
}
static void evolve(pet_record_t *p)
{
    int64_t now = clock_now();
    if (!now)
        return;
    if (!p->born)
        p->born = now;
    if (!p->updated || p->updated > now)
    {
        p->updated = now;
        return;
    }
    int64_t elapsed = (now - p->updated) / 180;
    int32_t steps = elapsed > 100 ? 100 : (int32_t)elapsed;
    if (!steps || p->stage != PET_ACTIVE)
        return;
    if (steps > 100)
        steps = 100;
    p->hunger = p->hunger + steps > 100 ? 100 : p->hunger + steps;
    p->joy = p->joy < steps ? 0 : p->joy - steps;
    if (p->asleep)
    {
        p->energy = p->energy + steps * 6 > 100 ? 100 : p->energy + steps * 6;
    }
    else
    {
        p->energy = p->energy < steps ? 0 : p->energy - steps;
    }
    if (p->energy == 100)
        p->asleep = 0;
    p->updated = now - (now - p->updated) % 180;
}
static pet_record_t *find(const char *seed)
{
    for (int i = 0; seed && i < PET_SLOTS; i++)
        if (!strcmp(seed, s_store.pets[i].seed))
            return &s_store.pets[i];
    return NULL;
}
bool pet_init(void)
{
    if (s_ready)
        return true;
    if (!s_lock)
        s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t h;
    size_t size = sizeof(s_store);
    esp_err_t e = nvs_open("orbi_children", NVS_READWRITE, &h);
    if (e == ESP_OK)
    {
        e = nvs_get_blob(h, "state", &s_store, &size);
        nvs_close(h);
    }
    if (e == ESP_ERR_NVS_NOT_FOUND)
    {
        memset(&s_store, 0, sizeof(s_store));
        s_store.magic = PET_MAGIC;
        s_store.company = 1;
        const board_info_t *b = board_info();
        uint8_t mac[6];
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK)
        {
            xSemaphoreGive(s_lock);
            return false;
        }
        uint8_t caps = (b->round ? 1 : 2) | (microphone_supported() ? 4 : 0) | (b->has_audio ? 8 : 0) |
                       (imu_supported() ? 16 : 0) | (b->has_knob ? 32 : 0) | (b->has_camera ? 64 : 0);
        /* Device model and MAC define the family. Birthday distinguishes siblings. */
        uint64_t seed = orbi_seed(caps, mac, b->name, 0, clock_now() ? (uint32_t)clock_now() : esp_random());
        orbi_code(seed, s_store.native);
        snprintf(s_store.selected, sizeof(s_store.selected), "%s", s_store.native);
        pet_record_t *p = &s_store.pets[0];
        snprintf(p->seed, sizeof(p->seed), "%s", s_store.native);
        snprintf(p->origin, sizeof(p->origin), "%s", sys_nickname());
        p->hunger = 20;
        p->energy = 90;
        p->joy = 70;
        p->born = p->updated = clock_now();
        s_ready = save();
    }
    else
    {
        s_ready = e == ESP_OK && size == sizeof(s_store) && s_store.magic == PET_MAGIC;
    }
    if (s_ready)
    {
        size = sizeof(s_dex);
        e = nvs_open("orbi_children", NVS_READWRITE, &h);
        if (e == ESP_OK)
        {
            e = nvs_get_blob(h, "dex", &s_dex, &size);
            nvs_close(h);
        }
        if (e != ESP_OK || size != sizeof(s_dex) || s_dex.magic != DEX_MAGIC || s_dex.count > DEX_MAX)
        {
            memset(&s_dex, 0, sizeof(s_dex));
            s_dex.magic = DEX_MAGIC;
        }
        for (int i = 0; i < PET_SLOTS; i++)
            if (s_store.pets[i].stage == PET_ACTIVE)
                dex_add(&s_store.pets[i]);
    }
    xSemaphoreGive(s_lock);
    return s_ready;
}
static cJSON *record_json(pet_record_t *p)
{
    evolve(p);
    uint64_t seed;
    orbi_genome_t g;
    orbi_parse(p->seed, &seed);
    orbi_genome(seed, &g);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "seed", p->seed);
    cJSON_AddStringToObject(j, "name", g.name);
    cJSON_AddStringToObject(j, "origin", p->origin);
    cJSON_AddStringToObject(j, "transfer", p->transfer);
    cJSON_AddNumberToObject(j, "stage", p->stage);
    cJSON_AddNumberToObject(j, "hunger", p->hunger);
    cJSON_AddNumberToObject(j, "energy", p->energy);
    cJSON_AddNumberToObject(j, "joy", p->joy);
    cJSON_AddNumberToObject(j, "xp", p->xp);
    pet_look_t l = look_of(p);
    cJSON_AddStringToObject(j, "species", pet_species_name(l.species));
    cJSON_AddStringToObject(j, "breed", pet_variant_name(&l));
    cJSON_AddNumberToObject(j, "level", l.level);
    cJSON_AddStringToObject(j, "stage_name", pet_stage_name(l.stage));
    cJSON_AddBoolToObject(j, "asleep", p->asleep);
    cJSON_AddNumberToObject(j, "born", (double)p->born);
    cJSON_AddNumberToObject(j, "updated", (double)p->updated);
    return j;
}
static cJSON *snapshot_locked(void)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "version", 1);
    cJSON_AddBoolToObject(out, "company", s_store.company != 0);
    cJSON_AddStringToObject(out, "native", s_store.native);
    cJSON_AddStringToObject(out, "selected", s_store.selected);
    cJSON *list = cJSON_AddArrayToObject(out, "pets");
    for (int i = 0; i < PET_SLOTS; i++)
        if (s_store.pets[i].seed[0])
            cJSON_AddItemToArray(list, record_json(&s_store.pets[i]));
    cJSON *dex = cJSON_AddArrayToObject(out, "dex");
    for (int i = 0; i < s_dex.count; i++)
        cJSON_AddItemToArray(dex, cJSON_CreateNumber(s_dex.keys[i]));
    return out;
}
cJSON *pet_snapshot(void)
{
    if (!s_ready)
        return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *out = snapshot_locked();
    xSemaphoreGive(s_lock);
    return out;
}
bool pet_current(pet_record_t *out)
{
    if (!s_ready)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pet_record_t *p = find(s_store.selected);
    if (!p || p->stage != PET_ACTIVE)
    {
        p = NULL;
        for (int i = 0; i < PET_SLOTS; i++)
            if (s_store.pets[i].seed[0] && s_store.pets[i].stage == PET_ACTIVE)
            {
                p = &s_store.pets[i];
                break;
            }
    }
    if (p)
    {
        evolve(p);
        *out = *p;
    }
    xSemaphoreGive(s_lock);
    return p != NULL;
}
static const char *text(const cJSON *j, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, key));
    return s ? s : "";
}
static bool number(const cJSON *j, const char *key, int64_t max, int64_t *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 || v->valuedouble > (double)max ||
        floor(v->valuedouble) != v->valuedouble)
        return false;
    *out = (int64_t)v->valuedouble;
    return true;
}
cJSON *pet_command(const cJSON *j, const char **error)
{
    *error = "Pet non disponibile o comando non valido";
    uint64_t parsed;
    const char *seed = text(j, "seed"), *action = text(j, "action"), *tx = text(j, "transfer");
    if (!s_ready || !orbi_parse(seed, &parsed) || strlen(seed) != 16)
        return NULL;
    char canonical[17];
    orbi_code(parsed, canonical);
    if (strcmp(seed, canonical))
        return NULL;
    bool transfer = !strcmp(action, "reserve") || !strcmp(action, "receive") || !strcmp(action, "depart") ||
                    !strcmp(action, "arrive") || !strcmp(action, "cancel") || !strcmp(action, "forget");
    if (transfer && (strlen(tx) < 16 || strlen(tx) > 40 || strspn(tx, "0123456789abcdef-") != strlen(tx)))
        return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pet_store_t before = s_store;
    pet_record_t *p = find(seed);
    bool ok = false;
    if (!strcmp(action, "forget"))
    {
        if (!p || strcmp(p->transfer, tx))
            ok = true;
        else if (p->stage == PET_DEPARTED)
        {
            memset(p, 0, sizeof(*p));
            ok = true;
        }
    }
    else if (!strcmp(action, "cancel"))
    {
        if (!p || (p->stage == PET_ACTIVE && strcmp(p->transfer, tx)))
            ok = true;
        else if (!strcmp(p->transfer, tx) && p->stage == PET_INCOMING)
        {
            memset(p, 0, sizeof(*p));
            ok = true;
        }
        else if (!strcmp(p->transfer, tx) && p->stage == PET_OUTGOING)
        {
            p->stage = PET_ACTIVE;
            p->transfer[0] = 0;
            ok = true;
        }
    }
    else if (!strcmp(action, "receive"))
    {
        if (p && !strcmp(p->transfer, tx) && (p->stage == PET_INCOMING || p->stage == PET_ACTIVE))
            ok = true;
        else if (!p || (p->stage == PET_DEPARTED && strcmp(p->transfer, tx)))
        {
            /* A pet coming back home replaces its own old departure receipt. */
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "pet");
            pet_record_t incoming = {0};
            int64_t n = 0;
            bool valid = cJSON_IsObject(v) && !strcmp(text(v, "seed"), seed) &&
                         strlen(text(v, "origin")) < sizeof(incoming.origin);
            const char *keys[] = {"hunger", "energy", "joy", "xp", "born", "updated"};
            int64_t values[6];
            for (int i = 0; i < 6; i++)
            {
                if (!number(v, keys[i], i < 3 ? 100 : i == 3 ? 100000000 : 4102444800LL, &n))
                    valid = false;
                values[i] = n;
            }
            if (valid)
            {
                incoming.hunger = (int32_t)values[0];
                incoming.energy = (int32_t)values[1];
                incoming.joy = (int32_t)values[2];
                incoming.xp = (int32_t)values[3];
                incoming.born = values[4];
                incoming.updated = values[5];
                incoming.asleep = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v, "asleep"));
                incoming.stage = PET_INCOMING;
                snprintf(incoming.seed, sizeof(incoming.seed), "%s", seed);
                snprintf(incoming.origin, sizeof(incoming.origin), "%s", text(v, "origin"));
                snprintf(incoming.transfer, sizeof(incoming.transfer), "%s", tx);
                if (!p)
                    for (int i = 0; i < PET_SLOTS; i++)
                        if (!s_store.pets[i].seed[0])
                        {
                            p = &s_store.pets[i];
                            break;
                        }
                if (p)
                {
                    *p = incoming;
                    ok = true;
                }
                else
                    *error = "Il device ospita gia troppi pet";
            }
        }
    }
    else if (p && transfer)
    {
        bool same = !strcmp(p->transfer, tx);
        if (!strcmp(action, "reserve") && p->stage == PET_DEPARTED && same)
            ok = true;
        if (!strcmp(action, "reserve") && (p->stage == PET_ACTIVE || (p->stage == PET_OUTGOING && same)))
        {
            evolve(p);
            p->stage = PET_OUTGOING;
            snprintf(p->transfer, sizeof(p->transfer), "%s", tx);
            ok = true;
        }
        if (!strcmp(action, "depart") && same && (p->stage == PET_OUTGOING || p->stage == PET_DEPARTED))
        {
            p->stage = PET_DEPARTED;
            ok = true;
        }
        if (!strcmp(action, "arrive") && same && (p->stage == PET_INCOMING || p->stage == PET_ACTIVE))
        {
            if (p->stage == PET_INCOMING)
                for (int i = 0; i < 4; i++)
                    if (!s_arrived[i][0])
                    {
                        snprintf(s_arrived[i], sizeof(s_arrived[i]), "%s", seed);
                        break;
                    }
            p->stage = PET_ACTIVE;
            p->updated = clock_now();
            dex_add(p);
            snprintf(s_store.selected, sizeof(s_store.selected), "%s", seed);
            ok = true;
        }
    }
    else if (p && p->stage == PET_ACTIVE)
    {
        evolve(p);
        if (!strcmp(action, "select"))
        {
            snprintf(s_store.selected, sizeof(s_store.selected), "%s", seed);
            ok = true;
        }
        else if (!strcmp(action, "company"))
        {
            s_store.company = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "on"));
            ok = true;
        }
        else if (!strcmp(action, "sleep"))
        {
            p->asleep = !p->asleep;
            ok = true;
        }
        else if (!p->asleep && !strcmp(action, "feed"))
        {
            p->hunger = p->hunger > 35 ? p->hunger - 35 : 0;
            p->joy = p->joy < 94 ? p->joy + 6 : 100;
            p->xp += 6;
            ok = true;
        }
        else if (!p->asleep && !strcmp(action, "play") && p->energy >= 10)
        {
            p->energy -= 10;
            p->joy = p->joy < 78 ? p->joy + 22 : 100;
            p->xp += 12;
            ok = true;
        }
        else if (!p->asleep && !strcmp(action, "wash"))
        {
            p->joy = p->joy < 92 ? p->joy + 8 : 100;
            p->xp += 4;
            ok = true;
        }
        else if (!p->asleep && !strcmp(action, "snack"))
        {
            p->hunger = p->hunger > 12 ? p->hunger - 12 : 0;
            p->joy = p->joy < 90 ? p->joy + 10 : 100;
            p->xp += 2;
            ok = true;
        }
        else if (!p->asleep && !strcmp(action, "train") && p->energy >= 20)
        {
            p->energy -= 15;
            p->xp += 18;
            ok = true;
        }
        else if (!p->asleep && !strcmp(action, "meet"))
        {
            p->joy = p->joy < 96 ? p->joy + 4 : 100;
            p->xp += 2;
            ok = true;
        }
        else if (!strcmp(action, "pet"))
        {
            p->joy = p->joy < 91 ? p->joy + 9 : 100;
            p->xp += 3;
            ok = true;
        }
        if (p->xp > 100000000)
            p->xp = 100000000;
    }
    cJSON *out = NULL;
    if (ok && save())
        out = snapshot_locked();
    else
    {
        s_store = before;
        if (ok)
            *error = "Salvataggio fallito: riprova";
    }
    xSemaphoreGive(s_lock);
    return out;
}
bool pet_select(const char *seed)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "seed", seed);
    cJSON_AddStringToObject(j, "action", "select");
    const char *error;
    cJSON *out = pet_command(j, &error);
    bool ok = out != NULL;
    cJSON_Delete(j);
    cJSON_Delete(out);
    return ok;
}
bool pet_company(void)
{
    if (!s_ready)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_store.company != 0;
    xSemaphoreGive(s_lock);
    return on;
}

bool pet_take_arrival(const char *seed)
{
    if (!s_ready)
        return false;
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < 4; i++)
        if (s_arrived[i][0] && !strcmp(s_arrived[i], seed))
        {
            s_arrived[i][0] = 0;
            found = true;
        }
    xSemaphoreGive(s_lock);
    return found;
}
int pet_dex(uint16_t *keys, int max)
{
    if (!s_ready)
        return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_dex.count < max ? s_dex.count : max;
    memcpy(keys, s_dex.keys, (size_t)n * sizeof(uint16_t));
    xSemaphoreGive(s_lock);
    return n;
}
int pet_list(pet_record_t *out, int max)
{
    if (!s_ready)
        return 0;
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PET_SLOTS && n < max; i++)
        if (s_store.pets[i].seed[0] && s_store.pets[i].stage == PET_ACTIVE)
        {
            evolve(&s_store.pets[i]);
            out[n++] = s_store.pets[i];
        }
    xSemaphoreGive(s_lock);
    return n;
}
void pet_traits(const pet_record_t *p, uint8_t out[5])
{
    uint64_t seed = 0;
    orbi_parse(p->seed, &seed);
    orbi_genome_t g;
    orbi_genome(seed, &g);
    out[0] = g.curiosity;
    out[1] = g.energy;
    out[2] = g.sociability;
    out[3] = g.appetite;
    out[4] = g.playfulness;
}
int pet_list_all(pet_record_t *out, int max)
{
    if (!s_ready)
        return 0;
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PET_SLOTS && n < max; i++)
        if (s_store.pets[i].seed[0])
            out[n++] = s_store.pets[i];
    xSemaphoreGive(s_lock);
    return n;
}
