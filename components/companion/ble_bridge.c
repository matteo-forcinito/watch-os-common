/*
 * Bluetooth API bridge: the companion uses the same HTTP API as over Wi-Fi,
 * without any network. A request line
 *
 *     GB({"t":"api","id":7,"p":"/giornata/api/tasks","b":{...}});
 *
 * runs on this device's own web service through the loopback interface
 * (web_server_start_local(): the server listens, Wi-Fi stays off) and the
 * answer goes back as base64 chunks small enough for one NUS line each:
 *
 *     {"t":"api","id":7,"s":200,"i":0,"n":3,"d":"..."}
 *
 * Only a bonded, encrypted link may use it (it reaches the whole API, like
 * the paired browser). One request at a time; the worker task and the local
 * server go away a few seconds after the last request. While requests flow
 * the connection interval is shortened, then relaxed again for battery.
 */
#include "ble_bridge.h"

#include "core/sys.h"
#include "services/web_server.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "mbedtls/base64.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ble_api";

#define PATH_MAX_LEN     200
#define BODY_MAX_LEN     1400
#define REPLY_MAX        (96 * 1024)   /* bigger answers need Wi-Fi */
#define CHUNK_RAW        540           /* 720 base64 chars: one NUS line < 1 KB */
#define IDLE_EXIT_US     (8LL * 1000000)

typedef struct
{
    uint32_t id;
    uint16_t conn;
    char path[PATH_MAX_LEN + 1];
    char *body;   /* JSON text or NULL (GET) */
} request_t;

static QueueHandle_t s_queue;
static atomic_bool s_worker;
static bridge_send_fn_t s_send;
static uint16_t s_fast_conn = BLE_HS_CONN_HANDLE_NONE;   /* link currently sped up */

static void reply_error(uint32_t id, int status, const char *message)
{
    size_t length = strlen(message);
    unsigned char encoded[256];
    size_t out = 0;
    mbedtls_base64_encode(encoded, sizeof(encoded), &out, (const unsigned char *)message,
                          length < 180 ? length : 180);
    char line[320];
    snprintf(line, sizeof(line), "{\"t\":\"api\",\"id\":%lu,\"s\":%d,\"i\":0,\"n\":1,\"d\":\"%.*s\"}\n",
             (unsigned long)id, status, (int)out, encoded);
    s_send(line);
}

/* Faster connection events while answers stream; relaxed when idle. */
static void link_speed(uint16_t conn, bool fast)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE || (fast ? s_fast_conn == conn : s_fast_conn != conn))
    {
        return;
    }
    s_fast_conn = fast ? conn : BLE_HS_CONN_HANDLE_NONE;
    /* Relaxed values match the ones set at connection (ble_companion.c). */
    const struct ble_gap_upd_params params = {
        .itvl_min = fast ? 12 : 80,    /* 15 ms : 100 ms */
        .itvl_max = fast ? 24 : 160,   /* 30 ms : 200 ms */
        .latency = fast ? 0 : 4,
        .supervision_timeout = fast ? 400 : 600,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    ble_gap_update_params(conn, &params);
}

static void run(request_t *r)
{
    char url[PATH_MAX_LEN + 24];
    snprintf(url, sizeof(url), "http://127.0.0.1%s", r->path);
    char key[33];
    if (!web_server_local_key(key))
    {
        reply_error(r->id, 503, "Servizio locale non avviato");
        return;
    }
    esp_http_client_config_t config = {
        .url = url,
        .method = r->body != NULL ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        reply_error(r->id, 503, "Memoria insufficiente");
        return;
    }
    esp_http_client_set_header(client, "X-Key", key);
    memset(key, 0, sizeof(key));
    size_t body_len = r->body != NULL ? strlen(r->body) : 0;
    if (r->body != NULL)
    {
        esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    uint8_t *reply = NULL;
    int status = 0, total = 0;
    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err == ESP_OK && body_len > 0 && esp_http_client_write(client, r->body, (int)body_len) != (int)body_len)
    {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) >= 0)
    {
        status = esp_http_client_get_status_code(client);
        reply = heap_caps_malloc(REPLY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        while (reply != NULL && total < REPLY_MAX)
        {
            int n = esp_http_client_read(client, (char *)reply + total, REPLY_MAX - total);
            if (n <= 0)
            {
                break;
            }
            total += n;
        }
        if (total == REPLY_MAX)
        {
            status = 413;   /* too big for Bluetooth: the companion switches to Wi-Fi */
            total = 0;
        }
    }
    else
    {
        err = ESP_FAIL;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || reply == NULL)
    {
        free(reply);
        reply_error(r->id, 502, "Servizio locale non raggiungibile");
        return;
    }

    int chunks = total == 0 ? 1 : (total + CHUNK_RAW - 1) / CHUNK_RAW;
    char *line = malloc(1024);
    for (int i = 0; line != NULL && i < chunks; i++)
    {
        int start = i * CHUNK_RAW;
        int length = total - start < CHUNK_RAW ? total - start : CHUNK_RAW;
        if (length < 0)
        {
            length = 0;
        }
        int head = snprintf(line, 1024, "{\"t\":\"api\",\"id\":%lu,\"s\":%d,\"i\":%d,\"n\":%d,\"d\":\"",
                            (unsigned long)r->id, status, i, chunks);
        size_t written = 0;
        mbedtls_base64_encode((unsigned char *)line + head, 1024 - head - 4, &written, reply + start, (size_t)length);
        snprintf(line + head + written, 1024 - head - written, "\"}\n");
        if (!s_send(line))
        {
            break;   /* disconnected: drop the rest */
        }
    }
    free(line);
    free(reply);
}

static void worker(void *arg)
{
    (void)arg;
    bool local = web_server_start_local() == ESP_OK;
    if (!local)
    {
        ESP_LOGW(TAG, "Local web service not started");
    }
    request_t *r = NULL;
    uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
    while (xQueueReceive(s_queue, &r, pdMS_TO_TICKS(IDLE_EXIT_US / 1000)) == pdTRUE)
    {
        conn = r->conn;
        link_speed(conn, true);
        if (local)
        {
            run(r);
        }
        else
        {
            reply_error(r->id, 503, "Servizio locale non avviato");
        }
        free(r->body);
        free(r);
    }
    link_speed(conn, false);
    if (local)
    {
        web_server_stop();
    }
    atomic_store(&s_worker, false);
    vTaskDelete(NULL);
}

void ble_bridge_init(bridge_send_fn_t send)
{
    s_send = send;
    if (s_queue == NULL)
    {
        s_queue = xQueueCreate(2, sizeof(request_t *));
    }
}

void ble_bridge_request(const cJSON *msg, uint16_t conn)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "p"));
    if (s_queue == NULL || s_send == NULL || !cJSON_IsNumber(id))
    {
        return;
    }
    uint32_t request_id = (uint32_t)id->valuedouble;
    if (path == NULL || path[0] != '/' || strlen(path) > PATH_MAX_LEN || strstr(path, "..") != NULL)
    {
        reply_error(request_id, 400, "Percorso non valido");
        return;
    }
    request_t *r = calloc(1, sizeof(*r));
    if (r == NULL)
    {
        reply_error(request_id, 503, "Memoria insufficiente");
        return;
    }
    r->id = request_id;
    r->conn = conn;
    snprintf(r->path, sizeof(r->path), "%s", path);
    const cJSON *body = cJSON_GetObjectItemCaseSensitive(msg, "b");
    if (body != NULL)
    {
        r->body = cJSON_PrintUnformatted(body);
        if (r->body == NULL || strlen(r->body) > BODY_MAX_LEN)
        {
            free(r->body);
            free(r);
            reply_error(request_id, 413, "Richiesta troppo grande");
            return;
        }
    }
    if (xQueueSend(s_queue, &r, 0) != pdTRUE)
    {
        free(r->body);
        free(r);
        reply_error(request_id, 503, "Occupato, riprova");
        return;
    }
    if (!atomic_exchange(&s_worker, true) &&
        !sys_task_create(worker, "ble_api", 6144, NULL, 3, SYS_CORE_ANY))
    {
        atomic_store(&s_worker, false);
        while (xQueueReceive(s_queue, &r, 0) == pdTRUE)
        {
            reply_error(r->id, 503, "Memoria insufficiente");
            free(r->body);
            free(r);
        }
    }
}
