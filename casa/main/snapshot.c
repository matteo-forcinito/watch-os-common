/*
 * Camera snapshots for Casa.
 *
 *   UI ── casa_snapshot_request(camera.x, w, h) ──► worker task
 *   worker: GET <ha>/api/camera_proxy/camera.x (Bearer token, same instance as
 *           the WebSocket) -> JPEG in PSRAM -> tjpgd -> RGB565 w x h ("cover":
 *           centred crop, every decoded block scaled down on the fly, so no
 *           full-resolution frame is ever held)
 *   UI ◄── casa_snapshot_info() / casa_snapshot_pixels(): the newest frame
 *
 * Two frame buffers: the worker fills the back one and swaps; the UI asks for
 * the next frame only after it shows the current one, so the buffer on screen
 * is never written. One request at a time.
 */
#include "casa.h"

#include "core/sys.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#if LV_USE_TJPGD
#include "src/libs/tjpgd/tjpgd.h"
#endif

static const char *TAG = "casa_snap";

#define SNAP_MAX_BYTES (768 * 1024)
#define SNAP_MAX_SIDE 320
#define SNAP_POOL 8192

typedef struct
{
    char id[CASA_ID];
    int width, height;
} snap_job_t;

static SemaphoreHandle_t s_lock;
static atomic_bool s_busy;
static uint16_t *s_frames[2];
static int s_front; /* index of the frame the UI may show */
static casa_snapshot_info_t s_info;

#if LV_USE_TJPGD
typedef struct
{
    const uint8_t *data;
    size_t size, pos;
    uint16_t *out;
    int ow, oh;             /* output size */
    int cx, cy, cw, ch;     /* crop rectangle in the source */
} decode_t;

static size_t jpeg_input(JDEC *jd, uint8_t *buffer, size_t count)
{
    decode_t *d = jd->device;
    if (d->pos + count > d->size) count = d->size - d->pos;
    if (buffer) memcpy(buffer, d->data + d->pos, count);
    d->pos += count;
    return count;
}

static int jpeg_output(JDEC *jd, void *bitmap, JRECT *rect)
{
    decode_t *d = jd->device;
    const uint8_t *rgb = bitmap;   /* JD_FORMAT 0: RGB888 */
    for (int y = rect->top; y <= rect->bottom; y++)
    {
        int oy = (y - d->cy) * d->oh / d->ch;
        for (int x = rect->left; x <= rect->right; x++, rgb += 3)
        {
            if (oy < 0 || oy >= d->oh || x < d->cx || x >= d->cx + d->cw) continue;
            int ox = (x - d->cx) * d->ow / d->cw;
            d->out[oy * d->ow + ox] = (uint16_t)(((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3));
        }
    }
    return 1;
}

static bool decode(const uint8_t *data, size_t size, uint16_t *out, int ow, int oh, char *error, size_t error_size)
{
    void *pool = heap_caps_malloc(SNAP_POOL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (pool == NULL) pool = heap_caps_malloc(SNAP_POOL, MALLOC_CAP_SPIRAM);
    if (pool == NULL)
    {
        snprintf(error, error_size, "Memoria insufficiente");
        return false;
    }
    JDEC jd;
    decode_t d = {.data = data, .size = size, .out = out, .ow = ow, .oh = oh};
    JRESULT result = jd_prepare(&jd, jpeg_input, pool, SNAP_POOL, &d);
    if (result == JDR_OK)
    {
        int sw = jd.width, sh = jd.height;
        /* Cover: the largest centred source area with the output's ratio. */
        if ((int64_t)sw * oh > (int64_t)sh * ow)
        {
            d.ch = sh;
            d.cw = (int)((int64_t)sh * ow / oh);
        }
        else
        {
            d.cw = sw;
            d.ch = (int)((int64_t)sw * oh / ow);
        }
        d.cx = (sw - d.cw) / 2;
        d.cy = (sh - d.ch) / 2;
        memset(out, 0, (size_t)ow * oh * sizeof(uint16_t));
        result = jd_decomp(&jd, jpeg_output, 0);
    }
    heap_caps_free(pool);
    if (result != JDR_OK) snprintf(error, error_size, "Immagine non leggibile (%d)", (int)result);
    return result == JDR_OK;
}
#endif

static void finish(const snap_job_t *job, bool ok, const char *error, int back)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ok)
    {
        s_front = back;
        s_info.width = job->width;
        s_info.height = job->height;
        s_info.taken = (int64_t)time(NULL);
        snprintf(s_info.id, sizeof(s_info.id), "%s", job->id);
    }
    s_info.ok = ok;
    snprintf(s_info.error, sizeof(s_info.error), "%s", error ? error : "");
    s_info.revision++;
    xSemaphoreGive(s_lock);
    atomic_store(&s_busy, false);
}

static void snap_task(void *arg)
{
    snap_job_t *job = arg;
    char error[48] = "";
    bool ok = false;
    uint8_t *jpeg = NULL;
    size_t length = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int back = 1 - s_front;
    xSemaphoreGive(s_lock);

    char path[CASA_ID + 64];
    snprintf(path, sizeof(path), "/api/camera_proxy/%s?width=%d&height=%d", job->id, job->width * 2, job->height * 2);
    char *url = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    char *token = heap_caps_malloc(320, MALLOC_CAP_SPIRAM);
    if (url == NULL || token == NULL || !casa_api_request(path, url, 512, token, 320))
    {
        snprintf(error, sizeof(error), url && token ? "Casa non configurata" : "Memoria insufficiente");
        goto done;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        snprintf(error, sizeof(error), "Connessione non avviata");
        goto done;
    }
    char header[340];
    snprintf(header, sizeof(header), "Bearer %s", token);
    memset(token, 0, 320);
    esp_http_client_set_header(client, "Authorization", header);
    memset(header, 0, sizeof(header));
    if (esp_http_client_open(client, 0) != ESP_OK)
    {
        snprintf(error, sizeof(error), "Home Assistant non raggiungibile");
    }
    else
    {
        int64_t declared = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200)
            snprintf(error, sizeof(error), status == 401 ? "Token rifiutato" : "Telecamera: errore %d", status);
        else if (declared > SNAP_MAX_BYTES)
            snprintf(error, sizeof(error), "Immagine troppo grande");
        else
        {
            size_t capacity = declared > 0 ? (size_t)declared : 128 * 1024;
            jpeg = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
            while (jpeg != NULL)
            {
                if (length == capacity)
                {
                    if (capacity >= SNAP_MAX_BYTES) { snprintf(error, sizeof(error), "Immagine troppo grande"); break; }
                    size_t larger = capacity * 2 > SNAP_MAX_BYTES ? SNAP_MAX_BYTES : capacity * 2;
                    uint8_t *grown = heap_caps_realloc(jpeg, larger, MALLOC_CAP_SPIRAM);
                    if (grown == NULL) break;
                    jpeg = grown;
                    capacity = larger;
                }
                int got = esp_http_client_read(client, (char *)jpeg + length, (int)(capacity - length));
                if (got <= 0) break;
                length += (size_t)got;
            }
            if (jpeg == NULL) snprintf(error, sizeof(error), "Memoria insufficiente");
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!error[0] && length > 4 && jpeg[0] == 0xFF && jpeg[1] == 0xD8)
    {
#if LV_USE_TJPGD
        int64_t start = esp_timer_get_time();
        ok = decode(jpeg, length, s_frames[back], job->width, job->height, error, sizeof(error));
        ESP_LOGI(TAG, "%s: %u bytes, decoded in %d ms", job->id, (unsigned)length,
                 (int)((esp_timer_get_time() - start) / 1000));
#else
        snprintf(error, sizeof(error), "Decoder JPEG assente");
#endif
    }
    else if (!error[0])
        snprintf(error, sizeof(error), length ? "Non e' un'immagine JPEG" : "Nessuna immagine");

done:
    if (!ok) ESP_LOGW(TAG, "%s: %s", job->id, error);
    heap_caps_free(jpeg);
    heap_caps_free(url);
    heap_caps_free(token);
    finish(job, ok, error, back);
    heap_caps_free(job);
    vTaskDelete(NULL);
}

bool casa_snapshot_request(const char *entity_id, int width, int height)
{
    if (width <= 0 || height <= 0 || width > SNAP_MAX_SIDE || height > SNAP_MAX_SIDE ||
        strncmp(entity_id, "camera.", 7) != 0)
        return false;
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return false;
    }
    for (int i = 0; i < 2; i++)
    {
        if (s_frames[i] == NULL)
            s_frames[i] = heap_caps_malloc(SNAP_MAX_SIDE * SNAP_MAX_SIDE * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (s_frames[i] == NULL) return false;
    }
    if (atomic_exchange(&s_busy, true)) return false;
    snap_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM);
    if (job == NULL)
    {
        atomic_store(&s_busy, false);
        return false;
    }
    snprintf(job->id, sizeof(job->id), "%s", entity_id);
    job->width = width;
    job->height = height;
    if (!sys_task_create(snap_task, "casa_snap", 8192, job, 3, SYS_CORE_ANY))
    {
        heap_caps_free(job);
        atomic_store(&s_busy, false);
        return false;
    }
    return true;
}

bool casa_snapshot_busy(void)
{
    return atomic_load(&s_busy);
}

void casa_snapshot_info(casa_snapshot_info_t *out)
{
    if (s_lock == NULL)
    {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_lock);
}

const uint16_t *casa_snapshot_pixels(void)
{
    if (s_lock == NULL) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint16_t *pixels = s_info.revision ? s_frames[s_front] : NULL;
    xSemaphoreGive(s_lock);
    return pixels;
}
