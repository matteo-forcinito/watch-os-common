#include "services/market.h"
#include "services/fw_version.h"
#include "services/wifi.h"
#include "services/storage.h"
#include "core/power.h"
#include "core/sys.h"
#include "hardware/sdcard.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "psa/crypto.h"

#include <errno.h>
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define TRANSFER_CHUNK 8192

typedef struct
{
    esp_http_client_handle_t client;
    char redirect[2048];
    bool truncated;
} connection_t;

typedef struct
{
    bool refresh;
    bool remove;
    market_entry_t entry;
} job_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static market_entry_t *s_entries;
static int s_count;
static market_status_t s_status;
static atomic_bool s_busy;
static atomic_bool s_cancel;
static atomic_bool s_development;
static atomic_int s_view;
static bool s_initialized;

static void status_set(market_phase_t phase, int percent, const char *message)
{
    portENTER_CRITICAL(&s_lock);
    s_status.phase = phase;
    s_status.percent = percent;
    snprintf(s_status.message, sizeof(s_status.message), "%s", message);
    portEXIT_CRITICAL(&s_lock);
}

void market_status(market_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
    out->busy = atomic_load(&s_busy);
}

bool market_busy(void) { return atomic_load(&s_busy); }
bool market_development(void) { return atomic_load(&s_development); }
int market_view(void) { return atomic_load(&s_view); }

static void save_preferences(void *arg)
{
    (void)arg;
    nvs_handle_t handle;
    if (nvs_open("market", NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_set_u8(handle, "dev", market_development());
        nvs_set_u8(handle, "view", (uint8_t)market_view());
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void market_set_development(bool enabled)
{
    atomic_store(&s_development, enabled);
    portENTER_CRITICAL(&s_lock);
    s_status.revision++;
    portEXIT_CRITICAL(&s_lock);
    sys_post(save_preferences, NULL);
}

void market_set_view(int view)
{
    if (view < 0 || view > 2) return;
    atomic_store(&s_view, view);
    sys_post(save_preferences, NULL);
}

/* cJSON nodes are small: by default they land in the scarce internal RAM. Keep them in PSRAM. */
static void *json_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void json_hooks(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
}

esp_err_t market_init(void)
{
    json_hooks();
    if (s_initialized) return ESP_OK;
    nvs_handle_t handle;
    if (nvs_open("market", NVS_READONLY, &handle) == ESP_OK)
    {
        uint8_t value = 0;
        if (nvs_get_u8(handle, "dev", &value) == ESP_OK) atomic_store(&s_development, value == 1);
        if (nvs_get_u8(handle, "view", &value) == ESP_OK && value <= 2) atomic_store(&s_view, value);
        nvs_close(handle);
    }
    s_initialized = true;
    return ESP_OK;
}

/* The dev channel includes stable apps and chooses the newest version per id.
 * Selecting stable never offers a development image or silently downgrades. */
static bool visible(int index)
{
    market_entry_t *entry = &s_entries[index];
    if (!market_development() && strcmp(entry->channel, "stable") != 0) return false;
    for (int i = 0; i < s_count; i++)
    {
        if (i == index || strcmp(s_entries[i].id, entry->id) != 0) continue;
        if (!market_development() && strcmp(s_entries[i].channel, "stable") != 0) continue;
        if (fw_version_compare(s_entries[i].version, entry->version) > 0) return false;
    }
    return true;
}

int market_count(void)
{
    int count = 0;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_count; i++) if (visible(i)) count++;
    portEXIT_CRITICAL(&s_lock);
    return count;
}

bool market_entry(int index, market_entry_t *out)
{
    bool found = false;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_count; i++)
    {
        if (visible(i) && index-- == 0)
        {
            *out = s_entries[i];
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}

void market_cancel(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_status.phase == MARKET_FETCHING || s_status.phase == MARKET_DOWNLOADING ||
        s_status.phase == MARKET_VERIFYING) atomic_store(&s_cancel, true);
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t headers(esp_http_client_event_t *event)
{
    connection_t *connection = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(event->header_key, "Location") == 0)
    {
        connection->truncated = strlen(event->header_value) >= sizeof(connection->redirect);
        snprintf(connection->redirect, sizeof(connection->redirect), "%s", event->header_value);
    }
    return ESP_OK;
}

static void close_connection(connection_t *connection)
{
    if (connection->client != NULL)
    {
        esp_http_client_close(connection->client);
        esp_http_client_cleanup(connection->client);
    }
    free(connection);
}

static esp_err_t open_connection(const char *url, connection_t **out)
{
    *out = NULL;
    connection_t *connection = calloc(1, sizeof(*connection));
    char *current = malloc(2048);
    if (connection == NULL || current == NULL)
    {
        free(connection);
        free(current);
        return ESP_ERR_NO_MEM;
    }
    snprintf(current, 2048, "%s", url);
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    for (int hop = 0; hop < 5 && !atomic_load(&s_cancel); hop++)
    {
        if (!market_trusted_url(current)) break;
        connection->redirect[0] = '\0';
        connection->truncated = false;
        esp_http_client_config_t config = {
            .url = current, .timeout_ms = 8000,
            .buffer_size = 4096, .buffer_size_tx = 4096,
            .user_agent = "WatchOS-Market/1.0", .disable_auto_redirect = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = headers, .user_data = connection,
        };
        connection->client = esp_http_client_init(&config);
        if (connection->client == NULL)
        {
            err = ESP_ERR_NO_MEM;
            break;
        }
        err = esp_http_client_open(connection->client, 0);
        if (err != ESP_OK) break;
        if (esp_http_client_fetch_headers(connection->client) < 0)
        {
            err = ESP_FAIL;
            break;
        }
        int code = esp_http_client_get_status_code(connection->client);
        if (code == 200)
        {
            *out = connection;
            free(current);
            return ESP_OK;
        }
        err = ESP_ERR_INVALID_RESPONSE;
        if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308) break;
        if (connection->truncated || !market_trusted_url(connection->redirect)) break;
        snprintf(current, 2048, "%s", connection->redirect);
        esp_http_client_cleanup(connection->client);
        connection->client = NULL;
    }
    close_connection(connection);
    free(current);
    return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
}

static esp_err_t read_catalog(char **out)
{
    *out = NULL;
    connection_t *connection;
    esp_err_t err = open_connection(MARKET_CATALOG_URL, &connection);
    if (err != ESP_OK) return err;
    char *text = heap_caps_malloc(MARKET_CATALOG_MAX + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (text == NULL)
    {
        close_connection(connection);
        return ESP_ERR_NO_MEM;
    }
    size_t used = 0;
    err = ESP_FAIL;
    while (!atomic_load(&s_cancel) && used <= MARKET_CATALOG_MAX)
    {
        int read = esp_http_client_read(connection->client, text + used, MARKET_CATALOG_MAX + 1 - used);
        if (read < 0) break;
        if (read == 0)
        {
            if (esp_http_client_is_complete_data_received(connection->client)) err = ESP_OK;
            break;
        }
        used += read;
    }
    text[used] = '\0';
    close_connection(connection);
    if (err == ESP_OK && used > 0 && memchr(text, '\0', used) == NULL) *out = text;
    else
    {
        free(text);
        if (err == ESP_OK) err = ESP_ERR_INVALID_RESPONSE;
    }
    return err;
}

static bool regular(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static void installed_entries(market_entry_t *entries, int count)
{
    extapp_t *local = heap_caps_calloc(EXTAPP_MAX, sizeof(*local), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (local == NULL) return;
    int total = extapp_scan(local, EXTAPP_MAX);
    for (int i = 0; i < count; i++)
    {
        entries[i].installed = false;
        entries[i].installed_version[0] = '\0';
        for (int j = 0; j < total; j++)
        {
            if (strcmp(entries[i].id, local[j].id) == 0)
            {
                entries[i].installed = true;
                entries[i].local = local[j];
                snprintf(entries[i].installed_version, sizeof(entries[i].installed_version), "%s", local[j].version);
                break;
            }
        }
    }
    free(local);
}

static esp_err_t refresh_catalog(void)
{
    char *text;
    esp_err_t err = read_catalog(&text);
    if (err != ESP_OK) return err;
    market_entry_t *entries = heap_caps_calloc(MARKET_MAX_APPS, sizeof(*entries), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int count = 0;
    if (entries == NULL) err = ESP_ERR_NO_MEM;
    else err = market_parse_catalog(text, esp_app_get_description()->project_name, entries, MARKET_MAX_APPS, &count);
    free(text);
    if (err != ESP_OK)
    {
        free(entries);
        return err;
    }
    installed_entries(entries, count);
    portENTER_CRITICAL(&s_lock);
    market_entry_t *previous = s_entries;
    s_entries = entries;
    s_count = count;
    s_status.revision++;
    portEXIT_CRITICAL(&s_lock);
    free(previous);
    return ESP_OK;
}

static esp_err_t download_file(const char *url, const char *path, size_t expected,
                               const char *expected_hash, int start, int weight)
{
    connection_t *connection;
    esp_err_t err = open_connection(url, &connection);
    if (err != ESP_OK) return err;
    int64_t length = esp_http_client_get_content_length(connection->client);
    if (length > 0 && (uint64_t)length != expected)
    {
        close_connection(connection);
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *file = fopen(path, "wb");
    uint8_t *buffer = heap_caps_malloc(TRANSFER_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    size_t used = 0;
    err = ESP_FAIL;
    if (file == NULL || buffer == NULL || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) goto done;
    while (!atomic_load(&s_cancel))
    {
        size_t available = expected - used + 1;
        int wanted = available < TRANSFER_CHUNK ? (int)available : TRANSFER_CHUNK;
        int read = esp_http_client_read(connection->client, (char *)buffer, wanted);
        if (read < 0) break;
        if (read == 0)
        {
            if (used != expected || !esp_http_client_is_complete_data_received(connection->client)) break;
            uint8_t digest[32];
            size_t digest_size = 0;
            char hex[65];
            if (psa_hash_finish(&hash, digest, sizeof(digest), &digest_size) != PSA_SUCCESS || digest_size != 32) break;
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
            if (strcmp(hex, expected_hash) != 0) break;
            if (fflush(file) != 0 || fsync(fileno(file)) != 0) break;
            err = ESP_OK;
            break;
        }
        if (used + read > expected || fwrite(buffer, 1, read, file) != (size_t)read ||
            psa_hash_update(&hash, buffer, read) != PSA_SUCCESS) break;
        used += read;
        status_set(MARKET_DOWNLOADING, start + (int)(used * weight / expected), "Download verificato su GitHub");
    }
done:
    psa_hash_abort(&hash);
    if (file != NULL && fclose(file) != 0) err = ESP_FAIL;
    free(buffer);
    close_connection(connection);
    if (err != ESP_OK) unlink(path);
    return err;
}

#define IMAGE_HEADER 288

static esp_err_t check_header(const uint8_t *header, size_t length, const market_entry_t *entry)
{
    uint32_t descriptor;
    memcpy(&descriptor, header + 32, sizeof(descriptor));
    if (length != IMAGE_HEADER || header[0] != 0xE9 || header[1] < 1 || header[1] > 16 ||
        header[12] != 9 || header[13] != 0 || descriptor != 0xABCD5432 ||
        memchr(header + 48, 0, 32) == NULL || memchr(header + 80, 0, 32) == NULL ||
        strcmp((char *)header + 48, entry->version) != 0 || strcmp((char *)header + 80, entry->project) != 0)
        return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

static esp_err_t check_image(const char *path, const market_entry_t *entry)
{
    uint8_t header[IMAGE_HEADER];
    FILE *file = fopen(path, "rb");
    if (file == NULL) return ESP_FAIL;
    size_t length = fread(header, 1, sizeof(header), file);
    fclose(file);
    return check_header(header, length, entry);
}

static esp_err_t check_icon(const char *path)
{
    uint8_t header[24];
    FILE *file = fopen(path, "rb");
    if (file == NULL) return ESP_FAIL;
    size_t length = fread(header, 1, sizeof(header), file);
    fclose(file);
    if (length != sizeof(header) || memcmp(header, "\x89PNG\r\n\x1a\n", 8) != 0 ||
        memcmp(header + 12, "IHDR", 4) != 0) return ESP_ERR_INVALID_RESPONSE;
    uint32_t width = ((uint32_t)header[16] << 24) | ((uint32_t)header[17] << 16) | ((uint32_t)header[18] << 8) | header[19];
    uint32_t height = ((uint32_t)header[20] << 24) | ((uint32_t)header[21] << 16) | ((uint32_t)header[22] << 8) | header[23];
    return width > 0 && width <= 256 && height > 0 && height <= 256 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static bool file_hash_matches(const char *path, const char *expected)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    uint8_t *buffer = heap_caps_malloc(TRANSFER_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    bool ok = buffer != NULL && psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS;
    while (ok)
    {
        size_t length = fread(buffer, 1, TRANSFER_CHUNK, file);
        if (length == 0)
        {
            ok = !ferror(file);
            break;
        }
        ok = psa_hash_update(&hash, buffer, length) == PSA_SUCCESS;
    }
    uint8_t digest[32];
    size_t length = 0;
    char hex[65];
    if (ok) ok = psa_hash_finish(&hash, digest, sizeof(digest), &length) == PSA_SUCCESS && length == 32;
    if (ok)
    {
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
        ok = strcmp(hex, expected) == 0;
    }
    psa_hash_abort(&hash);
    fclose(file);
    free(buffer);
    return ok;
}

static esp_err_t adopt_payload(const char *temporary, const char *destination, const char *hash)
{
    if (!regular(destination)) return rename(temporary, destination) == 0 ? ESP_OK : ESP_FAIL;
    if (file_hash_matches(destination, hash)) return unlink(temporary) == 0 ? ESP_OK : ESP_FAIL;
    char previous[160];
    snprintf(previous, sizeof(previous), "%s.repair", destination);
    if (regular(previous) && unlink(previous) != 0) return ESP_FAIL;
    if (rename(destination, previous) != 0) return ESP_FAIL;
    if (rename(temporary, destination) != 0)
    {
        rename(previous, destination);
        return ESP_FAIL;
    }
    unlink(previous);
    return ESP_OK;
}

/* Manifest publication is the commit point. Payloads get content-derived names,
 * so the previous app and user files stay intact throughout an update. */
static esp_err_t publish(const char *folder, const market_entry_t *entry,
                          const char *binary_name, const char *icon_name, bool removed)
{
    char manifest[128], pending[128], previous[128];
    snprintf(manifest, sizeof(manifest), "%s/manifest.json", folder);
    snprintf(pending, sizeof(pending), "%s/.market-manifest", folder);
    snprintf(previous, sizeof(previous), "%s/.manifest.previous", folder);
    if (!regular(manifest) && regular(previous) && rename(previous, manifest) != 0) return ESP_FAIL;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    bool ok = cJSON_AddStringToObject(root, "name", entry->name) != NULL &&
              cJSON_AddStringToObject(root, "bin", binary_name) != NULL &&
              cJSON_AddStringToObject(root, "icon", icon_name) != NULL &&
              cJSON_AddStringToObject(root, "version", entry->version) != NULL &&
              cJSON_AddStringToObject(root, "channel", entry->channel) != NULL &&
              cJSON_AddStringToObject(root, "board", entry->board) != NULL;
    if (removed) ok = cJSON_AddBoolToObject(root, "removed", true) != NULL && ok;
    char *text = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (text == NULL) return ESP_ERR_NO_MEM;
    FILE *file = fopen(pending, "wb");
    ok = file != NULL;
    if (ok)
    {
        ok = fwrite(text, 1, strlen(text), file) == strlen(text) && fflush(file) == 0 && fsync(fileno(file)) == 0;
        if (fclose(file) != 0) ok = false;
    }
    free(text);
    if (!ok) return ESP_FAIL;
    if (regular(manifest))
    {
        if (regular(previous) && unlink(previous) != 0) return ESP_FAIL;
        if (rename(manifest, previous) != 0) return ESP_FAIL;
    }
    if (rename(pending, manifest) != 0)
    {
        if (regular(previous)) rename(previous, manifest);
        return ESP_FAIL;
    }
    extapp_forget_cache();
    return ESP_OK;
}

static bool managed_payload(const char *name, const char *id)
{
    char legacy[40];
    snprintf(legacy, sizeof(legacy), "%s.bin", id);
    if (strcmp(name, legacy) == 0 || strcmp(name, "icon.png") == 0 ||
        strcmp(name, "icon.bin") == 0 || strcmp(name, ".manifest.previous") == 0 ||
        strcmp(name, ".market-download") == 0 || strcmp(name, ".market-manifest") == 0 ||
        strcmp(name, ".upload") == 0 || strcmp(name, ".upload-manifest") == 0 ||
        strcmp(name, ".previous") == 0) return true;
    int prefix;
    const char *suffix;
    if (strncmp(name, "app-", 4) == 0) { prefix = 4; suffix = ".bin"; }
    else if (strncmp(name, "icon-", 5) == 0) { prefix = 5; suffix = ".png"; }
    else return false;
    size_t length = strlen(name);
    size_t base = prefix + 16 + strlen(suffix);
    if (length != base && (length != base + 7 || strcmp(name + base, ".repair") != 0)) return false;
    for (int i = prefix; i < prefix + 16; i++)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
    return strncmp(name + prefix + 16, suffix, strlen(suffix)) == 0;
}

static void refresh_installed_catalog(void)
{
    market_entry_t *copy = heap_caps_malloc(MARKET_MAX_APPS * sizeof(*copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) return;
    int count = s_count;
    memcpy(copy, s_entries, count * sizeof(*copy));
    installed_entries(copy, count);
    portENTER_CRITICAL(&s_lock);
    market_entry_t *previous = s_entries;
    s_entries = copy;
    s_status.revision++;
    portEXIT_CRITICAL(&s_lock);
    free(previous);
}

/*
 * No card (AMOLED 1.75): the app goes straight into the app slot. Only one
 * app fits there; installing another replaces it. The download is hashed while
 * it streams and esp_ota_end() checks the image, so a broken download never
 * becomes launchable. The slot is not touched until the first 288 bytes prove
 * the image is the right app for this board. The icon goes to the flash disk.
 */
static void flash_icon_path(const char *id, char *out, size_t size)
{
    snprintf(out, size, STORAGE_FLASH_MOUNT "/apps/%s/icon.png", id);
}

static void drop_flash_icon(const char *id)
{
    if (!storage_flash_mounted() || id[0] == '\0' || !market_safe_id(id)) return;
    char path[96];
    flash_icon_path(id, path, sizeof(path));
    unlink(path);
    snprintf(path, sizeof(path), STORAGE_FLASH_MOUNT "/apps/%s", id);
    rmdir(path);   /* only if empty: an app may keep its own files there */
}

static esp_err_t stream_to_slot(const market_entry_t *entry, const esp_partition_t *slot)
{
    connection_t *connection;
    esp_err_t err = open_connection(entry->binary_url, &connection);
    if (err != ESP_OK) return err;
    int64_t length = esp_http_client_get_content_length(connection->client);
    if (length > 0 && (uint64_t)length != entry->binary_size)
    {
        close_connection(connection);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buffer = heap_caps_malloc(TRANSFER_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *header = heap_caps_malloc(IMAGE_HEADER, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    esp_ota_handle_t ota = 0;
    bool writing = false;
    size_t used = 0;
    err = ESP_FAIL;
    if (buffer == NULL || header == NULL || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) goto done;
    while (!atomic_load(&s_cancel))
    {
        size_t available = entry->binary_size - used + 1;
        int wanted = available < TRANSFER_CHUNK ? (int)available : TRANSFER_CHUNK;
        int read = esp_http_client_read(connection->client, (char *)buffer, wanted);
        if (read < 0) break;
        if (read == 0)
        {
            if (used != entry->binary_size || !writing ||
                !esp_http_client_is_complete_data_received(connection->client)) break;
            uint8_t digest[32];
            size_t digest_size = 0;
            char hex[65];
            if (psa_hash_finish(&hash, digest, sizeof(digest), &digest_size) != PSA_SUCCESS || digest_size != 32) break;
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
            if (strcmp(hex, entry->binary_sha256) != 0) break;
            status_set(MARKET_VERIFYING, 90, "Verifica compatibilità");
            writing = false;
            err = esp_ota_end(ota);
            break;
        }
        if (used + read > entry->binary_size || psa_hash_update(&hash, buffer, read) != PSA_SUCCESS) break;
        const uint8_t *data = buffer;
        size_t rest = read;
        if (!writing)
        {
            /* Collect the header first: the slot is erased only for a matching image. */
            size_t take = IMAGE_HEADER - used < rest ? IMAGE_HEADER - used : rest;
            memcpy(header + used, data, take);
            used += take;
            data += take;
            rest -= take;
            if (used < IMAGE_HEADER) continue;
            err = check_header(header, IMAGE_HEADER, entry);
            if (err != ESP_OK) break;
            err = ESP_FAIL;
            extapp_slot_forget();
            if (esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) break;
            writing = true;
            if (esp_ota_write(ota, header, IMAGE_HEADER) != ESP_OK) break;
        }
        if (rest > 0 && esp_ota_write(ota, data, rest) != ESP_OK) break;
        used += rest;
        status_set(MARKET_DOWNLOADING, (int)(used * 88 / entry->binary_size), "Scrittura diretta nella memoria app");
    }
done:
    if (writing) esp_ota_abort(ota);
    psa_hash_abort(&hash);
    free(buffer);
    free(header);
    close_connection(connection);
    return err;
}

static esp_err_t install_direct(const market_entry_t *entry)
{
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (slot == NULL || entry->binary_size > slot->size) return ESP_ERR_INVALID_SIZE;
    extapp_t previous;
    bool had = extapp_slot_app(&previous);
    esp_err_t err = stream_to_slot(entry, slot);
    if (err != ESP_OK)
    {
        if (had) drop_flash_icon(previous.id);   /* the slot may now be half written */
        refresh_installed_catalog();
        return err;
    }
    if (had && strcmp(previous.id, entry->id) != 0) drop_flash_icon(previous.id);
    char icon[64] = "";
    if (storage_flash_mounted() && !atomic_load(&s_cancel))
    {
        char folder[64], temporary[96], path[96];
        snprintf(folder, sizeof(folder), STORAGE_FLASH_MOUNT "/apps/%s", entry->id);
        snprintf(temporary, sizeof(temporary), "%s/.market-download", folder);
        flash_icon_path(entry->id, path, sizeof(path));
        if ((mkdir(STORAGE_FLASH_MOUNT "/apps", 0775) == 0 || errno == EEXIST) &&
            (mkdir(folder, 0775) == 0 || errno == EEXIST) &&
            download_file(entry->icon_url, temporary, entry->icon_size, entry->icon_sha256, 90, 8) == ESP_OK)
        {
            if (check_icon(temporary) == ESP_OK && (unlink(path), rename(temporary, path) == 0))
                snprintf(icon, sizeof(icon), "F:/apps/%s/icon.png", entry->id);
            else unlink(temporary);
        }
    }
    status_set(MARKET_PUBLISHING, 98, "Aggiunta al launcher");
    err = extapp_slot_note(entry->id, entry->name, entry->version, icon);
    refresh_installed_catalog();
    return err;
}

static esp_err_t remove_direct(const market_entry_t *entry)
{
    extapp_t current;
    if (!extapp_slot_app(&current) || strcmp(current.id, entry->id) != 0) return ESP_ERR_NOT_FOUND;
    status_set(MARKET_PUBLISHING, 0, "Rimozione dell'app");
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    extapp_slot_forget();
    /* Erasing the first sector is enough: the slot no longer holds a valid image. */
    esp_err_t err = esp_partition_erase_range(slot, 0, slot->erase_size);
    drop_flash_icon(entry->id);
    refresh_installed_catalog();
    if (err == ESP_OK) status_set(MARKET_DONE, 100, "App eliminata.");
    return err;
}

static esp_err_t remove_entry(const market_entry_t *entry)
{
    if (!sdcard_is_mounted()) return remove_direct(entry);
    char folder[80];
    snprintf(folder, sizeof(folder), SDCARD_MOUNT "/apps/%s", entry->id);
    status_set(MARKET_PUBLISHING, 0, "Rimozione dell'app");
    if (atomic_load(&s_cancel)) return ESP_FAIL;
    /* A durable tombstone hides the app even if power fails during cleanup.
     * A later verified installation replaces it with a normal manifest. */
    esp_err_t err = publish(folder, entry, "", "", true);
    if (err != ESP_OK) return err;
    DIR *directory = opendir(folder);
    bool cleaned = directory != NULL;
    if (directory != NULL)
    {
        struct dirent *file;
        while ((file = readdir(directory)) != NULL)
        {
            if (!managed_payload(file->d_name, entry->id)) continue;
            char path[160];
            snprintf(path, sizeof(path), "%s/%.48s", folder, file->d_name);
            if (regular(path) && unlink(path) != 0) cleaned = false;
        }
        closedir(directory);
    }
    refresh_installed_catalog();
    status_set(MARKET_DONE, 100, cleaned ? "App eliminata. Dati personali conservati." :
                                         "App rimossa. Alcuni file richiedono pulizia.");
    return ESP_OK;
}

static esp_err_t install_entry(const market_entry_t *entry)
{
    if (!sdcard_is_mounted()) return install_direct(entry);
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (slot == NULL || entry->binary_size > slot->size) return ESP_ERR_INVALID_SIZE;
    uint32_t total, available;
    sdcard_space(&total, &available);
    if ((uint64_t)available * 1024 * 1024 < entry->binary_size + entry->icon_size + 1024 * 1024)
        return ESP_ERR_NO_MEM;
    char folder[80], temporary[128], binary[128], icon[128], binary_name[48], icon_name[48];
    if (mkdir(SDCARD_MOUNT "/apps", 0775) != 0 && errno != EEXIST) return ESP_FAIL;
    snprintf(folder, sizeof(folder), SDCARD_MOUNT "/apps/%s", entry->id);
    if (mkdir(folder, 0775) != 0 && errno != EEXIST) return ESP_FAIL;
    snprintf(temporary, sizeof(temporary), "%s/.market-download", folder);
    snprintf(binary_name, sizeof(binary_name), "app-%.16s.bin", entry->binary_sha256);
    snprintf(icon_name, sizeof(icon_name), "icon-%.16s.png", entry->icon_sha256);
    snprintf(binary, sizeof(binary), "%s/%s", folder, binary_name);
    snprintf(icon, sizeof(icon), "%s/%s", folder, icon_name);
    esp_err_t err = download_file(entry->binary_url, temporary, entry->binary_size, entry->binary_sha256, 0, 85);
    if (err != ESP_OK) return err;
    status_set(MARKET_VERIFYING, 86, "Verifica compatibilità");
    err = check_image(temporary, entry);
    if (err != ESP_OK || atomic_load(&s_cancel))
    {
        unlink(temporary);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    /* Equal content means reinstalling the same release. Keep the published
     * file intact; the downloaded copy has already passed its full hash. */
    err = adopt_payload(temporary, binary, entry->binary_sha256);
    if (err != ESP_OK) return err;
    err = download_file(entry->icon_url, temporary, entry->icon_size, entry->icon_sha256, 85, 10);
    if (err != ESP_OK) return err;
    err = check_icon(temporary);
    if (err != ESP_OK || atomic_load(&s_cancel))
    {
        unlink(temporary);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    err = adopt_payload(temporary, icon, entry->icon_sha256);
    if (err != ESP_OK) return err;
    status_set(MARKET_PUBLISHING, 98, "Aggiunta alla libreria");
    if (atomic_load(&s_cancel)) return ESP_FAIL;
    err = publish(folder, entry, binary_name, icon_name, false);
    if (err == ESP_OK)
    {
        /* Workers own catalog mutations. Readers copy under the same lock. */
        refresh_installed_catalog();
    }
    return err;
}

static void worker(void *arg)
{
    job_t *job = arg;
    power_keep_screen_on(true);
    esp_err_t err = job->remove ? remove_entry(&job->entry) : wifi_acquire(15000);
    if (!job->remove && err == ESP_OK)
    {
        wifi_set_fast(true);
        err = job->refresh ? refresh_catalog() : install_entry(&job->entry);
        wifi_set_fast(false);
        wifi_release();
    }
    if (atomic_load(&s_cancel)) status_set(MARKET_CANCELLED, 0, "Download annullato. App precedente conservata.");
    else if (job->remove && err == ESP_OK) { /* remove_entry published its outcome. */ }
    else if (err == ESP_OK) status_set(job->refresh ? MARKET_READY : MARKET_DONE, 100,
                                      job->refresh ? "Catalogo pronto" :
                                      sdcard_is_mounted() ? "App salvata. Puoi aprirla dalla libreria." :
                                                            "App installata. La trovi nel launcher.");
    else if (err == ESP_ERR_INVALID_SIZE) status_set(MARKET_FAILED, 0, "App troppo grande per questo dispositivo.");
    else if (err == ESP_ERR_NO_MEM) status_set(MARKET_FAILED, 0, "Memoria o spazio insufficienti.");
    else status_set(MARKET_FAILED, 0, job->remove ? "Rimozione non completata. Riprova." :
                                                "Download non completato. Controlla rete e riprova.");
    power_keep_screen_on(false);
    storage_files_release();
    free(job);
    atomic_store(&s_busy, false);
    vTaskDelete(NULL);
}

static esp_err_t start(job_t *job)
{
    if (job == NULL) return ESP_ERR_NO_MEM;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true))
    {
        free(job);
        return ESP_ERR_INVALID_STATE;
    }
    if (!storage_files_acquire())
    {
        free(job);
        atomic_store(&s_busy, false);
        status_set(MARKET_FAILED, 0, "Memoria file occupata. Riprova tra poco.");
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_cancel, false);
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.app_id, sizeof(s_status.app_id), "%s", job->refresh ? "" : job->entry.id);
    portEXIT_CRITICAL(&s_lock);
    status_set(job->refresh ? MARKET_FETCHING : MARKET_DOWNLOADING, 0,
               job->refresh ? "Esploro il catalogo..." : job->remove ? "Rimozione..." : "Connessione a GitHub...");
    if (!sys_task_create(worker, "market", 8192, job, 3, 0))
    {
        free(job);
        atomic_store(&s_busy, false);
        storage_files_release();
        status_set(MARKET_FAILED, 0, "Memoria insufficiente per il download.");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t market_refresh(void)
{
    job_t *job = calloc(1, sizeof(*job));
    if (job != NULL) job->refresh = true;
    return start(job);
}

esp_err_t market_install(const char *id)
{
    if (!market_safe_id(id)) return ESP_ERR_INVALID_STATE;
    job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) return ESP_ERR_NO_MEM;
    bool found = false;
    for (int i = 0; market_entry(i, &job->entry); i++)
    {
        if (strcmp(job->entry.id, id) == 0)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        free(job);
        return ESP_ERR_NOT_FOUND;
    }
    return start(job);
}

esp_err_t market_remove(const char *id)
{
    if (!market_safe_id(id)) return ESP_ERR_INVALID_STATE;
    job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) return ESP_ERR_NO_MEM;
    for (int i = 0; market_entry(i, &job->entry); i++)
    {
        if (strcmp(job->entry.id, id) == 0 && job->entry.installed)
        {
            job->remove = true;
            return start(job);
        }
    }
    free(job);
    return ESP_ERR_NOT_FOUND;
}
