#include "services/market.h"
#include "services/fw_version.h"
#include "cJSON.h"
#include "sdkconfig.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool market_safe_id(const char *id)
{
    if (id == NULL || !((id[0] >= 'A' && id[0] <= 'Z') || (id[0] >= 'a' && id[0] <= 'z')) || strlen(id) > 31)
    {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)id; *p; p++)
    {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) && *p != '_' && *p != '-') return false;
    }
    return true;
}

bool market_trusted_url(const char *url)
{
    if (url == NULL || strncmp(url, "https://", 8) != 0) return false;
    const char *host = url + 8;
    size_t length = strcspn(host, "/?");
    static const char *const allowed[] = {
        "github.com", "raw.githubusercontent.com", "release-assets.githubusercontent.com",
        "objects.githubusercontent.com", "github-releases.githubusercontent.com"
    };
    for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
    {
        if (strlen(allowed[i]) == length && strncmp(host, allowed[i], length) == 0)
        {
            return strchr(url, '\r') == NULL && strchr(url, '\n') == NULL && strchr(url, '#') == NULL;
        }
    }
    return false;
}

static bool copy_text(cJSON *object, const char *key, char *out, size_t size)
{
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
    if (text == NULL || text[0] == '\0' || strlen(text) >= size) return false;
    memcpy(out, text, strlen(text) + 1);
    return true;
}

static bool valid_hash(const char *text)
{
    if (strlen(text) != 64) return false;
    for (unsigned i = 0; i < 64; i++)
    {
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    }
    return true;
}

static bool version_valid(const char *text, bool dev)
{
    unsigned values[3];
    int end = 0;
    if (sscanf(text, "%u.%u.%u%n", &values[0], &values[1], &values[2], &end) != 3 ||
        values[0] > 9999 || values[1] > 9999 || values[2] > 9999) return false;
    char stable[32];
    snprintf(stable, sizeof(stable), "%u.%u.%u", values[0], values[1], values[2]);
    if (!dev) return strcmp(text, stable) == 0;
    if (strncmp(text, stable, strlen(stable)) != 0 || strncmp(text + end, "-dev.", 5) != 0) return false;
    const char *number = text + end + 5;
    if (*number == '\0' || (*number == '0' && number[1] != '\0')) return false;
    for (; *number; number++) if (!isdigit((unsigned char)*number)) return false;
    return true;
}

static bool asset(cJSON *object, const char *key, char *url, char *hash, size_t *size, size_t maximum)
{
    cJSON *data = cJSON_GetObjectItemCaseSensitive(object, key);
    cJSON *length = cJSON_GetObjectItemCaseSensitive(data, "size");
    if (!cJSON_IsObject(data) || !copy_text(data, "url", url, MARKET_URL_MAX) ||
        !market_trusted_url(url) || !copy_text(data, "sha256", hash, 65) || !valid_hash(hash) ||
        !cJSON_IsNumber(length) || length->valuedouble < 1 || length->valuedouble > maximum ||
        length->valuedouble != (size_t)length->valuedouble) return false;
    *size = (size_t)length->valuedouble;
    return true;
}

esp_err_t market_parse_catalog(const char *text, const char *board,
                               market_entry_t *entries, int capacity, int *count)
{
    *count = 0;
    if (text == NULL || strlen(text) > MARKET_CATALOG_MAX || capacity < 1) return ESP_ERR_INVALID_ARG;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(text, &end, true);
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    if (!cJSON_IsObject(root) || !cJSON_IsNumber(schema) || schema->valuedouble != 2 ||
        !cJSON_IsArray(apps) || cJSON_GetArraySize(apps) > MARKET_CATALOG_ENTRIES_MAX) goto done;

    market_entry_t *entry = calloc(1, sizeof(*entry));
    if (entry == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto done;
    }
    cJSON *app;
    cJSON_ArrayForEach(app, apps)
    {
        memset(entry, 0, sizeof(*entry));
        if (!copy_text(app, "board", entry->board, sizeof(entry->board))) goto invalid;
        if (strcmp(entry->board, board) != 0) continue;
        if (!copy_text(app, "id", entry->id, sizeof(entry->id)) || !market_safe_id(entry->id) ||
            !copy_text(app, "name", entry->name, sizeof(entry->name)) ||
            !copy_text(app, "summary", entry->summary, sizeof(entry->summary)) ||
            !copy_text(app, "version", entry->version, sizeof(entry->version)) ||
            !copy_text(app, "project", entry->project, sizeof(entry->project)) || !market_safe_id(entry->project) ||
            !copy_text(app, "channel", entry->channel, sizeof(entry->channel)) ||
            (strcmp(entry->channel, "stable") != 0 && strcmp(entry->channel, "dev") != 0) ||
            !version_valid(entry->version, strcmp(entry->channel, "dev") == 0) ||
            !asset(app, "binary", entry->binary_url, entry->binary_sha256, &entry->binary_size, 7 * 1024 * 1024) ||
            entry->binary_size < 288 ||
            !asset(app, "icon", entry->icon_url, entry->icon_sha256, &entry->icon_size, MARKET_ICON_MAX)) goto invalid;
        if (strcmp(entry->project, "watcher_os") == 0 || strcmp(entry->project, "amoled_watch") == 0 ||
            strcmp(entry->project, "amoled175_watch") == 0 || strcmp(entry->project, "amoled175c_watch") == 0 ||
            strcmp(entry->project, "knob_os") == 0) goto invalid;
        entry->color = 0x7B6CFF;
        const char *color = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(app, "color"));
        if (color != NULL)
        {
            if (strlen(color) != 6) goto invalid;
            for (int i = 0; i < 6; i++) if (!isxdigit((unsigned char)color[i])) goto invalid;
            entry->color = (uint32_t)strtoul(color, NULL, 16);
        }
        int replace = -1;
        for (int i = 0; i < *count; i++)
        {
            if (strcmp(entries[i].id, entry->id) == 0 && strcmp(entries[i].channel, entry->channel) == 0)
            {
                int order = fw_version_compare(entry->version, entries[i].version);
                if (order == 0) goto invalid;
                replace = order > 0 ? i : -2;
                break;
            }
        }
        if (replace == -2) continue;
        if (replace == -1)
        {
            if (*count == capacity) goto invalid;
            replace = (*count)++;
        }
        entries[replace] = *entry;
    }
    result = ESP_OK;
invalid:
    free(entry);
done:
    cJSON_Delete(root);
    if (result != ESP_OK) *count = 0;
    return result;
}

bool market_catalog_selftest(void)
{
#if CONFIG_WATCH_DEV_CONSOLE
    static const char fixture[] =
        "{\"schema\":2,\"apps\":[{\"id\":\"Orbit\",\"name\":\"ORBIT\",\"summary\":\"Offline\","
        "\"version\":\"1.0.0\",\"board\":\"amoled_watch\",\"project\":\"orbit\",\"channel\":\"stable\","
        "\"binary\":{\"url\":\"https://github.com/a/b/releases/download/v/orbit.bin\",\"size\":1024,"
        "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"},"
        "\"icon\":{\"url\":\"https://raw.githubusercontent.com/a/b/main/icon.png\",\"size\":64,"
        "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}}]}";
    static const struct { const char *key; const char *value; } attacks[] = {
        {"id", "../Orbit"}, {"id", "Orbit/x"}, {"project", "amoled_watch"},
        {"version", "1.0.0-dev.1"}, {"channel", "unknown"}, {"version", "01.0.0"}
    };
    market_entry_t *entries = calloc(2, sizeof(*entries));
    if (entries == NULL) return false;
    int count;
    bool ok = market_parse_catalog(fixture, "amoled_watch", entries, 2, &count) == ESP_OK &&
              count == 1 && strcmp(entries[0].id, "Orbit") == 0;
    for (unsigned i = 0; i < sizeof(attacks) / sizeof(attacks[0]) && ok; i++)
    {
        cJSON *root = cJSON_Parse(fixture);
        cJSON *app = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "apps"), 0);
        cJSON_ReplaceItemInObject(app, attacks[i].key, cJSON_CreateString(attacks[i].value));
        char *text = cJSON_PrintUnformatted(root);
        ok = text != NULL && market_parse_catalog(text, "amoled_watch", entries, 2, &count) != ESP_OK && count == 0;
        free(text);
        cJSON_Delete(root);
    }
    ok = ok && market_parse_catalog(fixture, "watcher_os", entries, 2, &count) == ESP_OK && count == 0;
    ok = ok && !market_trusted_url("http://github.com/a") &&
         !market_trusted_url("https://github.com.evil.example/a") &&
         !market_trusted_url("https://name@github.com/a") &&
         !market_trusted_url("https://github.com:80/a") &&
         market_trusted_url("https://release-assets.githubusercontent.com/a?signature=x");
    free(entries);
    return ok;
#else
    return false;
#endif
}
