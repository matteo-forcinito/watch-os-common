/*
 * Pieno data (see pieno.h): download, filter, cache, history.
 *
 * The station list is read first: stations within the radius go into a new
 * table and a small id -> index hash. Then the price file fills that table.
 * Only when both files were read completely does the new table replace the
 * old one, so a broken download never empties the screen.
 */
#include "pieno.h"

#include "core/settings.h"
#include "core/sys.h"
#include "services/storage.h"
#include "services/time_sync.h"
#include "services/wifi.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "pieno";

#define STATIONS_URL "https://www.mimit.gov.it/images/exportCSV/anagrafica_impianti_attivi.csv"
#define PRICES_URL   "https://www.mimit.gov.it/images/exportCSV/prezzo_alle_8.csv"
#define CACHE_MAGIC  0x504E4F33u   /* "PNO3": bump when pn_station_t changes */
#define HASH_SIZE    2048          /* > 2 x PN_MAX_STATIONS, power of two */
#define PN_LINE_MAX     700
#define RETRY_US     (6LL * 3600 * 1000000)
#define CHANGES_MAX  (2 * 1024 * 1024)

const char *const pn_fuel_names[PN_FUELS] = {"Benzina", "Gasolio", "GPL", "Metano"};

typedef struct
{
    uint32_t magic;
    uint32_t count;
    char extraction[12];
    int64_t fetched;
    float lat;
    float lon;
    uint8_t radius_km;
} cache_header_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static pn_station_t *s_table;          /* PSRAM, PN_MAX_STATIONS */
static int s_count;
static pn_settings_t s_settings;
static pn_status_t s_status;
static pn_day_t *s_history;            /* PSRAM, PN_HISTORY_DAYS * 8 */
static int s_history_count;
static bool s_refresh_wanted;
static float s_cache_lat, s_cache_lon;
static uint8_t s_cache_radius;

/* ------------------------------------------------------------ locking */

void pn_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void pn_unlock(void)
{
    xSemaphoreGive(s_lock);
}

const pn_station_t *pn_stations(int *count)
{
    *count = s_count;
    return s_table;
}

void pn_status(pn_status_t *out)
{
    pn_lock();
    *out = s_status;
    pn_unlock();
}

static void status_set(pn_phase_t phase, int percent, const char *message)
{
    pn_lock();
    s_status.phase = phase;
    s_status.percent = percent;
    snprintf(s_status.message, sizeof(s_status.message), "%s", message);
    s_status.revision++;
    pn_unlock();
}

int64_t pn_local_now(void)
{
    time_t now = time(NULL);
    if (now < 1700000000)
    {
        return 0;   /* clock not set yet */
    }
    struct tm tm;
    localtime_r(&now, &tm);
    return pn_days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) * 86400 +
           tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

/* ------------------------------------------------------------ ranking */

bool pn_price_valid(const pn_station_t *s, int fuel, int self, const pn_settings_t *settings)
{
    if (s->price[fuel][self] <= 0 || s->km > settings->radius_km + 0.05f)
    {
        return false;   /* not sold, or outside a radius made smaller since the download */
    }
    int64_t now = pn_local_now();
    return settings->max_age_days == 0 || now == 0 || s->when[fuel][self] == 0 ||
           now - s->when[fuel][self] <= (int64_t)settings->max_age_days * 86400;
}

float pn_rank_price(const pn_station_t *s, int fuel, int self, const pn_settings_t *settings)
{
    float price = s->price[fuel][self];
    if (price <= 0)
    {
        return 0;
    }
    return settings->real_cost ? pn_real_price(price, s->km, settings->litres, settings->km_per_litre) : price;
}

int pn_rank(int fuel, int self, const pn_settings_t *settings, uint16_t *order, int max)
{
    /* Sort every valid station, then keep the first max (not the first max found). */
    uint16_t *all = malloc(sizeof(uint16_t) * PN_MAX_STATIONS);
    float *value = malloc(sizeof(float) * PN_MAX_STATIONS);
    if (all == NULL || value == NULL)
    {
        free(all);
        free(value);
        return 0;
    }
    int n = 0;
    for (int i = 0; i < s_count; i++)
    {
        if (pn_price_valid(&s_table[i], fuel, self, settings))
        {
            all[n] = (uint16_t)i;
            value[n] = pn_rank_price(&s_table[i], fuel, self, settings);
            n++;
        }
    }
    for (int i = 1; i < n; i++)
    {
        uint16_t key = all[i];
        float v = value[i];
        int j = i - 1;
        /* Same price: the nearer station first. */
        while (j >= 0 && (value[j] > v || (value[j] == v && s_table[all[j]].km > s_table[key].km)))
        {
            all[j + 1] = all[j];
            value[j + 1] = value[j];
            j--;
        }
        all[j + 1] = key;
        value[j + 1] = v;
    }
    n = n < max ? n : max;
    memcpy(order, all, sizeof(uint16_t) * n);
    free(all);
    free(value);
    return n;
}

bool pn_area(int fuel, int self, const pn_settings_t *settings, float *min, float *median, float *max, int *count)
{
    float *values = heap_caps_malloc(sizeof(float) * PN_MAX_STATIONS, MALLOC_CAP_SPIRAM);
    if (values == NULL)
    {
        return false;
    }
    int n = 0;
    for (int i = 0; i < s_count; i++)
    {
        if (pn_price_valid(&s_table[i], fuel, self, settings))
        {
            values[n++] = s_table[i].price[fuel][self];
        }
    }
    *count = n;
    bool ok = pn_stats(values, n, min, median, max);
    free(values);
    return ok;
}

/* ------------------------------------------------------------- files */

static bool path_of(const char *name, char *out, size_t size)
{
    const char *root = storage_dir();
    if (root == NULL)
    {
        return false;
    }
    char folder[48];
    snprintf(folder, sizeof(folder), "%s/pieno", root);
    mkdir(folder, 0775);
    snprintf(out, size, "%s/%s", folder, name);
    return true;
}

static void save_cache(void)
{
    char path[80], temporary[88];
    if (!path_of("cache.bin", path, sizeof(path)) || !storage_files_acquire())
    {
        return;
    }
    snprintf(temporary, sizeof(temporary), "%s.new", path);
    FILE *file = fopen(temporary, "wb");
    bool ok = file != NULL;
    if (ok)
    {
        pn_lock();
        cache_header_t header = {
            .magic = CACHE_MAGIC, .count = (uint32_t)s_count, .fetched = s_status.fetched,
            .lat = s_cache_lat, .lon = s_cache_lon, .radius_km = s_cache_radius,
        };
        snprintf(header.extraction, sizeof(header.extraction), "%s", s_status.extraction);
        ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
             fwrite(s_table, sizeof(pn_station_t), s_count, file) == (size_t)s_count;
        pn_unlock();
        ok = fclose(file) == 0 && ok;
    }
    if (ok)
    {
        unlink(path);
        ok = rename(temporary, path) == 0;
    }
    if (!ok)
    {
        unlink(temporary);
        ESP_LOGW(TAG, "Cache not saved");
    }
    storage_files_release();
}

static void load_cache(void)
{
    char path[80];
    if (!path_of("cache.bin", path, sizeof(path)) || !storage_files_acquire())
    {
        return;
    }
    FILE *file = fopen(path, "rb");
    cache_header_t header;
    if (file != NULL && fread(&header, sizeof(header), 1, file) == 1 && header.magic == CACHE_MAGIC &&
        header.count <= PN_MAX_STATIONS && fread(s_table, sizeof(pn_station_t), header.count, file) == header.count)
    {
        s_count = (int)header.count;
        s_status.count = s_count;
        s_status.fetched = header.fetched;
        snprintf(s_status.extraction, sizeof(s_status.extraction), "%.11s", header.extraction);
        s_cache_lat = header.lat;
        s_cache_lon = header.lon;
        s_cache_radius = header.radius_km;
        ESP_LOGI(TAG, "Cache: %d stations, data of %s", s_count, s_status.extraction);
    }
    if (file != NULL)
    {
        fclose(file);
    }
    storage_files_release();
}

static void load_history(void)
{
    char path[80];
    if (!path_of("history.bin", path, sizeof(path)) || !storage_files_acquire())
    {
        return;
    }
    FILE *file = fopen(path, "rb");
    if (file != NULL)
    {
        /* Keep the newest records if the file is longer than the table. */
        fseek(file, 0, SEEK_END);
        long records = ftell(file) / (long)sizeof(pn_day_t);
        long skip = records > PN_HISTORY_DAYS * 8 ? records - PN_HISTORY_DAYS * 8 : 0;
        fseek(file, skip * (long)sizeof(pn_day_t), SEEK_SET);
        s_history_count = (int)fread(s_history, sizeof(pn_day_t), PN_HISTORY_DAYS * 8, file);
        fclose(file);
    }
    storage_files_release();
}

static void save_history(void)
{
    char path[80];
    if (!path_of("history.bin", path, sizeof(path)) || !storage_files_acquire())
    {
        return;
    }
    FILE *file = fopen(path, "wb");
    if (file != NULL)
    {
        pn_lock();
        fwrite(s_history, sizeof(pn_day_t), s_history_count, file);
        pn_unlock();
        fclose(file);
    }
    storage_files_release();
}

int pn_history(int fuel, int self, pn_day_t *out, int max)
{
    int n = 0;
    pn_lock();
    for (int i = 0; i < s_history_count && n < max; i++)
    {
        if (s_history[i].fuel == fuel && s_history[i].self == self)
        {
            out[n++] = s_history[i];
        }
    }
    pn_unlock();
    return n;
}

/* One record per day, fuel and mode: today's replaces an earlier one. Locked. */
static void history_put(const pn_day_t *day)
{
    for (int i = s_history_count - 1; i >= 0; i--)
    {
        if (s_history[i].day == day->day && s_history[i].fuel == day->fuel && s_history[i].self == day->self)
        {
            s_history[i] = *day;
            return;
        }
    }
    if (s_history_count == PN_HISTORY_DAYS * 8)
    {
        memmove(s_history, s_history + 8, sizeof(pn_day_t) * (s_history_count - 8));
        s_history_count -= 8;
    }
    s_history[s_history_count++] = *day;
}

int pn_changes(uint32_t id, char *out, int size)
{
    char path[80];
    int used = 0;
    out[0] = '\0';
    if (!path_of("changes.csv", path, sizeof(path)) || !storage_files_acquire())
    {
        return 0;
    }
    FILE *file = fopen(path, "r");
    char line[64], key[16];
    snprintf(key, sizeof(key), ",%lu,", (unsigned long)id);
    while (file != NULL && fgets(line, sizeof(line), file) != NULL)
    {
        /* day,id,fuel,self,price -> day,fuel,self,price */
        char *match = strstr(line, key);
        if (match == NULL || used + 40 >= size)
        {
            continue;
        }
        *match = '\0';
        used += snprintf(out + used, size - used, "%s,%s", line, match + strlen(key));
    }
    if (file != NULL)
    {
        fclose(file);
    }
    storage_files_release();
    return used;
}

/* ------------------------------------------------------------ settings */

static void settings_defaults(pn_settings_t *s)
{
    const settings_t *os = settings_get();
    memset(s, 0, sizeof(*s));
    s->lat = os->weather_lat;
    s->lon = os->weather_lon;
    snprintf(s->place, sizeof(s->place), "%s", os->weather_city);
    s->radius_km = 10;
    s->fuel = PN_BENZINA;
    s->self = PN_SELF;
    s->real_cost = 1;
    s->litres = 40;
    s->km_per_litre = 15;
    s->max_age_days = 7;
}

static void settings_load(void)
{
    nvs_handle_t nvs;
    size_t size = sizeof(s_settings);
    bool ok = false;
    if (nvs_open("pieno", NVS_READONLY, &nvs) == ESP_OK)
    {
        ok = nvs_get_blob(nvs, "settings", &s_settings, &size) == ESP_OK && size == sizeof(s_settings);
        nvs_close(nvs);
    }
    if (!ok || s_settings.radius_km < 3 || s_settings.radius_km > 30 || s_settings.fuel >= PN_FUELS)
    {
        settings_defaults(&s_settings);
    }
}

static void settings_save_job(void *arg)
{
    (void)arg;
    pn_settings_t copy;
    pn_settings(&copy);
    nvs_handle_t nvs;
    if (nvs_open("pieno", NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_blob(nvs, "settings", &copy, sizeof(copy));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void pn_settings(pn_settings_t *out)
{
    pn_lock();
    *out = s_settings;
    pn_unlock();
}

void pn_set_settings(const pn_settings_t *settings)
{
    pn_settings_t s = *settings;
    s.radius_km = s.radius_km < 3 ? 3 : s.radius_km > 30 ? 30 : s.radius_km;
    s.fuel = s.fuel >= PN_FUELS ? 0 : s.fuel;
    s.self = s.self ? PN_SELF : PN_SERVED;
    s.litres = s.litres < 5 ? 5 : s.litres > 150 ? 150 : s.litres;
    s.km_per_litre = s.km_per_litre < 3 ? 3 : s.km_per_litre > 40 ? 40 : s.km_per_litre;
    s.max_age_days = s.max_age_days > 60 ? 60 : s.max_age_days;
    pn_lock();
    bool moved = s.lat != s_settings.lat || s.lon != s_settings.lon || s.radius_km != s_settings.radius_km;
    s_settings = s;
    if (moved)
    {
        /* New home: distances change now, new stations come with a download. */
        for (int i = 0; i < s_count; i++)
        {
            s_table[i].km = (float)pn_distance_km(s.lat, s.lon, s_table[i].lat, s_table[i].lon);
        }
        s_refresh_wanted = true;
    }
    s_status.revision++;
    pn_unlock();
    sys_post(settings_save_job, NULL);
    if (moved && s_task != NULL)
    {
        xTaskNotifyGive(s_task);
    }
}

void pn_refresh(void)
{
    pn_lock();
    s_refresh_wanted = true;
    pn_unlock();
    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
    }
}

/* ---------------------------------------------------------- streaming */

typedef struct
{
    char line[PN_LINE_MAX];
    int used;
    bool overflow;
    int64_t total;
    int64_t done;
    int lines;
} reader_t;

typedef bool (*line_fn_t)(char *line, void *ctx);

/* Download url and hand every complete line to fn. false on any failure. */
static bool stream(const char *url, line_fn_t fn, void *ctx, reader_t *r, int base, int span)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "Pieno/1.0 (watch)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        return false;
    }
    char *chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    bool ok = chunk != NULL && esp_http_client_open(client, 0) == ESP_OK;
    if (ok)
    {
        r->total = esp_http_client_fetch_headers(client);
        ok = esp_http_client_get_status_code(client) == 200;
    }
    r->used = 0;
    r->done = 0;
    r->lines = 0;
    int last_percent = -1;
    while (ok)
    {
        int got = esp_http_client_read(client, chunk, 4096);
        if (got < 0)
        {
            ok = false;
            break;
        }
        if (got == 0)
        {
            ok = esp_http_client_is_complete_data_received(client);
            break;
        }
        r->done += got;
        for (int i = 0; i < got; i++)
        {
            char c = chunk[i];
            if (c == '\n')
            {
                r->line[r->used] = '\0';
                if (!r->overflow && !fn(r->line, ctx))
                {
                    ok = false;
                    break;
                }
                r->lines++;
                r->used = 0;
                r->overflow = false;
            }
            else if (r->used < PN_LINE_MAX - 1)
            {
                r->line[r->used++] = c;
            }
            else
            {
                r->overflow = true;   /* a line this long is not data: skipped */
            }
        }
        if (r->total > 0)
        {
            int percent = base + (int)(r->done * span / r->total);
            if (percent != last_percent)
            {
                last_percent = percent;
                pn_lock();
                s_status.percent = percent;
                pn_unlock();
            }
        }
    }
    if (ok && r->used > 0 && !r->overflow)
    {
        r->line[r->used] = '\0';
        ok = fn(r->line, ctx);
    }
    free(chunk);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

typedef struct
{
    pn_station_t *table;
    int count;
    int16_t *hash;         /* index + 1, 0 = empty */
    uint32_t *keys;
    pn_settings_t home;
    char extraction[12];
    bool first;
} build_t;

static void hash_put(build_t *b, uint32_t id, int index)
{
    uint32_t h = (id * 2654435761u) & (HASH_SIZE - 1);
    while (b->hash[h] != 0)
    {
        h = (h + 1) & (HASH_SIZE - 1);
    }
    b->hash[h] = (int16_t)(index + 1);
    b->keys[h] = id;
}

static int hash_get(const build_t *b, uint32_t id)
{
    uint32_t h = (id * 2654435761u) & (HASH_SIZE - 1);
    while (b->hash[h] != 0)
    {
        if (b->keys[h] == id)
        {
            return b->hash[h] - 1;
        }
        h = (h + 1) & (HASH_SIZE - 1);
    }
    return -1;
}

static void copy_text(char *out, size_t size, const char *in)
{
    /* Trim and collapse the double spaces of the ministry's data. */
    size_t n = 0;
    bool space = true;
    for (; *in != '\0' && n < size - 1; in++)
    {
        if (*in == ' ')
        {
            if (!space)
            {
                out[n++] = ' ';
            }
            space = true;
        }
        else
        {
            out[n++] = *in;
            space = false;
        }
    }
    while (n > 0 && out[n - 1] == ' ')
    {
        n--;
    }
    out[n] = '\0';
}

static bool on_station(char *line, void *ctx)
{
    build_t *b = ctx;
    pn_station_line_t s;
    if (!pn_parse_station(line, &s))
    {
        return true;
    }
    double km = pn_distance_km(b->home.lat, b->home.lon, s.lat, s.lon);
    if (km > b->home.radius_km || b->count == PN_MAX_STATIONS)
    {
        return true;
    }
    pn_station_t *t = &b->table[b->count];
    memset(t, 0, sizeof(*t));
    t->id = s.id;
    copy_text(t->brand, sizeof(t->brand), s.brand);
    copy_text(t->name, sizeof(t->name), s.name);
    copy_text(t->address, sizeof(t->address), s.address);
    copy_text(t->town, sizeof(t->town), s.town);
    copy_text(t->province, sizeof(t->province), s.province);
    t->lat = (float)s.lat;
    t->lon = (float)s.lon;
    t->km = (float)km;
    hash_put(b, s.id, b->count);
    b->count++;
    return true;
}

static bool on_price(char *line, void *ctx)
{
    build_t *b = ctx;
    if (b->first)
    {
        b->first = false;
        if (strncmp(line, "Estrazione del ", 15) == 0)
        {
            snprintf(b->extraction, sizeof(b->extraction), "%.10s", line + 15);
        }
        return true;
    }
    pn_price_line_t p;
    if (!pn_parse_price(line, &p))
    {
        return true;
    }
    int index = hash_get(b, p.id);
    if (index < 0)
    {
        return true;
    }
    pn_station_t *t = &b->table[index];
    int fuel = pn_fuel_of(p.product);
    if (fuel >= 0)
    {
        /* Two lines for the same product: the newest communication wins. */
        if (t->price[fuel][p.self] == 0 || p.when >= t->when[fuel][p.self])
        {
            t->price[fuel][p.self] = p.price;
            t->when[fuel][p.self] = p.when;
        }
    }
    else if (t->extra_count < PN_MAX_EXTRAS)
    {
        pn_extra_t *e = &t->extra[t->extra_count++];
        snprintf(e->product, sizeof(e->product), "%s", p.product);
        e->price = p.price;
        e->self = p.self;
    }
    return true;
}

/* First line of the price file only: is there anything newer than our cache? */
static bool peek_extraction(char *out, size_t size)
{
    esp_http_client_config_t config = {
        .url = PRICES_URL, .timeout_ms = 15000, .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach, .user_agent = "Pieno/1.0 (watch)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    bool ok = client != NULL && esp_http_client_open(client, 0) == ESP_OK &&
              esp_http_client_fetch_headers(client) >= 0 && esp_http_client_get_status_code(client) == 200;
    char head[48] = "";
    if (ok)
    {
        int got = esp_http_client_read(client, head, sizeof(head) - 1);
        ok = got > 25 && strncmp(head, "Estrazione del ", 15) == 0;
        if (ok)
        {
            snprintf(out, size, "%.10s", head + 15);
        }
    }
    if (client != NULL)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return ok;
}

/* Compare with the old table: carry "previous" over, log the changes. Locked. */
static void diff_and_log(build_t *b, int32_t day)
{
    char path[80];
    FILE *log = NULL;
    if (path_of("changes.csv", path, sizeof(path)) && storage_files_acquire())
    {
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > CHANGES_MAX)
        {
            char old[88];
            snprintf(old, sizeof(old), "%s.old", path);
            unlink(old);
            rename(path, old);
        }
        log = fopen(path, "a");
    }
    for (int i = 0; i < b->count; i++)
    {
        pn_station_t *t = &b->table[i];
        const pn_station_t *o = NULL;
        for (int j = 0; j < s_count; j++)
        {
            if (s_table[j].id == t->id)
            {
                o = &s_table[j];
                break;
            }
        }
        for (int f = 0; f < PN_FUELS; f++)
        {
            for (int m = 0; m < 2; m++)
            {
                float now = t->price[f][m], before = o != NULL ? o->price[f][m] : 0;
                if (o != NULL && before > 0 && now > 0 && (now - before > 0.0005f || before - now > 0.0005f))
                {
                    t->previous[f][m] = before;
                }
                else
                {
                    t->previous[f][m] = o != NULL ? o->previous[f][m] : 0;
                }
                if (log != NULL && now > 0 && (o == NULL || before == 0 || now - before > 0.0005f || before - now > 0.0005f))
                {
                    fprintf(log, "%ld,%lu,%d,%d,%.3f\n", (long)day, (unsigned long)t->id, f, m, (double)now);
                }
            }
        }
    }
    if (log != NULL)
    {
        fclose(log);
        storage_files_release();
    }
}

static int32_t extraction_day(const char *text)
{
    /* "2026-10-12" */
    int y = 0, m = 0, d = 0;
    if (sscanf(text, "%d-%d-%d", &y, &m, &d) != 3)
    {
        return (int32_t)(pn_local_now() / 86400);
    }
    return (int32_t)pn_days_from_civil(y, m, d);
}

static bool download(void)
{
    pn_settings_t home;
    pn_settings(&home);
    pn_status_t status;
    pn_status(&status);
    bool moved = home.lat != s_cache_lat || home.lon != s_cache_lon || home.radius_km != s_cache_radius;

    char latest[12] = "";
    status_set(PN_STATIONS, 0, "Controllo i nuovi prezzi...");
    if (!moved && status.count > 0 && peek_extraction(latest, sizeof(latest)) &&
        strcmp(latest, status.extraction) == 0)
    {
        pn_lock();
        s_status.fetched = time(NULL);
        pn_unlock();
        status_set(PN_IDLE, 100, "Prezzi già aggiornati");
        return true;
    }

    build_t *b = heap_caps_calloc(1, sizeof(build_t), MALLOC_CAP_SPIRAM);
    reader_t *r = heap_caps_calloc(1, sizeof(reader_t), MALLOC_CAP_SPIRAM);
    if (b != NULL)
    {
        b->table = heap_caps_calloc(PN_MAX_STATIONS, sizeof(pn_station_t), MALLOC_CAP_SPIRAM);
        b->hash = heap_caps_calloc(HASH_SIZE, sizeof(int16_t), MALLOC_CAP_SPIRAM);
        b->keys = heap_caps_calloc(HASH_SIZE, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    }
    bool ok = b != NULL && r != NULL && b->table != NULL && b->hash != NULL && b->keys != NULL;
    if (ok)
    {
        b->home = home;
        b->first = true;
        status_set(PN_STATIONS, 0, "Scarico l'elenco dei distributori");
        ok = stream(STATIONS_URL, on_station, b, r, 0, 50);
        ESP_LOGI(TAG, "Stations: %s, %d lines, %d within %d km", ok ? "ok" : "failed", r->lines, b->count, home.radius_km);
    }
    if (ok)
    {
        status_set(PN_PRICES, 50, "Scarico i prezzi di oggi");
        ok = stream(PRICES_URL, on_price, b, r, 50, 50);
        ESP_LOGI(TAG, "Prices: %s, %d lines, data of %s", ok ? "ok" : "failed", r->lines, b->extraction);
    }
    if (ok)
    {
        int32_t day = extraction_day(b->extraction);
        pn_lock();
        diff_and_log(b, day);
        memcpy(s_table, b->table, sizeof(pn_station_t) * b->count);
        s_count = b->count;
        s_cache_lat = home.lat;
        s_cache_lon = home.lon;
        s_cache_radius = home.radius_km;
        s_status.count = s_count;
        s_status.fetched = time(NULL);
        snprintf(s_status.extraction, sizeof(s_status.extraction), "%s", b->extraction);
        pn_unlock();
        /* The area's day in the history, for every fuel and mode. */
        for (int f = 0; f < PN_FUELS; f++)
        {
            for (int m = 0; m < 2; m++)
            {
                pn_day_t d = {.day = day, .fuel = (uint8_t)f, .self = (uint8_t)m};
                int count = 0;
                pn_lock();
                bool any = pn_area(f, m, &home, &d.min, &d.median, &d.max, &count);
                d.count = (uint16_t)count;
                if (any)
                {
                    history_put(&d);
                }
                pn_unlock();
            }
        }
        save_cache();
        save_history();
        status_set(PN_IDLE, 100, "Prezzi aggiornati");
    }
    else
    {
        status_set(PN_FAILED, 0, "Download non riuscito: riprovo più tardi");
    }
    if (b != NULL)
    {
        free(b->table);
        free(b->hash);
        free(b->keys);
    }
    free(b);
    free(r);
    return ok;
}

/* The ministry publishes around 8:00: look again after that if our data is older than today. */
static bool stale(void)
{
    pn_status_t s;
    pn_status(&s);
    if (s.count == 0 || s.fetched == 0)
    {
        return true;
    }
    int64_t now = pn_local_now();
    if (now == 0)
    {
        return false;   /* no clock: the cache is the best we have */
    }
    int32_t today = (int32_t)(now / 86400);
    bool morning_done = now % 86400 >= 8 * 3600 + 30 * 60;
    bool old_data = extraction_day(s.extraction) < today;
    return old_data && morning_done && (time(NULL) - s.fetched) * 1000000LL > RETRY_US / 6;
}

static void worker(void *arg)
{
    (void)arg;
    s_task = xTaskGetCurrentTaskHandle();
    int failures = 0;
    for (;;)
    {
        pn_lock();
        bool wanted = s_refresh_wanted;
        s_refresh_wanted = false;
        pn_unlock();
        if (wanted || stale())
        {
            status_set(PN_WAITING_WIFI, 0, "Connessione al Wi-Fi...");
            if (wifi_acquire(25000) == ESP_OK)
            {
                if (time(NULL) < 1700000000)
                {
                    time_sync_now();   /* boards without a clock chip: needed to judge the prices' age */
                }
                wifi_set_fast(true);
                failures = download() ? 0 : failures + 1;
                wifi_set_fast(false);
                wifi_release();
            }
            else
            {
                failures++;
                status_set(PN_FAILED, 0, "Wi-Fi non raggiungibile");
            }
        }
        /* After a failure retry in 1, 2, 4... minutes (max 1 h); otherwise check every 10 min. */
        uint32_t wait_s = failures ? (60u << (failures > 6 ? 6 : failures - 1)) : 600;
        wait_s = wait_s > 3600 ? 3600 : wait_s;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_s * 1000));
    }
}

esp_err_t pn_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_table = heap_caps_calloc(PN_MAX_STATIONS, sizeof(pn_station_t), MALLOC_CAP_SPIRAM);
    s_history = heap_caps_calloc(PN_HISTORY_DAYS * 8, sizeof(pn_day_t), MALLOC_CAP_SPIRAM);
    if (s_lock == NULL || s_table == NULL || s_history == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    settings_load();
    load_cache();
    load_history();
    if (s_count > 0 && (s_cache_lat != s_settings.lat || s_cache_lon != s_settings.lon ||
                        s_cache_radius != s_settings.radius_km))
    {
        s_refresh_wanted = true;   /* the cache was made for another place or radius */
        for (int i = 0; i < s_count; i++)
        {
            s_table[i].km = (float)pn_distance_km(s_settings.lat, s_settings.lon, s_table[i].lat, s_table[i].lon);
        }
    }
    snprintf(s_status.message, sizeof(s_status.message), "%s", s_count ? "Prezzi dalla memoria" : "Primo download...");
    return sys_task_create(worker, "pieno", 8192, NULL, 3, SYS_CORE_ANY) ? ESP_OK : ESP_ERR_NO_MEM;
}
