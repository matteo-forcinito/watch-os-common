#include "link_family.h"
#include "link_gateway.h"
#include "link_orbit_json.h"
#include "services/ecos.h"
#include "services/ecos_orbit.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static cJSON *family_error(int *status, int code, const char *message)
{
    *status = code;
    cJSON *out = cJSON_CreateObject();
    if (out != NULL) cJSON_AddStringToObject(out, "error", message);
    return out;
}

static cJSON *family_member_json(const ecos_member_t *member)
{
    cJSON *out = cJSON_CreateObject();
    if (out != NULL)
    {
        cJSON_AddStringToObject(out, "id", member->id);
        cJSON_AddStringToObject(out, "name", member->name);
        cJSON_AddStringToObject(out, "board", member->product);
        cJSON_AddStringToObject(out, "version", member->version);
        cJSON_AddBoolToObject(out, "reachable", member->known && member->reachable && member->seen_ago <= 30);
        cJSON_AddNumberToObject(out, "seen_ago", member->seen_ago);
        cJSON_AddNumberToObject(out, "battery", member->battery);
        cJSON_AddBoolToObject(out, "powered", member->powered || member->usb || member->charging);
    }
    return out;
}

cJSON *link_family_snapshot(void)
{
    ecos_family_t *family = heap_caps_calloc(1, sizeof(*family), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (family == NULL) return NULL;
    ecos_family_hold(true);
    esp_err_t refreshed = ecos_family_refresh();
    int64_t until = esp_timer_get_time() + 4000000;
    do
    {
        ecos_family(family);
        if (refreshed != ESP_OK || !family->busy) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    } while (esp_timer_get_time() < until);
    cJSON *out = cJSON_CreateObject();
    cJSON *members = cJSON_AddArrayToObject(out, "members");
    cJSON_AddBoolToObject(out, "busy", family->busy);
    for (int i = 0; i < family->count; ++i)
    {
        cJSON *member = family_member_json(&family->members[i]);
        if (member != NULL) cJSON_AddItemToArray(members, member);
    }
    ecos_family_hold(false);
    heap_caps_free(family);
    return out;
}

/* The caller authenticates BLE or dashboard access. Peer execution uses the
 * paired EL02 channel, with no IP server or router on either side. */
cJSON *link_family_relay(const cJSON *envelope, int *status)
{
    if (status == NULL) return NULL;
    *status = 200;
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(envelope, "id"));
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(envelope, "p"));
    const cJSON *body = cJSON_GetObjectItemCaseSensitive(envelope, "b");
    if (id == NULL || strlen(id) != 12 || path == NULL)
    {
        return family_error(status, 400, "Serve {id,p,b?}");
    }
    ecos_family_t *family = heap_caps_calloc(1, sizeof(*family), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (family == NULL) return family_error(status, 503, "Memoria insufficiente");
    ecos_family(family);
    const ecos_member_t *target = NULL;
    for (int i = 0; i < family->count; ++i)
        if (strcmp(id, family->members[i].id) == 0) { target = &family->members[i]; break; }
    if (target == NULL)
    {
        heap_caps_free(family);
        return family_error(status, 404, "Membro non abbinato");
    }
    cJSON *metadata = strcmp(path, "/api/system") == 0 && body == NULL ? family_member_json(target) : NULL;
    bool system = strcmp(path, "/api/system") == 0 && body == NULL;
    heap_caps_free(family);
    if (system) return metadata;
    cJSON *command = link_gateway_command(path, body);
    if (command == NULL)
    {
        return family_error(status, 404, "Operazione non disponibile sul bus");
    }
    char *text = cJSON_PrintUnformatted(command);
    cJSON_Delete(command);
    if (text == NULL || strlen(text) > ECOS_ORBIT_REQUEST_MAX)
    {
        cJSON_free(text);
        return family_error(status, 413, "Comando troppo grande");
    }
    uint8_t answer[ECOS_ORBIT_MAX];
    ecos_family_hold(true);
    int received = ecos_family_orbit_large(id, (const uint8_t *)text, strlen(text), answer, sizeof(answer), 10000);
    ecos_family_hold(false);
    cJSON_free(text);
    cJSON *out = received > 0 ? link_orbit_parse_json(answer, (size_t)received) : NULL;
    if (out == NULL)
    {
        return family_error(status, 504, "Esito sconosciuto: non ripetere automaticamente il comando");
    }
    bool rejected = cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(out, "ok")) ||
                    cJSON_GetObjectItemCaseSensitive(out, "error") != NULL;
    out = link_gateway_response(path, out);
    if (rejected) *status = 409;
    return out;
}
