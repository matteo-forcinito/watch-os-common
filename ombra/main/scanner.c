/*
 * Bluetooth LE observer and device table (see ombra.h).
 *
 * Active scan, duplicates allowed: every advertisement updates RSSI, so the
 * distance moves while you walk. Scan responses add names.
 *
 * "Following you": a tracker separated from its owner (Find My 25-byte frame,
 * SmartTag, Tile, Google unwanted-tracking frame) that keeps showing up with
 * gaps shorter than FOLLOW_GAP_MS accumulates follow time. Trackers rotate their
 * address: when one vanishes and a tracker of the same model appears within
 * LINEAGE_MS, the new address inherits the follow time (same idea as the
 * anti-stalking apps). Above the user threshold, with the shield on, it alerts.
 */
#include "ombra.h"

#include "core/power.h"
#include "core/sys.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ombra";

#define FORGET_MS 90000      /* not heard for 90 s: drop it */
#define FOLLOW_GAP_MS 300000 /* a 5-minute gap restarts the follow time */
#define LINEAGE_MS 20000
#define MINE_MAX 24

typedef struct
{
    ombra_device_t pub;
    int32_t smooth_q4; /* RSSI x 16 */
    uint32_t follow_start;
    int8_t second_best; /* strongest frame of the current second */
    bool used, alerted;
} record_t;

static record_t *s_table; /* PSRAM */
static SemaphoreHandle_t s_lock;
static uint32_t s_next_id = 1, s_revision;
static bool s_shield = true, s_scanning;
static int s_threshold = 15;
static uint32_t s_alert_id;
static uint8_t s_mine[MINE_MAX][6];
static int s_mine_count;
static uint8_t s_own_addr_type;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ------------------------------------------------------------ settings */

static void load_settings(void)
{
    nvs_handle_t nvs;
    if (nvs_open("ombra", NVS_READONLY, &nvs) != ESP_OK)
        return;
    uint8_t value;
    if (nvs_get_u8(nvs, "shield", &value) == ESP_OK)
        s_shield = value != 0;
    if (nvs_get_u8(nvs, "thresh", &value) == ESP_OK && value >= 5 && value <= 60)
        s_threshold = value;
    size_t size = sizeof(s_mine);
    if (nvs_get_blob(nvs, "mine", s_mine, &size) == ESP_OK)
        s_mine_count = (int)(size / 6);
    nvs_close(nvs);
}

/* NVS writes happen on the sys worker, never in the UI or the BLE host task. */
static void save_settings(void *arg)
{
    (void)arg;
    uint8_t mine[MINE_MAX][6];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int count = s_mine_count;
    memcpy(mine, s_mine, sizeof(mine));
    uint8_t shield = s_shield, threshold = (uint8_t)s_threshold;
    xSemaphoreGive(s_lock);
    nvs_handle_t nvs;
    if (nvs_open("ombra", NVS_READWRITE, &nvs) != ESP_OK)
        return;
    nvs_set_u8(nvs, "shield", shield);
    nvs_set_u8(nvs, "thresh", threshold);
    nvs_set_blob(nvs, "mine", mine, (size_t)count * 6);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static bool is_mine(const uint8_t mac[6])
{
    for (int i = 0; i < s_mine_count; i++)
        if (memcmp(s_mine[i], mac, 6) == 0)
            return true;
    return false;
}

/* ------------------------------------------------------------ table */

static int8_t reference_power(const ombra_adv_t *adv)
{
    if (adv->has_tx_power && adv->tx_power < 0 && adv->tx_power > -100)
        return adv->tx_power;
    switch (adv->kind)
    {
    case OMBRA_KIND_TRACKER: return -62; /* small coin-cell tags */
    case OMBRA_KIND_PHONE:
    case OMBRA_KIND_COMPUTER: return -56;
    case OMBRA_KIND_TV:
    case OMBRA_KIND_HOME: return -58;
    default: return -60;
    }
}

static ombra_addr_t address_type(const ble_addr_t *addr)
{
    if (addr->type == BLE_ADDR_PUBLIC || addr->type == BLE_ADDR_PUBLIC_ID)
        return OMBRA_ADDR_PUBLIC;
    switch (addr->val[5] >> 6) /* top bits of the most significant byte */
    {
    case 3: return OMBRA_ADDR_STATIC;
    case 1: return OMBRA_ADDR_PRIVATE;
    default: return OMBRA_ADDR_ANON;
    }
}

static record_t *find_record(const uint8_t mac[6])
{
    for (int i = 0; i < OMBRA_DEVICES; i++)
        if (s_table[i].used && memcmp(s_table[i].pub.mac, mac, 6) == 0)
            return &s_table[i];
    return NULL;
}

static record_t *new_record(void)
{
    record_t *oldest = NULL;
    for (int i = 0; i < OMBRA_DEVICES; i++)
    {
        if (!s_table[i].used)
            return &s_table[i];
        /* Full: replace the faintest device not heard recently (never a suspect). */
        if (!s_table[i].pub.suspect && (!oldest || s_table[i].pub.last_ms < oldest->pub.last_ms))
            oldest = &s_table[i];
    }
    return oldest;
}

/* A rotated tracker address: inherit the history of the one that just vanished. */
static void inherit_lineage(record_t *fresh, uint32_t now)
{
    if (fresh->pub.adv.tracker == OMBRA_TRACKER_NONE)
        return;
    for (int i = 0; i < OMBRA_DEVICES; i++)
    {
        record_t *old = &s_table[i];
        if (!old->used || old == fresh || old->pub.adv.tracker != fresh->pub.adv.tracker)
            continue;
        uint32_t silent = now - old->pub.last_ms;
        if (silent < 3000 || silent > LINEAGE_MS)
            continue;
        fresh->follow_start = old->follow_start;
        fresh->pub.first_ms = old->pub.first_ms;
        fresh->alerted = old->alerted;
        fresh->pub.mine = fresh->pub.mine || old->pub.mine;
        old->used = false;
        ESP_LOGI(TAG, "%s rotated its address, follow time kept", classify_tracker_name(fresh->pub.adv.tracker));
        return;
    }
}

static void update(const struct ble_gap_disc_desc *disc)
{
    if (disc->rssi == 127)
        return;
    ombra_adv_t adv;
    classify_adv(disc->data, disc->length_data, &adv);
    uint32_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    record_t *r = find_record(disc->addr.val);
    if (r == NULL)
    {
        r = new_record();
        if (r == NULL)
        {
            xSemaphoreGive(s_lock);
            return;
        }
        memset(r, 0, sizeof(*r));
        r->used = true;
        r->pub.id = s_next_id++;
        memcpy(r->pub.mac, disc->addr.val, 6);
        r->pub.addr_type = (uint8_t)address_type(&disc->addr);
        r->pub.adv = adv;
        r->pub.first_ms = r->follow_start = now;
        r->pub.last_ms = now;
        r->pub.best = disc->rssi;
        r->smooth_q4 = disc->rssi * 16;
        r->second_best = -127;
        r->pub.mine = is_mine(r->pub.mac);
        memset(r->pub.history, -127, sizeof(r->pub.history));
        inherit_lineage(r, now);
    }
    else if (disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP)
        classify_merge(&r->pub.adv, &adv);
    else
    {
        /* Keep the name learned from a scan response; the newest advertisement wins otherwise. */
        ombra_adv_t merged = adv;
        classify_merge(&merged, &r->pub.adv);
        merged.separated = adv.separated || (adv.tracker == OMBRA_TRACKER_NONE && r->pub.adv.separated);
        r->pub.adv = merged;
    }
    if (now - r->pub.last_ms > FOLLOW_GAP_MS)
        r->follow_start = now;
    r->pub.last_ms = now;
    r->pub.last = disc->rssi;
    r->pub.frames++;
    if (disc->rssi > r->pub.best)
        r->pub.best = disc->rssi;
    if (disc->rssi > r->second_best)
        r->second_best = disc->rssi;
    /* Exponential smoothing (1/4): fast enough to walk with, steady enough to read. */
    r->smooth_q4 += (disc->rssi * 16 - r->smooth_q4) / 4;
    r->pub.rssi = (int8_t)(r->smooth_q4 / 16);
    r->pub.reference = reference_power(&r->pub.adv);
    r->pub.distance_cm = classify_distance_cm(r->pub.rssi, r->pub.reference);
    r->pub.follow_ms = r->pub.last_ms - r->follow_start;
    xSemaphoreGive(s_lock);
}

/* Once a second: history, expiry, suspects and alerts. */
static void maintenance_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t now = now_ms(), alert = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < OMBRA_DEVICES; i++)
        {
            record_t *r = &s_table[i];
            if (!r->used)
                continue;
            if (now - r->pub.last_ms > FORGET_MS && !r->pub.suspect)
            {
                r->used = false;
                continue;
            }
            memmove(r->pub.history, r->pub.history + 1, OMBRA_HISTORY - 1);
            r->pub.history[OMBRA_HISTORY - 1] = r->second_best;
            r->second_best = -127;
            bool separated_tracker = r->pub.adv.kind == OMBRA_KIND_TRACKER && r->pub.adv.separated;
            bool suspect = separated_tracker && !r->pub.mine && now - r->pub.last_ms < FOLLOW_GAP_MS &&
                           r->pub.follow_ms >= (uint32_t)s_threshold * 60000u;
            if (suspect && !r->alerted && s_shield)
            {
                r->alerted = true;
                alert = r->pub.id;
            }
            if (!suspect && now - r->pub.last_ms > FORGET_MS)
                r->used = false; /* a suspect that left long ago */
            r->pub.suspect = suspect;
        }
        if (alert)
            s_alert_id = alert;
        s_revision++;
        xSemaphoreGive(s_lock);
        if (alert)
        {
            ESP_LOGW(TAG, "Tracker following you (id %lu)", (unsigned long)alert);
            power_wake(POWER_WAKE_NOTIFICATION);
            ombra_sound_alert();
        }
    }
}

/* ------------------------------------------------------------ NimBLE */

static int gap_event(struct ble_gap_event *event, void *arg);

static void start_scan(void)
{
    struct ble_gap_disc_params params = {
        .itvl = 0x00A0,   /* 100 ms */
        .window = 0x0090, /* 90 ms: nothing else uses the radio */
        .passive = 0,     /* scan requests bring the names */
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    s_scanning = rc == 0;
    if (rc != 0)
        ESP_LOGE(TAG, "Scan start failed: %d", rc);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC)
        update(&event->disc);
    else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE)
        start_scan();
    return 0;
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(1) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0)
    {
        ESP_LOGE(TAG, "No usable Bluetooth address");
        return;
    }
    start_scan();
    ESP_LOGI(TAG, "Scanning");
}

static void on_reset(int reason)
{
    s_scanning = false;
    ESP_LOGW(TAG, "Bluetooth reset (%d)", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ombra_start(void)
{
    s_table = heap_caps_calloc(OMBRA_DEVICES, sizeof(record_t), MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    if (s_table == NULL || s_lock == NULL)
        return ESP_ERR_NO_MEM;
    load_settings();
    esp_err_t error = nimble_port_init();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "Bluetooth start: %s", esp_err_to_name(error));
        return error;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    if (!sys_task_create(maintenance_task, "ombra_tick", 3072, NULL, 3, tskNO_AFFINITY))
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

/* ------------------------------------------------------------ queries */

void ombra_status(ombra_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < OMBRA_DEVICES; i++)
    {
        const record_t *r = &s_table[i];
        if (!r->used || now_ms() - r->pub.last_ms > 30000)
            continue;
        out->total++;
        out->kinds[r->pub.adv.kind < OMBRA_KIND_COUNT ? r->pub.adv.kind : 0]++;
        out->trackers += r->pub.adv.kind == OMBRA_KIND_TRACKER;
        out->suspects += r->pub.suspect;
        out->mine += r->pub.mine;
    }
    out->scanning = s_scanning;
    out->shield = s_shield;
    out->threshold_min = s_threshold;
    out->revision = s_revision;
    out->alert_id = s_alert_id;
    out->uptime_ms = now_ms();
    xSemaphoreGive(s_lock);
}

static int nearest_first(const void *a, const void *b)
{
    const ombra_device_t *x = a, *y = b;
    if (x->suspect != y->suspect)
        return x->suspect ? -1 : 1;
    return (int)y->rssi - (int)x->rssi;
}

int ombra_snapshot(ombra_device_t *out, int max, int filter)
{
    if (s_lock == NULL)
        return 0;
    int count = 0;
    uint32_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < OMBRA_DEVICES && count < max; i++)
    {
        const record_t *r = &s_table[i];
        if (!r->used || (now - r->pub.last_ms > 30000 && !r->pub.suspect))
            continue;
        if (filter >= 0 && r->pub.adv.kind != filter)
            continue;
        out[count++] = r->pub;
    }
    xSemaphoreGive(s_lock);
    qsort(out, (size_t)count, sizeof(*out), nearest_first);
    return count;
}

bool ombra_get(uint32_t id, ombra_device_t *out)
{
    bool found = false;
    if (s_lock == NULL || id == 0)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < OMBRA_DEVICES && !found; i++)
        if (s_table[i].used && s_table[i].pub.id == id)
        {
            *out = s_table[i].pub;
            found = true;
        }
    xSemaphoreGive(s_lock);
    return found;
}

void ombra_set_mine(uint32_t id, bool mine)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < OMBRA_DEVICES; i++)
    {
        record_t *r = &s_table[i];
        if (!r->used || r->pub.id != id)
            continue;
        r->pub.mine = mine;
        r->pub.suspect = r->pub.suspect && !mine;
        int at = -1;
        for (int k = 0; k < s_mine_count; k++)
            if (memcmp(s_mine[k], r->pub.mac, 6) == 0)
                at = k;
        if (mine && at < 0)
        {
            if (s_mine_count == MINE_MAX)
            {
                memmove(s_mine[0], s_mine[1], (MINE_MAX - 1) * 6); /* forget the oldest */
                s_mine_count--;
            }
            memcpy(s_mine[s_mine_count++], r->pub.mac, 6);
        }
        else if (!mine && at >= 0)
        {
            memmove(s_mine[at], s_mine[at + 1], (size_t)(s_mine_count - at - 1) * 6);
            s_mine_count--;
        }
    }
    s_revision++;
    xSemaphoreGive(s_lock);
    sys_post(save_settings, NULL);
}

void ombra_set_shield(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_shield = on;
    s_revision++;
    xSemaphoreGive(s_lock);
    sys_post(save_settings, NULL);
}

void ombra_set_threshold(int minutes)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_threshold = minutes < 5 ? 5 : minutes > 60 ? 60 : minutes;
    for (int i = 0; i < OMBRA_DEVICES; i++)
        s_table[i].alerted = false;
    s_revision++;
    xSemaphoreGive(s_lock);
    sys_post(save_settings, NULL);
}

const char *ombra_addr_text(ombra_addr_t type)
{
    switch (type)
    {
    case OMBRA_ADDR_PUBLIC: return "pubblico (fisso)";
    case OMBRA_ADDR_STATIC: return "casuale statico";
    case OMBRA_ADDR_PRIVATE: return "privato, cambia nel tempo";
    default: return "anonimo";
    }
}

void ombra_display_name(const ombra_device_t *d, char *out, size_t size)
{
    const char *company = classify_company(d->adv.company);
    if (d->adv.tracker != OMBRA_TRACKER_NONE)
        snprintf(out, size, "%s", classify_tracker_name(d->adv.tracker));
    else if (d->adv.name[0])
        snprintf(out, size, "%s", d->adv.name);
    else if (d->adv.detail[0])
        snprintf(out, size, "%s", d->adv.detail);
    else if (company)
        snprintf(out, size, "%s %s", classify_kind_name(d->adv.kind), company);
    else
        snprintf(out, size, "%s", classify_kind_name(d->adv.kind));
}
