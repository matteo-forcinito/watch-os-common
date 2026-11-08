#include "services/orbit_state.h"

#include "orbit_model.h"
#include "services/ecos.h"
#include "ecos_internal.h"
#include "core/sys.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string.h>

#define ORBIT_NVS_NS "ecos_orbit"
#define ORBIT_NVS_CLOCK "clock_res"
#define ORBIT_RESERVE_BLOCK UINT64_C(256)
#define ORBIT_RPC_PREFIX_SIZE 5
#define ORBIT_SYNC_INTERVAL_US (30LL * 1000000)
#define ORBIT_SYNC_TIMEOUT_MS 1800

enum { ORBIT_CMD_MERGE = 1, ORBIT_CMD_GET = 2, ORBIT_CMD_SUMMARY = 3 };
static const uint8_t s_magic[4] = {'O', 'S', '0', '1'};

static orbit_model_store_t s_store;
static orbit_model_entry_t *s_entries;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_storage;
static bool s_ready;
static volatile bool s_sync_pending;
static portMUX_TYPE s_pending_mux = portMUX_INITIALIZER_UNLOCKED;
static size_t s_peer_cursor;
static int64_t s_last_sync_us;
static uint64_t s_reserved_ceiling;

/* Implemented by the ECOS link owner: increments the hold only if its radio
 * is already active, so gossip never starts/awakens the radio. */
extern bool ecos_family_hold_active(void);

static bool take_lock(TickType_t wait)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, wait) == pdTRUE;
}

static bool sync_pending_get(void)
{
    portENTER_CRITICAL(&s_pending_mux);
    bool pending = s_sync_pending;
    portEXIT_CRITICAL(&s_pending_mux);
    return pending;
}

static void sync_pending_set(bool pending)
{
    portENTER_CRITICAL(&s_pending_mux);
    s_sync_pending = pending;
    portEXIT_CRITICAL(&s_pending_mux);
}

static void give_lock(void)
{
    xSemaphoreGive(s_lock);
}

static bool persist_ceiling_locked(uint64_t ceiling)
{
    nvs_handle_t nvs;
    if (nvs_open(ORBIT_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_u64(nvs, ORBIT_NVS_CLOCK, ceiling);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

/* Reserve before a version can be exposed. The durable ceiling is deliberately
 * separate from the current Lamport clock to avoid spending a flash write per
 * local operation. */
static bool reserve_clock_locked(uint64_t clock)
{
    if (clock <= s_reserved_ceiling) return true;
    uint64_t ceiling = UINT64_MAX - clock < ORBIT_RESERVE_BLOCK - 1
                           ? UINT64_MAX
                           : clock + ORBIT_RESERVE_BLOCK - 1;
    if (!persist_ceiling_locked(ceiling)) return false;
    s_reserved_ceiling = ceiling;
    return true;
}

static bool ensure_clock_reserved_locked(uint64_t clock)
{
    return reserve_clock_locked(clock);
}

static bool read_reserved_clock(uint64_t *ceiling)
{
    nvs_handle_t nvs;
    if (nvs_open(ORBIT_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_get_u64(nvs, ORBIT_NVS_CLOCK, ceiling);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        *ceiling = 0;
        err = ESP_OK;
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

bool orbit_state_init(void)
{
    if (s_ready) return true;
    SemaphoreHandle_t lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (lock == NULL) return false;
    orbit_model_entry_t *entries = heap_caps_calloc(ORBIT_MODEL_CAPACITY, sizeof(*entries),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (entries == NULL)
    {
        vSemaphoreDelete(lock);
        return false;
    }
    uint64_t ceiling = 0;
    if (!read_reserved_clock(&ceiling))
    {
        heap_caps_free(entries);
        vSemaphoreDelete(lock);
        return false;
    }
    if (!orbit_model_store_init(&s_store, entries, ORBIT_MODEL_CAPACITY))
    {
        heap_caps_free(entries);
        vSemaphoreDelete(lock);
        return false;
    }
    s_store.observed_clock = ceiling;
    s_entries = entries;
    s_reserved_ceiling = ceiling;
    s_lock = lock;
    sync_pending_set(false);
    s_peer_cursor = 0;
    s_last_sync_us = -ORBIT_SYNC_INTERVAL_US;
    s_ready = true;
    return true;
}

static bool valid_local_key(const uint8_t *key, size_t key_len)
{
    return key != NULL && key_len > 0 && key_len <= ORBIT_STATE_KEY_MAX;
}

static bool local_write(const uint8_t *key, size_t key_len, const uint8_t *value,
                        size_t value_len, bool tombstone)
{
    if (!s_ready || !valid_local_key(key, key_len) || value_len > ORBIT_STATE_VALUE_MAX ||
        (value_len != 0 && value == NULL) || (tombstone && value_len != 0) ||
        !take_lock(portMAX_DELAY)) return false;
    uint64_t next = 0;
    bool ok = orbit_model_next_clock(&s_store, 0, &next) &&
              ensure_clock_reserved_locked(next);
    if (ok)
    {
        uint8_t author[6] = {0};
        if (esp_read_mac(author, ESP_MAC_WIFI_STA) != ESP_OK || !orbit_model_author_valid(author))
            ok = false;
        else
            ok = orbit_model_local_update(&s_store, key, key_len, value, value_len, tombstone,
                                          next, author);
    }
    give_lock();
    return ok;
}

bool orbit_state_put(const uint8_t *key, size_t key_len,
                     const uint8_t *value, size_t value_len)
{
    return local_write(key, key_len, value, value_len, false);
}

bool orbit_state_delete(const uint8_t *key, size_t key_len)
{
    return local_write(key, key_len, NULL, 0, true);
}

bool orbit_state_get(const uint8_t *key, size_t key_len,
                     uint8_t *value, size_t value_capacity, size_t *value_len,
                     uint64_t *clock, uint8_t author[6])
{
    if (!s_ready || !valid_local_key(key, key_len) || value_len == NULL ||
        !take_lock(0)) return false;
    const orbit_model_entry_t *entry = orbit_model_get(&s_store, key, key_len);
    bool ok = entry != NULL && !entry->tombstone && entry->value_len <= value_capacity &&
              (entry->value_len == 0 || value != NULL);
    if (ok)
    {
        if (entry->value_len != 0) memcpy(value, entry->value, entry->value_len);
        *value_len = entry->value_len;
        if (clock != NULL) *clock = entry->clock;
        if (author != NULL) memcpy(author, entry->author, sizeof(entry->author));
    }
    give_lock();
    return ok;
}

static bool prefix_valid(const uint8_t *request, size_t length)
{
    return request != NULL && length >= ORBIT_RPC_PREFIX_SIZE &&
           memcmp(request, s_magic, sizeof(s_magic)) == 0;
}

static void put_u64(uint8_t *out, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) out[i] = (uint8_t)(value >> (56 - 8 * i));
}

static bool merge_entry(const orbit_model_entry_t *entry)
{
    if (!s_ready || !orbit_model_entry_valid(entry) || !take_lock(portMAX_DELAY)) return false;
    bool ok = ensure_clock_reserved_locked(entry->clock) && orbit_model_merge(&s_store, entry);
    give_lock();
    return ok;
}

static const orbit_model_entry_t *entry_by_index_locked(uint8_t index)
{
    size_t seen = 0;
    for (size_t i = 0; i < s_store.capacity; ++i)
        if (s_entries[i].used && seen++ == index) return &s_entries[i];
    return NULL;
}

static size_t used_count_locked(void)
{
    return s_store.count;
}

size_t orbit_state_handle_rpc(const char *from_id, const uint8_t *request, size_t length,
                              uint8_t *reply, size_t max)
{
    (void)from_id;
    if (!s_ready || !prefix_valid(request, length) || request[4] < ORBIT_CMD_MERGE ||
        request[4] > ORBIT_CMD_SUMMARY || reply == NULL) return 0;
    uint8_t command = request[4];
    if (command == ORBIT_CMD_MERGE)
    {
        if (length <= ORBIT_RPC_PREFIX_SIZE || max < 6) return 0;
        orbit_model_entry_t entry;
        if (!orbit_model_decode(request + ORBIT_RPC_PREFIX_SIZE,
                                length - ORBIT_RPC_PREFIX_SIZE, &entry)) return 0;
        bool merged = merge_entry(&entry);
        memcpy(reply, s_magic, sizeof(s_magic));
        reply[4] = command;
        reply[5] = merged ? 1 : 0;
        return 6;
    }
    if (length != ORBIT_RPC_PREFIX_SIZE + 1 || max < 8 || !take_lock(portMAX_DELAY)) return 0;
    uint8_t index = request[5];
    uint8_t count = (uint8_t)used_count_locked();
    const orbit_model_entry_t *entry = entry_by_index_locked(index);
    memcpy(reply, s_magic, sizeof(s_magic));
    reply[4] = command;
    reply[5] = count;
    reply[6] = index;
    if (command == ORBIT_CMD_GET)
    {
        reply[7] = entry != NULL ? 1 : 0; /* explicit end marker */
        size_t result = 8;
        if (entry != NULL)
        {
            size_t encoded = orbit_model_encode(entry, reply + result, max - result);
            if (encoded == 0)
            {
                give_lock();
                return 0;
            }
            result += encoded;
        }
        give_lock();
        return result;
    }
    /* Summary contains the full key and version, so diffing has no hash
     * collision path. flags bit 0 means present; bit 1 means tombstone. */
    reply[7] = entry != NULL ? (uint8_t)(1 | (entry->tombstone ? 2 : 0)) : 0;
    size_t result = 8;
    if (entry != NULL)
    {
        size_t need = 1 + entry->key_len + 8 + 6;
        if (max - result < need)
        {
            give_lock();
            return 0;
        }
        reply[result++] = entry->key_len;
        memcpy(reply + result, entry->key, entry->key_len);
        result += entry->key_len;
        put_u64(reply + result, entry->clock);
        result += 8;
        memcpy(reply + result, entry->author, 6);
        result += 6;
    }
    give_lock();
    return result;
}

static bool parse_summary(const uint8_t *reply, size_t length, uint8_t *count, uint8_t *index,
                          orbit_model_entry_t *version)
{
    if (length < 8 || memcmp(reply, s_magic, 4) != 0 || reply[4] != ORBIT_CMD_SUMMARY) return false;
    *count = reply[5];
    *index = reply[6];
    if (*count > ORBIT_MODEL_CAPACITY || *index >= ORBIT_MODEL_CAPACITY) return false;
    if (reply[7] == 0) return length == 8 && *index >= *count;
    if (*index >= *count || (reply[7] & ~3) != 0 || length < 8 + 1 + 1 + 8 + 6) return false;
    size_t pos = 8;
    uint8_t key_len = reply[pos++];
    if (key_len == 0 || key_len > ORBIT_MODEL_KEY_MAX || length != 8 + 1 + key_len + 8 + 6)
        return false;
    memset(version, 0, sizeof(*version));
    version->used = true;
    version->tombstone = (reply[7] & 2) != 0;
    version->key_len = key_len;
    memcpy(version->key, reply + pos, key_len);
    pos += key_len;
    for (size_t i = 0; i < 8; ++i) version->clock = (version->clock << 8) | reply[pos++];
    memcpy(version->author, reply + pos, 6);
    return version->clock != 0 && orbit_model_author_valid(version->author);
}

static bool local_version_for(const orbit_model_entry_t *remote, bool *needs_pull)
{
    if (!take_lock(portMAX_DELAY)) return false;
    const orbit_model_entry_t *local = orbit_model_get(&s_store, remote->key, remote->key_len);
    *needs_pull = local == NULL || orbit_model_version_compare(remote, local) > 0;
    give_lock();
    return true;
}

static bool sync_rpc(const char *id, uint8_t command, uint8_t index,
                     uint8_t *reply, size_t *reply_len)
{
    uint8_t request[ORBIT_RPC_PREFIX_SIZE + 1];
    memcpy(request, s_magic, 4);
    request[4] = command;
    request[5] = index;
    int received = ecos_family_orbit(id, request, sizeof(request), reply, ECOS_ORBIT_MAX,
                                     ORBIT_SYNC_TIMEOUT_MS);
    if (received <= 0) return false;
    *reply_len = (size_t)received;
    return true;
}

static bool send_entry(const char *id, const orbit_model_entry_t *entry)
{
    uint8_t request[ECOS_ORBIT_MAX], reply[6];
    memcpy(request, s_magic, 4);
    request[4] = ORBIT_CMD_MERGE;
    size_t length = orbit_model_encode(entry, request + ORBIT_RPC_PREFIX_SIZE,
                                       sizeof(request) - ORBIT_RPC_PREFIX_SIZE);
    if (length == 0) return false;
    int received = ecos_family_orbit(id, request, ORBIT_RPC_PREFIX_SIZE + length,
                                     reply, sizeof(reply), ORBIT_SYNC_TIMEOUT_MS);
    return received == 6 && memcmp(reply, s_magic, 4) == 0 && reply[4] == ORBIT_CMD_MERGE && reply[5] == 1;
}

static void sync_worker(void *arg)
{
    (void)arg;
    orbit_model_entry_t *remote_entries = NULL;
    bool held = ecos_family_hold_active();
    if (!held) goto done;
    ecos_family_t *family = heap_caps_calloc(1, sizeof(*family), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (family == NULL) goto cleanup;
    ecos_family(family);
    int selected = -1;
    if (family->radio && family->count > 0)
    {
        for (int step = 0; step < family->count; ++step)
        {
            size_t at = (s_peer_cursor + (size_t)step) % (size_t)family->count;
            if (family->members[at].reachable)
            {
                selected = (int)at;
                s_peer_cursor = at + 1;
                break;
            }
        }
    }
    if (selected >= 0)
    {
        const char *id = family->members[selected].id;
        uint8_t reply[ECOS_ORBIT_MAX];
        remote_entries = heap_caps_calloc(ORBIT_MODEL_CAPACITY, sizeof(*remote_entries),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (remote_entries == NULL) goto cleanup;
        /* Pull only remote records newer than local; each RPC is outside the
         * state mutex. A failed call ends this bounded turn. */
        uint8_t request[ORBIT_RPC_PREFIX_SIZE + 1];
        memcpy(request, s_magic, 4);
        request[4] = ORBIT_CMD_SUMMARY;
        uint8_t remote_count = 0;
        bool summary_ok = true;
        for (uint8_t index = 0; index < ORBIT_MODEL_CAPACITY; ++index)
        {
            if (ecos_family_stream_active()) { summary_ok = false; break; }
            request[5] = index;
            int n = ecos_family_orbit(id, request, sizeof(request), reply, sizeof(reply), ORBIT_SYNC_TIMEOUT_MS);
            if (n <= 0)
            {
                summary_ok = false;
                break;
            }
            orbit_model_entry_t remote;
            uint8_t count = 0, returned_index = 0;
            if (!parse_summary(reply, (size_t)n, &count, &returned_index, &remote) || returned_index != index)
            {
                summary_ok = false;
                break;
            }
            remote_count = count;
            if (index >= count) break;
            remote_entries[index] = remote;
            bool pull = false;
            if (!local_version_for(&remote, &pull)) break;
            if (!pull) continue;
            size_t get_len = 0;
            if (!sync_rpc(id, ORBIT_CMD_GET, index, reply, &get_len) || get_len < 9 ||
                memcmp(reply, s_magic, 4) != 0 || reply[4] != ORBIT_CMD_GET || reply[7] != 1 ||
                reply[5] != remote_count || reply[6] != index)
            {
                summary_ok = false;
                break;
            }
            orbit_model_entry_t full;
            if (!orbit_model_decode(reply + 8, get_len - 8, &full) ||
                full.key_len != remote.key_len || memcmp(full.key, remote.key, remote.key_len) != 0 ||
                orbit_model_version_compare(&full, &remote) < 0 || !merge_entry(&full))
            {
                summary_ok = false;
                break;
            }
        }
        if (summary_ok)
        {
            /* Push only records newer than the full-key summary snapshot. */
            for (uint8_t index = 0; index < ORBIT_MODEL_CAPACITY; ++index)
            {
                if (ecos_family_stream_active()) break;
                orbit_model_entry_t copy = {0};
                bool present = false;
                if (!take_lock(portMAX_DELAY)) break;
                const orbit_model_entry_t *entry = entry_by_index_locked(index);
                if (entry != NULL) { copy = *entry; present = true; }
                give_lock();
                if (!present) break;
                const orbit_model_entry_t *remote = NULL;
                for (uint8_t r = 0; r < remote_count; ++r)
                    if (orbit_model_same_key(&remote_entries[r], copy.key, copy.key_len))
                    {
                        remote = &remote_entries[r];
                        break;
                    }
                if ((remote == NULL || orbit_model_version_compare(&copy, remote) > 0) &&
                    !send_entry(id, &copy)) break;
            }
        }
    }
cleanup:
    if (family != NULL) heap_caps_free(family);
    if (remote_entries != NULL) heap_caps_free(remote_entries);
    if (held) ecos_family_hold(false);
done:
    sync_pending_set(false);
    vTaskDelete(NULL);
}

static void sync_start_job(void *arg)
{
    (void)arg;
    if (!s_ready)
    {
        sync_pending_set(false);
        return;
    }
    if (!ecos_link_running() || !sys_task_create(sync_worker, "orbit_sync", 6144, NULL, 2, SYS_CORE_ANY))
    {
        sync_pending_set(false);
    }
}

void orbit_state_tick(void)
{
    if (!s_ready) return;
    if (!ecos_link_running() || ecos_family_stream_active()) return;
    int64_t now = esp_timer_get_time();
    if (!take_lock(0)) return;
    bool schedule = !sync_pending_get() &&
                    (now - s_last_sync_us >= ORBIT_SYNC_INTERVAL_US);
    if (schedule)
    {
        sync_pending_set(true);
        s_last_sync_us = now;
    }
    give_lock();
    if (schedule && !sys_post(sync_start_job, NULL)) sync_pending_set(false);
}
