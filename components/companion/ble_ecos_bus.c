/* Bounded EL02 ciphertext transport over authenticated NUS; no new worker. */
#include "ble_ecos_bus.h"
#include "sdkconfig.h"
#if CONFIG_WATCH_ECOS_PAIRING
#include "services/ecos.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include <stdio.h>
#include <string.h>

typedef struct
{
    unsigned epoch;
    char request[17];
} bridge_t;

static portMUX_TYPE s_guard = portMUX_INITIALIZER_UNLOCKED;
static bridge_t *s_bridge;
static ble_ecos_send_t s_send;
static SemaphoreHandle_t s_operation;

static int hex(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static bool identifier(const char *value, size_t length)
{
    if (value == NULL || strlen(value) != length) return false;
    for (size_t i = 0; i < length; ++i)
    {
        if (hex(value[i]) < 0) return false;
    }
    return true;
}

static bool bridge_send(const uint8_t *frame, size_t length, void *arg)
{
    const bridge_t *bridge = arg;
    if (frame == NULL && length == 0)
    {
        char line[96];
        snprintf(line, sizeof(line), "{\"t\":\"ecosbus\",\"q\":\"%s\",\"end\":true}\n", bridge->request);
        return s_send != NULL && s_send(line, bridge->epoch);
    }
    if (frame == NULL || length < 49 || length > 205 || s_send == NULL) return false;
    unsigned char encoded[277];
    size_t written = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &written, frame, length) != 0) return false;
    char line[352];
    int count = snprintf(line, sizeof(line), "{\"t\":\"ecosbus\",\"q\":\"%s\",\"d\":\"%.*s\"}\n",
                         bridge->request, (int)written, encoded);
    return count > 0 && (size_t)count < sizeof(line) && s_send(line, bridge->epoch);
}

void ble_ecos_bus_init(ble_ecos_send_t send)
{
    s_send = send;
    if (s_operation == NULL) s_operation = xSemaphoreCreateMutex();
}

/* Requests and stop/disconnect are serialized here. The family service drains
 * its worker's send references before close returns, so context can be freed. */
static void disconnect_locked(unsigned epoch)
{
    portENTER_CRITICAL(&s_guard);
    bridge_t *bridge = s_bridge;
    if (bridge != NULL && bridge->epoch == epoch) s_bridge = NULL;
    else bridge = NULL;
    portEXIT_CRITICAL(&s_guard);
    if (bridge != NULL)
    {
        ecos_family_ble_close(bridge);
        heap_caps_free(bridge);
    }
}

static void request_locked(const cJSON *request, unsigned epoch)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "q"));
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "action"));
    if (!identifier(id, 16) || action == NULL) return;
    if (strcmp(action, "open") == 0)
    {
        const char *mac = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "mac"));
        if (!identifier(mac, 12)) return;
        uint8_t bytes[6];
        for (unsigned i = 0; i < 6; ++i) bytes[i] = (uint8_t)((hex(mac[i * 2]) << 4) | hex(mac[i * 2 + 1]));
        portENTER_CRITICAL(&s_guard);
        bool occupied = s_bridge != NULL;
        portEXIT_CRITICAL(&s_guard);
        bool opened = false;
        bridge_t *bridge = occupied ? NULL : heap_caps_calloc(1, sizeof(*bridge), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (bridge != NULL)
        {
            bridge->epoch = epoch;
            memcpy(bridge->request, id, sizeof(bridge->request));
            opened = ecos_family_ble_open(bridge_send, bridge, bytes);
            if (opened)
            {
                portENTER_CRITICAL(&s_guard);
                s_bridge = bridge;
                portEXIT_CRITICAL(&s_guard);
            }
            else heap_caps_free(bridge);
        }
        char line[112];
        snprintf(line, sizeof(line), "{\"t\":\"ecosbus\",\"q\":\"%s\",\"open\":%s}\n", id, opened ? "true" : "false");
        if (s_send == NULL || !s_send(line, epoch))
        {
            if (opened) disconnect_locked(epoch);
        }
        return;
    }
    portENTER_CRITICAL(&s_guard);
    bridge_t *bridge = s_bridge;
    bool current = bridge != NULL && bridge->epoch == epoch && strcmp(bridge->request, id) == 0;
    portEXIT_CRITICAL(&s_guard);
    if (!current) return;
    if (strcmp(action, "close") == 0) disconnect_locked(epoch);
    else if (strcmp(action, "frame") == 0)
    {
        const char *encoded = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "d"));
        uint8_t frame[205];
        size_t length = 0;
        if (encoded != NULL && strlen(encoded) <= 276 &&
            mbedtls_base64_decode(frame, sizeof(frame), &length, (const unsigned char *)encoded, strlen(encoded)) == 0)
        {
            ecos_family_ble_receive(frame, length, bridge);
        }
    }
}

void ble_ecos_bus_disconnect(unsigned epoch)
{
    if (s_operation == NULL) return;
    xSemaphoreTake(s_operation, portMAX_DELAY);
    disconnect_locked(epoch);
    xSemaphoreGive(s_operation);
}

void ble_ecos_bus_request(const cJSON *request, unsigned epoch)
{
    if (s_operation == NULL) return;
    xSemaphoreTake(s_operation, portMAX_DELAY);
    request_locked(request, epoch);
    xSemaphoreGive(s_operation);
}
#else
void ble_ecos_bus_init(ble_ecos_send_t send) { (void)send; }
void ble_ecos_bus_request(const cJSON *request, unsigned epoch) { (void)request; (void)epoch; }
void ble_ecos_bus_disconnect(unsigned epoch) { (void)epoch; }
#endif
