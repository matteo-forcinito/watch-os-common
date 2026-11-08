/* EC01 public handshake over the already authenticated NUS connection. */
#include "ble_ecos.h"
#include "sdkconfig.h"

#if CONFIG_WATCH_ECOS_PAIRING
#include "services/ecos.h"
#include "core/sys.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include <stdio.h>
#include <string.h>

typedef struct
{
    unsigned epoch, users;
    char request[17];
} pairing_t;

static portMUX_TYPE s_guard = portMUX_INITIALIZER_UNLOCKED;
static pairing_t *s_pair;
static ble_ecos_send_t s_send;

static bool request_id(const char *id)
{
    if (id == NULL || strlen(id) != 16) return false;
    for (unsigned i = 0; i < 16; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return true;
}

static void start_error(void *arg)
{
    pairing_t *pair = arg;
    char line[112];
    snprintf(line, sizeof(line), "{\"t\":\"ecos\",\"q\":\"%s\",\"error\":\"busy\"}\n", pair->request);
    if (s_send != NULL) s_send(line, pair->epoch);
    heap_caps_free(pair);
}

static bool pair_send(const uint8_t *frame, size_t length, void *arg)
{
    pairing_t *pair = arg;
    if (frame == NULL && length == 0)
    {
        portENTER_CRITICAL(&s_guard);
        if (s_pair == pair) s_pair = NULL;
        portEXIT_CRITICAL(&s_guard);
        for (;;)
        {
            portENTER_CRITICAL(&s_guard);
            unsigned users = pair->users;
            portEXIT_CRITICAL(&s_guard);
            if (users == 0) break;
            vTaskDelay(1);
        }
        char line[96];
        snprintf(line, sizeof(line), "{\"t\":\"ecos\",\"q\":\"%s\",\"end\":true}\n", pair->request);
        if (s_send != NULL) s_send(line, pair->epoch);
        heap_caps_free(pair);
        return true;
    }
    if (frame == NULL || length == 0 || length > 122 || s_send == NULL) return false;
    unsigned char encoded[165];
    size_t written = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &written, frame, length) != 0) return false;
    char line[248];
    int count = snprintf(line, sizeof(line), "{\"t\":\"ecos\",\"q\":\"%s\",\"d\":\"%.*s\"}\n",
                         pair->request, (int)written, encoded);
    return count > 0 && (size_t)count < sizeof(line) && s_send(line, pair->epoch);
}

void ble_ecos_init(ble_ecos_send_t send)
{
    s_send = send;
}

void ble_ecos_request(const cJSON *request, unsigned epoch)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "q"));
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "action"));
    if (!request_id(id) || action == NULL) return;
    if (strcmp(action, "start") == 0)
    {
        pairing_t *pair = heap_caps_calloc(1, sizeof(*pair), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pair == NULL) return;
        pair->epoch = epoch;
        memcpy(pair->request, id, sizeof(pair->request));
        portENTER_CRITICAL(&s_guard);
        bool busy = s_pair != NULL;
        if (!busy) s_pair = pair;
        portEXIT_CRITICAL(&s_guard);
        if (busy)
        {
            if (!sys_post(start_error, pair)) heap_caps_free(pair);
            return;
        }
        if (ecos_pair_ble_start(pair_send, pair) != ESP_OK)
        {
            portENTER_CRITICAL(&s_guard);
            s_pair = NULL;
            portEXIT_CRITICAL(&s_guard);
            if (!sys_post(start_error, pair)) heap_caps_free(pair);
        }
        return;
    }
    portENTER_CRITICAL(&s_guard);
    pairing_t *pair = s_pair;
    if (pair != NULL && pair->epoch == epoch && strcmp(pair->request, id) == 0) ++pair->users;
    else pair = NULL;
    portEXIT_CRITICAL(&s_guard);
    if (pair == NULL) return;
    if (strcmp(action, "cancel") == 0) ecos_pair_ble_disconnect(pair);
    else if (strcmp(action, "frame") == 0)
    {
        const char *encoded = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "d"));
        uint8_t frame[122];
        size_t length = 0;
        if (encoded != NULL && strlen(encoded) <= 164 &&
            mbedtls_base64_decode(frame, sizeof(frame), &length, (const unsigned char *)encoded, strlen(encoded)) == 0)
            ecos_pair_ble_receive(frame, length, pair);
    }
    portENTER_CRITICAL(&s_guard);
    --pair->users;
    portEXIT_CRITICAL(&s_guard);
}

void ble_ecos_disconnect(unsigned epoch)
{
    portENTER_CRITICAL(&s_guard);
    pairing_t *pair = s_pair;
    if (pair != NULL && pair->epoch == epoch) ++pair->users;
    else pair = NULL;
    portEXIT_CRITICAL(&s_guard);
    if (pair != NULL)
    {
        ecos_pair_ble_disconnect(pair);
        portENTER_CRITICAL(&s_guard);
        --pair->users;
        portEXIT_CRITICAL(&s_guard);
    }
}

#else
void ble_ecos_init(ble_ecos_send_t send) { (void)send; }
void ble_ecos_request(const cJSON *request, unsigned epoch) { (void)request; (void)epoch; }
void ble_ecos_disconnect(unsigned epoch) { (void)epoch; }
#endif
