/*
 * Bilancio's book (see bilancio.h): rules, movements, categories and the
 * balance in PSRAM, saved as one JSON file 1.5 s after the last change (the
 * format of docs/progetti/BILANCIO.md, amounts in euros, readable by hand).
 *
 * The file is written by hand, not through a cJSON tree: 3000 movements
 * would be 20000 nodes. Reading parses it with cJSON (nodes in PSRAM thanks
 * to the appkit hooks). Movements older than 24 months are summed into one
 * line per month (archive), the balance keeping their effect.
 */
#include "bilancio.h"

#include "core/sys.h"
#include "services/storage.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "bilancio";

static const char *const DEFAULT_CATEGORIES[] = {
    "casa", "bollette", "rate", "auto", "spesa", "abbonamenti", "salute", "svago", "tasse", "altro",
};

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_save_timer;
static bl_rule_t *s_rules;
static int s_rule_count;
static bl_tx_t *s_tx;
static int s_tx_count;
static bl_archive_t *s_archive;
static int s_archive_count;
static char s_categories[BL_MAX_CATEGORIES][BL_CATEGORY_LEN];
static int s_category_count;
static bl_book_t s_book;
static uint32_t s_next_id = 1;
static uint32_t s_revision;
static bool s_dirty;

/* ---------------------------------------------------------------- base */

void bl_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void bl_unlock(void) { xSemaphoreGive(s_lock); }

uint32_t bl_revision(void)
{
    return __atomic_load_n(&s_revision, __ATOMIC_RELAXED);
}

bl_day_t bl_today(void)
{
    time_t now = time(NULL);
    if (now < 1700000000)
    {
        return BL_NONE;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    return bl_days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/* Locked: the book's view of the arrays. */
static void refresh_book(void)
{
    s_book.rules = s_rules;
    s_book.rule_count = s_rule_count;
    s_book.tx = s_tx;
    s_book.tx_count = s_tx_count;
}

const bl_book_t *bl_book(void) { return &s_book; }

const char (*bl_categories(int *count))[BL_CATEGORY_LEN]
{
    *count = s_category_count;
    return (const char (*)[BL_CATEGORY_LEN])s_categories;
}

const bl_archive_t *bl_archive(int *count)
{
    *count = s_archive_count;
    return s_archive;
}

const bl_rule_t *bl_rule_by_id(uint32_t id)
{
    for (int i = 0; i < s_rule_count; i++)
    {
        if (s_rules[i].id == id)
        {
            return &s_rules[i];
        }
    }
    return NULL;
}

void bl_copy(char *out, size_t size, const char *in)
{
    size_t n = 0;
    const unsigned char *c = (const unsigned char *)(in != NULL ? in : "");
    while (*c != '\0')
    {
        size_t length = *c < 0x80 ? 1 : (*c & 0xE0) == 0xC0 ? 2 : (*c & 0xF0) == 0xE0 ? 3 : (*c & 0xF8) == 0xF0 ? 4 : 0;
        bool valid = length > 0 && !(length == 2 && *c < 0xC2) && (*c >= 0x20 || length > 1);
        for (size_t i = 1; valid && i < length; i++)
        {
            valid = (c[i] & 0xC0) == 0x80;
        }
        if (!valid)
        {
            c++;
            continue;
        }
        if (n + length >= size)
        {
            break;
        }
        memcpy(out + n, c, length);
        n += length;
        c += length;
    }
    out[n] = '\0';
}

static void save_job(void *arg);

static void save_later(void *arg)
{
    (void)arg;
    sys_post(save_job, NULL);
}

/* Locked. */
static void changed(void)
{
    refresh_book();
    __atomic_add_fetch(&s_revision, 1, __ATOMIC_RELAXED);
    s_dirty = true;
    if (s_save_timer != NULL)
    {
        esp_timer_stop(s_save_timer);
        esp_timer_start_once(s_save_timer, 1500 * 1000);
    }
}

/* ------------------------------------------------------------ archive */

static void archive_add(int month, bl_cents_t amount)
{
    int i = 0;
    while (i < s_archive_count && s_archive[i].month < month)
    {
        i++;
    }
    if (i == s_archive_count || s_archive[i].month != month)
    {
        if (s_archive_count == BL_MAX_ARCHIVE)
        {
            if (i == 0)
            {
                return;                 /* older than everything kept */
            }
            memmove(&s_archive[0], &s_archive[1], sizeof(bl_archive_t) * (size_t)(i - 1));
            i--;
        }
        else
        {
            memmove(&s_archive[i + 1], &s_archive[i], sizeof(bl_archive_t) * (size_t)(s_archive_count - i));
            s_archive_count++;
        }
        s_archive[i] = (bl_archive_t){.month = month};
    }
    if (amount >= 0)
    {
        s_archive[i].in += amount;
    }
    else
    {
        s_archive[i].out -= amount;
    }
}

/* Locked: movements before the last BL_KEEP_MONTHS go into the archive. */
static bool prune(bl_day_t today)
{
    if (today == BL_NONE)
    {
        return false;
    }
    bl_day_t cutoff = bl_month_start(bl_month_of(today) - (BL_KEEP_MONTHS - 1));
    int kept = 0;
    bool any = false;
    for (int i = 0; i < s_tx_count; i++)
    {
        bl_tx_t *t = &s_tx[i];
        if (t->date >= cutoff)
        {
            s_tx[kept++] = *t;
            continue;
        }
        any = true;
        archive_add(bl_month_of(t->date), t->amount);
        /* Still part of the balance: the balance takes it in. */
        if (s_book.balance_date != BL_NONE && t->id > s_book.balance_after && t->date >= s_book.balance_date)
        {
            s_book.balance += t->amount;
        }
    }
    s_tx_count = kept;
    return any;
}

/* ------------------------------------------------------------- writing */

typedef struct
{
    char *data;
    size_t length;
    size_t size;
    bool failed;
} out_t;

static void emit(out_t *o, const char *format, ...)
{
    if (o->failed)
    {
        return;
    }
    for (;;)
    {
        va_list args;
        va_start(args, format);
        int n = vsnprintf(o->data + o->length, o->size - o->length, format, args);
        va_end(args);
        if (n < 0)
        {
            o->failed = true;
            return;
        }
        if ((size_t)n < o->size - o->length)
        {
            o->length += (size_t)n;
            return;
        }
        size_t size = o->size * 2 + (size_t)n;
        char *bigger = heap_caps_realloc(o->data, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (bigger == NULL)
        {
            o->failed = true;
            return;
        }
        o->data = bigger;
        o->size = size;
    }
}

static void emit_string(out_t *o, const char *text)
{
    emit(o, "\"");
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; c++)
    {
        if (*c == '"' || *c == '\\')
        {
            emit(o, "\\%c", *c);
        }
        else if (*c < 0x20)
        {
            emit(o, "\\u%04x", *c);
        }
        else
        {
            emit(o, "%c", *c);
        }
    }
    emit(o, "\"");
}

static void emit_money(out_t *o, bl_cents_t cents)
{
    unsigned long long a = (unsigned long long)(cents < 0 ? -cents : cents);
    emit(o, "%s%llu.%02llu", cents < 0 ? "-" : "", a / 100, a % 100);
}

/* "month"... "year"; past a year the number of months ("every": 24). */
static void emit_every(out_t *o, int every)
{
    switch (every)
    {
    case 2: emit(o, "\"bimonth\""); break;
    case 3: emit(o, "\"quarter\""); break;
    case 4: emit(o, "\"four\""); break;
    case 6: emit(o, "\"semester\""); break;
    case 12: emit(o, "\"year\""); break;
    case 0:
    case 1: emit(o, "\"month\""); break;
    default: emit(o, "%d", every); break;
    }
}

bool bl_every_valid(int every)
{
    return every == 1 || every == 2 || every == 3 || every == 4 || every == 6 || every == 12 || every == 24 ||
           every == 36 || every == 48 || every == 60 || every == 120;
}

static const char *shift_name(int shift)
{
    return shift == BL_SHIFT_BEFORE ? "before" : shift == BL_SHIFT_AFTER ? "after" : "exact";
}

static const char *category_name(int index)
{
    return index < s_category_count ? s_categories[index] : s_categories[s_category_count - 1];
}

static void emit_rule(out_t *o, const bl_rule_t *r)
{
    char month[8];
    emit(o, "    {\"id\": %lu, \"name\": ", (unsigned long)r->id);
    emit_string(o, r->name);
    emit(o, ", \"amount\": ");
    emit_money(o, r->amount);
    emit(o, ", \"every\": ");
    emit_every(o, r->every);
    if (r->every > 1 && r->every <= 12)
    {
        emit(o, ", \"month\": %d", r->month);
    }
    emit(o, ", \"day\": %d, \"weekend\": \"%s\"", r->day, shift_name(r->shift));
    if (r->kind == BL_EXPENSE)
    {
        emit(o, ", \"category\": ");
        emit_string(o, category_name(r->category));
    }
    if (r->first != 0)
    {
        bl_format_month(r->first, month);
        emit(o, ", \"start\": \"%s\"", month);
    }
    if (r->last != 0)
    {
        bl_format_month(r->last, month);
        emit(o, ", \"end\": \"%s\"", month);
    }
    emit(o, "}");
}

static void emit_rules(out_t *o, int kind)
{
    bool first = true;
    for (int i = 0; i < s_rule_count; i++)
    {
        if (s_rules[i].kind == kind)
        {
            emit(o, first ? "\n" : ",\n");
            emit_rule(o, &s_rules[i]);
            first = false;
        }
    }
    emit(o, first ? "]" : "\n  ]");
}

/* Locked. */
static char *to_text(void)
{
    out_t o = {.size = 16384 + (size_t)s_tx_count * 112};
    o.data = heap_caps_malloc(o.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (o.data == NULL)
    {
        return NULL;
    }
    char date[12];
    emit(&o, "{\n  \"version\": 1,\n  \"currency\": \"EUR\",\n  \"next_id\": %lu,\n", (unsigned long)s_next_id);
    if (s_book.balance_date != BL_NONE)
    {
        bl_format_date(s_book.balance_date, date);
        emit(&o, "  \"balance\": {\"amount\": ");
        emit_money(&o, s_book.balance);
        emit(&o, ", \"date\": \"%s\", \"after\": %lu},\n", date, (unsigned long)s_book.balance_after);
    }
    emit(&o, "  \"income\": [");
    emit_rules(&o, BL_INCOME);
    emit(&o, ",\n  \"recurring\": [");
    emit_rules(&o, BL_EXPENSE);
    emit(&o, ",\n  \"transactions\": [");
    for (int i = 0; i < s_tx_count; i++)
    {
        const bl_tx_t *t = &s_tx[i];
        bl_format_date(t->date, date);
        emit(&o, "%s    {\"id\": %lu, \"date\": \"%s\", \"amount\": ", i ? ",\n" : "\n", (unsigned long)t->id, date);
        emit_money(&o, t->amount);
        emit(&o, ", \"category\": ");
        emit_string(&o, category_name(t->category));
        if (t->note[0] != '\0')
        {
            emit(&o, ", \"note\": ");
            emit_string(&o, t->note);
        }
        if (t->rule != 0)
        {
            emit(&o, ", \"rule\": %lu", (unsigned long)t->rule);
        }
        emit(&o, "}");
    }
    emit(&o, s_tx_count ? "\n  ],\n  \"categories\": [" : "],\n  \"categories\": [");
    for (int i = 0; i < s_category_count; i++)
    {
        emit(&o, i ? ", " : "");
        emit_string(&o, s_categories[i]);
    }
    emit(&o, "],\n  \"archive\": [");
    for (int i = 0; i < s_archive_count; i++)
    {
        bl_format_month(s_archive[i].month, date);
        emit(&o, "%s{\"month\": \"%s\", \"in\": ", i ? ", " : "", date);
        emit_money(&o, s_archive[i].in);
        emit(&o, ", \"out\": ");
        emit_money(&o, s_archive[i].out);
        emit(&o, "}");
    }
    emit(&o, "]\n}\n");
    if (o.failed)
    {
        free(o.data);
        return NULL;
    }
    return o.data;
}

char *bl_export(void)
{
    bl_lock();
    char *text = to_text();
    bl_unlock();
    return text;
}

static void save_job(void *arg)
{
    (void)arg;
    bl_lock();
    char *text = s_dirty ? to_text() : NULL;
    if (text != NULL)
    {
        s_dirty = false;
    }
    bl_unlock();
    if (text == NULL)
    {
        return;
    }
    char path[96], temporary[104];
    bool ok = false;
    if (storage_config_path("bilancio.json", path, sizeof(path)) && storage_files_acquire())
    {
        snprintf(temporary, sizeof(temporary), "%s.new", path);
        size_t length = strlen(text);
        FILE *file = fopen(temporary, "wb");
        ok = file != NULL && fwrite(text, 1, length, file) == length;
        if (file != NULL)
        {
            ok = fclose(file) == 0 && ok;
        }
        if (ok)
        {
            unlink(path);
            ok = rename(temporary, path) == 0;
        }
        storage_files_release();
    }
    free(text);
    if (!ok)
    {
        ESP_LOGW(TAG, "Not saved, retrying");
        bl_lock();
        s_dirty = true;
        if (s_save_timer != NULL)
        {
            esp_timer_start_once(s_save_timer, 10 * 1000 * 1000);
        }
        bl_unlock();
    }
}

void bl_flush(void)
{
    if (s_lock == NULL)
    {
        return;
    }
    if (s_save_timer != NULL)
    {
        esp_timer_stop(s_save_timer);
    }
    save_job(NULL);
}

/* ------------------------------------------------------------- reading */

static double num(const cJSON *o, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static const char *text_of(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int category_find(const char *name)
{
    for (int i = 0; i < s_category_count; i++)
    {
        if (strcasecmp(s_categories[i], name) == 0)
        {
            return i;
        }
    }
    return -1;
}

/* Locked. */
static int category_add(const char *name)
{
    char clean[BL_CATEGORY_LEN];
    bl_copy(clean, sizeof(clean), name);
    if (clean[0] == '\0')
    {
        return -1;
    }
    int index = category_find(clean);
    if (index < 0 && s_category_count < BL_MAX_CATEGORIES)
    {
        index = s_category_count++;
        memcpy(s_categories[index], clean, sizeof(clean));
    }
    return index;
}

/* Locked: a category by name or number, "altro" (the last) if unknown and no room. */
static uint8_t category_of(const cJSON *o)
{
    const char *name = text_of(o, "category");
    int index = name != NULL ? category_add(name) : (int)num(o, "category", -1);
    return (uint8_t)(index >= 0 && index < s_category_count ? index : s_category_count - 1);
}

static int every_of(const cJSON *o)
{
    const char *name = text_of(o, "every");
    if (name == NULL)
    {
        int n = (int)num(o, "every", 1);
        return bl_every_valid(n) ? n : 1;
    }
    return strcmp(name, "bimonth") == 0 ? 2 : strcmp(name, "quarter") == 0 ? 3 : strcmp(name, "four") == 0 ? 4 :
           strcmp(name, "semester") == 0 ? 6 : strcmp(name, "year") == 0 ? 12 : 1;
}

static int shift_of(const cJSON *o, int kind)
{
    const char *name = text_of(o, "weekend");
    if (name == NULL)
    {
        return kind == BL_INCOME ? BL_SHIFT_BEFORE : BL_SHIFT_EXACT;
    }
    return strcmp(name, "before") == 0 ? BL_SHIFT_BEFORE : strcmp(name, "after") == 0 ? BL_SHIFT_AFTER : BL_SHIFT_EXACT;
}

static void take_id(uint32_t id)
{
    if (id >= s_next_id)
    {
        s_next_id = id + 1;
    }
}

static void read_rules(const cJSON *list, int kind)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, list)
    {
        if (s_rule_count == BL_MAX_RULES)
        {
            return;
        }
        bl_rule_t r = {0};
        r.id = (uint32_t)num(item, "id", 0);
        r.kind = (uint8_t)kind;
        bl_copy(r.name, sizeof(r.name), text_of(item, "name"));
        r.amount = bl_cents(num(item, "amount", 0));
        r.amount = r.amount < 0 ? -r.amount : r.amount;
        r.every = (uint8_t)every_of(item);
        r.month = (uint8_t)num(item, "month", 1);
        r.day = (uint8_t)num(item, "day", 1);
        r.shift = (uint8_t)shift_of(item, kind);
        r.category = kind == BL_EXPENSE ? category_of(item) : 0;
        r.first = bl_parse_month(text_of(item, "start"));
        r.last = bl_parse_month(text_of(item, "end"));
        if (r.id == 0 || r.day < 1 || r.day > 31 || r.month < 1 || r.month > 12)
        {
            continue;
        }
        take_id(r.id);
        s_rules[s_rule_count++] = r;
    }
}

/* Locked: fill the (empty) book from a parsed file. */
static void from_json(const cJSON *root)
{
    s_next_id = (uint32_t)num(root, "next_id", 1);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "categories"), *item;
    cJSON_ArrayForEach(item, list)
    {
        if (cJSON_IsString(item))
        {
            category_add(item->valuestring);
        }
    }
    if (s_category_count == 0)
    {
        for (size_t i = 0; i < sizeof(DEFAULT_CATEGORIES) / sizeof(DEFAULT_CATEGORIES[0]); i++)
        {
            category_add(DEFAULT_CATEGORIES[i]);
        }
    }
    const cJSON *balance = cJSON_GetObjectItemCaseSensitive(root, "balance");
    bl_day_t date = bl_parse_date(text_of(balance, "date"));
    if (date != BL_NONE)
    {
        s_book.balance = bl_cents(num(balance, "amount", 0));
        s_book.balance_date = date;
        s_book.balance_after = (uint32_t)num(balance, "after", 0);
    }
    read_rules(cJSON_GetObjectItemCaseSensitive(root, "income"), BL_INCOME);
    read_rules(cJSON_GetObjectItemCaseSensitive(root, "recurring"), BL_EXPENSE);
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "transactions"))
    {
        if (s_tx_count == BL_MAX_TX)
        {
            break;
        }
        bl_tx_t t = {0};
        t.id = (uint32_t)num(item, "id", 0);
        t.date = bl_parse_date(text_of(item, "date"));
        t.amount = bl_cents(num(item, "amount", 0));
        t.category = category_of(item);
        t.rule = (uint32_t)num(item, "rule", 0);
        bl_copy(t.note, sizeof(t.note), text_of(item, "note"));
        if (t.id == 0 || t.date == BL_NONE || t.amount == 0)
        {
            continue;
        }
        take_id(t.id);
        s_tx[s_tx_count++] = t;
    }
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "archive"))
    {
        int month = bl_parse_month(text_of(item, "month"));
        if (month != 0)
        {
            archive_add(month, bl_cents(num(item, "in", 0)));
            archive_add(month, -bl_cents(num(item, "out", 0)));
        }
    }
}

/* Locked. */
static void clear(void)
{
    s_rule_count = 0;
    s_tx_count = 0;
    s_archive_count = 0;
    s_category_count = 0;
    s_next_id = 1;
    s_book = (bl_book_t){.balance_date = BL_NONE};
}

/* The same hooks appkit installs (cJSON nodes in PSRAM): the external app reads the book before
 * the dashboard starts, and 3000 movements are ~20000 nodes. */
static void *json_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void json_in_psram(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
}

static bool valid_file(const cJSON *root)
{
    return cJSON_IsObject(root) && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "version")) &&
           (cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "transactions")) ||
            cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "recurring")) ||
            cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "income")));
}

static void load(void)
{
    char path[96];
    if (!storage_config_path("bilancio.json", path, sizeof(path)) || !storage_files_acquire())
    {
        return;
    }
    char *text = NULL;
    FILE *file = fopen(path, "rb");
    if (file != NULL)
    {
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        if (size > 0 && size < 1024 * 1024)
        {
            text = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (text != NULL && fread(text, 1, (size_t)size, file) == (size_t)size)
            {
                text[size] = '\0';
            }
            else
            {
                free(text);
                text = NULL;
            }
        }
        fclose(file);
    }
    storage_files_release();
    cJSON *root = text != NULL ? cJSON_Parse(text) : NULL;
    free(text);
    if (valid_file(root))
    {
        from_json(root);
        ESP_LOGI(TAG, "%d rules, %d movements, %d archived months", s_rule_count, s_tx_count, s_archive_count);
    }
    else if (text != NULL)
    {
        ESP_LOGW(TAG, "bilancio.json not readable: starting empty");
    }
    cJSON_Delete(root);
}

bool bl_import(const char *text, char *why, size_t size)
{
    cJSON *root = cJSON_Parse(text);
    if (!valid_file(root))
    {
        cJSON_Delete(root);
        snprintf(why, size, "Non è un file di Bilancio");
        return false;
    }
    bl_lock();
    clear();
    from_json(root);
    prune(bl_today());
    changed();
    bl_unlock();
    cJSON_Delete(root);
    return true;
}

/* ---------------------------------------------------------------- edits */

bool bl_set_balance(bl_cents_t amount, bl_day_t date)
{
    bl_lock();
    s_book.balance = date == BL_NONE ? 0 : amount;   /* BL_NONE: back to "from income and expenses" */
    s_book.balance_date = date;
    s_book.balance_after = date == BL_NONE ? 0 : s_next_id - 1;   /* everything recorded so far is in it */
    changed();
    bl_unlock();
    return true;
}

uint32_t bl_tx_put(const bl_tx_t *tx)
{
    if (tx->date == BL_NONE || tx->amount == 0)
    {
        return 0;
    }
    bl_lock();
    bl_tx_t *slot = NULL;
    for (int i = 0; i < s_tx_count && tx->id != 0; i++)
    {
        if (s_tx[i].id == tx->id)
        {
            slot = &s_tx[i];
        }
    }
    uint32_t id = 0;
    if (slot != NULL)
    {
        *slot = *tx;
        id = tx->id;
    }
    else if (tx->id == 0)
    {
        if (s_tx_count == BL_MAX_TX)
        {
            prune(bl_today());
        }
        if (s_tx_count < BL_MAX_TX)
        {
            slot = &s_tx[s_tx_count++];
            *slot = *tx;
            slot->id = id = s_next_id++;
        }
    }
    if (slot != NULL)
    {
        slot->category = slot->category < s_category_count ? slot->category : (uint8_t)(s_category_count - 1);
        changed();
    }
    bl_unlock();
    return id;
}

bool bl_tx_delete(uint32_t id)
{
    bl_lock();
    bool found = false;
    for (int i = 0; i < s_tx_count; i++)
    {
        if (s_tx[i].id == id)
        {
            memmove(&s_tx[i], &s_tx[i + 1], sizeof(bl_tx_t) * (size_t)(s_tx_count - i - 1));
            s_tx_count--;
            found = true;
            break;
        }
    }
    if (found)
    {
        changed();
    }
    bl_unlock();
    return found;
}

uint32_t bl_rule_put(const bl_rule_t *rule)
{
    bl_lock();
    bl_rule_t *slot = NULL;
    for (int i = 0; i < s_rule_count && rule->id != 0; i++)
    {
        if (s_rules[i].id == rule->id)
        {
            slot = &s_rules[i];
        }
    }
    uint32_t id = 0;
    if (slot != NULL)
    {
        *slot = *rule;
        id = rule->id;
    }
    else if (rule->id == 0 && s_rule_count < BL_MAX_RULES)
    {
        slot = &s_rules[s_rule_count++];
        *slot = *rule;
        slot->id = id = s_next_id++;
    }
    if (slot != NULL)
    {
        changed();
    }
    bl_unlock();
    return id;
}

bool bl_rule_delete(uint32_t id)
{
    bl_lock();
    bool found = false;
    for (int i = 0; i < s_rule_count; i++)
    {
        if (s_rules[i].id == id)
        {
            memmove(&s_rules[i], &s_rules[i + 1], sizeof(bl_rule_t) * (size_t)(s_rule_count - i - 1));
            s_rule_count--;
            found = true;
            break;
        }
    }
    /* Movements that paid it stay linked (they are not variable spending). */
    if (found)
    {
        changed();
    }
    bl_unlock();
    return found;
}

int bl_category_add(const char *name)
{
    bl_lock();
    int before = s_category_count;
    int index = category_add(name);
    if (s_category_count != before)
    {
        changed();
    }
    bl_unlock();
    return index;
}

bool bl_category_rename(int index, const char *name)
{
    char clean[BL_CATEGORY_LEN];
    bl_copy(clean, sizeof(clean), name);
    bl_lock();
    bool ok = index >= 0 && index < s_category_count && clean[0] != '\0' &&
              (category_find(clean) < 0 || category_find(clean) == index);
    if (ok)
    {
        memcpy(s_categories[index], clean, sizeof(clean));
        changed();
    }
    bl_unlock();
    return ok;
}

/* ---------------------------------------------------------------- start */

esp_err_t bl_start(void)
{
    if (s_lock != NULL)
    {
        return ESP_OK;
    }
    s_rules = heap_caps_calloc(BL_MAX_RULES, sizeof(bl_rule_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tx = heap_caps_calloc(BL_MAX_TX, sizeof(bl_tx_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_archive = heap_caps_calloc(BL_MAX_ARCHIVE, sizeof(bl_archive_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t timer = {.callback = save_later, .name = "bl_save"};
    if (s_rules == NULL || s_tx == NULL || s_archive == NULL || lock == NULL ||
        esp_timer_create(&timer, &s_save_timer) != ESP_OK)
    {
        free(s_rules);
        free(s_tx);
        free(s_archive);
        if (lock != NULL)
        {
            vSemaphoreDelete(lock);
        }
        s_rules = NULL;
        s_tx = NULL;
        s_archive = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_lock = lock;
    json_in_psram();
    bl_lock();
    clear();
    load();
    if (s_category_count == 0)
    {
        for (size_t i = 0; i < sizeof(DEFAULT_CATEGORIES) / sizeof(DEFAULT_CATEGORIES[0]); i++)
        {
            category_add(DEFAULT_CATEGORIES[i]);
        }
    }
    bool pruned = prune(bl_today());
    refresh_book();
    if (pruned)
    {
        changed();
    }
    bl_unlock();
    return ESP_OK;
}
