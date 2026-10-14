/*
 * Copertura measurements (see copertura.h).
 *
 * The probe task holds Wi-Fi for the whole life of the app (power save off:
 * a sleeping radio would add its own latency to every ping) and samples the
 * signal every 500 ms. A ping session to the router answers "how fast and
 * how reliable", which the signal alone cannot tell.
 */
#include "copertura.h"

#include "core/sys.h"
#include "services/wifi.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "copertura";

#define PINGS 20

static SemaphoreHandle_t s_lock;
EXT_RAM_BSS_ATTR static cp_live_t s_live;
EXT_RAM_BSS_ATTR static int8_t s_ring_rssi[CP_LIVE];
EXT_RAM_BSS_ATTR static int16_t s_ring_ping[CP_LIVE];
static int s_ring_next, s_ring_count;
EXT_RAM_BSS_ATTR static int16_t s_pings[PINGS];        /* last results, -1 = lost */
static int s_ping_next, s_ping_count;
static int16_t s_last_ping = -1;
static esp_ping_handle_t s_ping;
static uint32_t s_ping_gateway;
static float s_ema;

EXT_RAM_BSS_ATTR static cp_measure_t s_measure;
static int64_t s_measure_end;
static int s_m_samples, s_m_sum, s_m_min, s_m_max;
static int s_m_sent, s_m_answered, s_m_ping_sum, s_m_ping_max;
static uint8_t s_m_bssid[6], s_m_channel;
static uint8_t s_m_floor;

EXT_RAM_BSS_ATTR static cp_finder_t s_finder;
static bool s_scan_wanted;
static volatile bool s_quit;            /* cp_stop(): the probe task ends */
static volatile bool s_probe_running;
EXT_RAM_BSS_ATTR static cp_ap_t s_aps[CP_MAX_APS];
static int s_ap_count;
static int64_t s_ap_time;

void cp_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void cp_unlock(void) { xSemaphoreGive(s_lock); }

void cp_live(cp_live_t *out)
{
    cp_lock();
    *out = s_live;
    cp_unlock();
}

int cp_history(int8_t *rssi, int16_t *ping, int max)
{
    cp_lock();
    int n = s_ring_count < max ? s_ring_count : max;
    for (int i = 0; i < n; i++)
    {
        int index = (s_ring_next - n + i + CP_LIVE) % CP_LIVE;
        rssi[i] = s_ring_rssi[index];
        ping[i] = s_ring_ping[index];
    }
    cp_unlock();
    return n;
}

void cp_measure_status(cp_measure_t *out)
{
    cp_lock();
    *out = s_measure;
    cp_unlock();
}

const cp_ap_t *cp_aps(int *count, int64_t *when)
{
    *count = s_ap_count;
    if (when != NULL)
    {
        *when = s_ap_time;
    }
    return s_aps;
}

void cp_scan_now(void)
{
    cp_lock();
    s_scan_wanted = true;
    cp_unlock();
}

bool cp_measure(const char *room, uint8_t floor)
{
    cp_lock();
    bool ok = s_measure.phase == CP_IDLE && s_live.connected;
    if (ok)
    {
        s_measure.phase = CP_MEASURING;
        s_measure.percent = 0;
        snprintf(s_measure.room, sizeof(s_measure.room), "%s", room);
        s_measure_end = esp_timer_get_time() + CP_MEASURE_S * 1000000LL;
        s_m_samples = s_m_sum = s_m_sent = s_m_answered = s_m_ping_sum = s_m_ping_max = 0;
        s_m_min = 0;
        s_m_max = -127;
        s_m_floor = floor;
        memcpy(s_m_bssid, s_live.bssid, 6);
        s_m_channel = s_live.channel;
        cp_changed(false);
    }
    cp_unlock();
    return ok;
}

/* ---------------------------------------------------------------- finder */

void cp_finder_start(int mode)
{
    cp_lock();
    memset(&s_finder, 0, sizeof(s_finder));
    s_finder.on = true;
    s_finder.mode = mode;
    s_finder.best = s_live.rssi != 0 ? s_live.rssi : -100;
    cp_unlock();
}

void cp_finder_stop(void)
{
    cp_lock();
    s_finder.on = false;
    cp_unlock();
    cp_sound_heat(-1, false);
}

void cp_finder_state(cp_finder_t *out)
{
    cp_lock();
    *out = s_finder;
    cp_unlock();
}

/* ------------------------------------------------------------------ ping */

static void ping_result(int16_t ms)
{
    cp_lock();
    s_last_ping = ms;
    s_pings[s_ping_next] = ms;
    s_ping_next = (s_ping_next + 1) % PINGS;
    if (s_ping_count < PINGS)
    {
        s_ping_count++;
    }
    int lost = 0, sum = 0, answered = 0;
    for (int i = 0; i < s_ping_count; i++)
    {
        if (s_pings[i] < 0)
        {
            lost++;
        }
        else
        {
            sum += s_pings[i];
            answered++;
        }
    }
    s_live.ping = ms;
    s_live.loss = (uint8_t)(lost * 100 / s_ping_count);
    s_live.ping_avg = (uint16_t)(answered ? sum / answered : 0);
    if (s_measure.phase == CP_MEASURING)
    {
        s_m_sent++;
        if (ms >= 0)
        {
            s_m_answered++;
            s_m_ping_sum += ms;
            s_m_ping_max = ms > s_m_ping_max ? ms : s_m_ping_max;
        }
    }
    cp_unlock();
}

static void on_ping_ok(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint32_t elapsed = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    ping_result((int16_t)(elapsed > 30000 ? 30000 : elapsed));
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    (void)args;
    ping_result(-1);
}

static void ping_start(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif == NULL || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.gw.addr == 0)
    {
        return;
    }
    if (s_ping != NULL && s_ping_gateway == info.gw.addr)
    {
        return;
    }
    if (s_ping != NULL)
    {
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = NULL;
    }
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr.type = IPADDR_TYPE_V4;
    config.target_addr.u_addr.ip4.addr = info.gw.addr;
    config.count = ESP_PING_COUNT_INFINITE;
    config.interval_ms = 1000;
    config.timeout_ms = 900;
    config.data_size = 32;
    config.task_stack_size = 3072;
    esp_ping_callbacks_t callbacks = {.on_ping_success = on_ping_ok, .on_ping_timeout = on_ping_timeout};
    if (esp_ping_new_session(&config, &callbacks, &s_ping) == ESP_OK)
    {
        esp_ping_start(s_ping);
        s_ping_gateway = info.gw.addr;
        cp_lock();
        snprintf(s_live.gateway, sizeof(s_live.gateway), IPSTR, IP2STR(&info.gw));
        cp_unlock();
    }
}

/* ------------------------------------------------------------------ scan */

/* Scan the air: our network's radios and the neighbours. Fills the point if given. */
static void scan(cp_point_t *point)
{
    wifi_ap_detail_t *found = calloc(CP_MAX_APS, sizeof(wifi_ap_detail_t));
    if (found == NULL)
    {
        return;
    }
    int n = wifi_scan_detail(found, CP_MAX_APS);
    cp_lock();
    s_ap_count = 0;
    int radios = 0;
    int8_t alt = 0;
    for (int i = 0; i < n && s_ap_count < CP_MAX_APS; i++)
    {
        cp_ap_t *a = &s_aps[s_ap_count++];
        snprintf(a->ssid, sizeof(a->ssid), "%s", found[i].ssid);
        memcpy(a->bssid, found[i].bssid, 6);
        a->channel = found[i].channel;
        a->rssi = found[i].rssi;
        a->auth = found[i].auth;
        a->ours = s_live.ssid[0] != '\0' && strcmp(a->ssid, s_live.ssid) == 0;
        if (a->ours)
        {
            radios++;
            if (point != NULL && memcmp(a->bssid, point->bssid, 6) != 0 && (alt == 0 || a->rssi > alt))
            {
                alt = a->rssi;
            }
        }
    }
    /* A weak router can be missed by the scan while we are connected to it: it is ours anyway. */
    if (radios == 0 && s_live.connected && s_ap_count < CP_MAX_APS)
    {
        cp_ap_t *a = &s_aps[s_ap_count++];
        snprintf(a->ssid, sizeof(a->ssid), "%s", s_live.ssid);
        memcpy(a->bssid, s_live.bssid, 6);
        a->channel = s_live.channel;
        a->rssi = s_live.rssi;
        a->auth = 0;
        a->ours = true;
        radios = 1;
    }
    s_ap_time = time(NULL);
    if (point != NULL)
    {
        point->radios = (uint8_t)radios;
        point->alt_rssi = alt;
    }
    cp_changed(false);
    cp_unlock();
    free(found);
    ESP_LOGI(TAG, "Scan: %d radios, %d of ours", n, radios);
}

/* ------------------------------------------------------------------ task */

static void sample(void)
{
    wifi_ap_record_t ap;
    bool connected = wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    cp_lock();
    s_live.connected = connected;
    if (connected)
    {
        s_live.rssi_raw = ap.rssi;
        s_ema = s_ema == 0 ? ap.rssi : s_ema * 0.65f + ap.rssi * 0.35f;
        s_live.rssi = (int8_t)(s_ema - 0.5f);
        s_live.channel = ap.primary;
        memcpy(s_live.bssid, ap.bssid, 6);
        snprintf(s_live.ssid, sizeof(s_live.ssid), "%s", (const char *)ap.ssid);
        snprintf(s_live.phy, sizeof(s_live.phy), "%s", ap.phy_11ax ? "Wi-Fi 6" : ap.phy_11n ? "Wi-Fi 4" :
                 ap.phy_11g ? "11g" : "11b");
        s_ring_rssi[s_ring_next] = ap.rssi;
        s_ring_ping[s_ring_next] = s_last_ping;
        s_ring_next = (s_ring_next + 1) % CP_LIVE;
        if (s_ring_count < CP_LIVE)
        {
            s_ring_count++;
        }
        if (s_measure.phase == CP_MEASURING)
        {
            s_m_samples++;
            s_m_sum += ap.rssi;
            s_m_min = s_m_min == 0 || ap.rssi < s_m_min ? ap.rssi : s_m_min;
            s_m_max = ap.rssi > s_m_max ? ap.rssi : s_m_max;
            int64_t left = s_measure_end - esp_timer_get_time();
            s_measure.percent = 100 - (int)(left / (CP_MEASURE_S * 10000LL));
            s_measure.percent = s_measure.percent > 99 ? 99 : s_measure.percent < 0 ? 0 : s_measure.percent;
        }
        if (s_finder.on)
        {
            int rssi = s_live.rssi;
            s_finder.rssi = rssi;
            if (rssi > s_finder.best)
            {
                s_finder.best = rssi;
            }
            s_finder.heat = cp_finder_heat(s_finder.mode, rssi, s_finder.best);
            /* Trend over the last ~2 s of raw samples. */
            int now = 0, before = 0, n = 0;
            for (int i = 0; i < 4 && i < s_ring_count; i++)
            {
                now += s_ring_rssi[(s_ring_next - 1 - i + CP_LIVE) % CP_LIVE];
                before += s_ring_rssi[(s_ring_next - 5 - i + CP_LIVE) % CP_LIVE];
                n++;
            }
            int delta = n ? (now - before) / n : 0;
            s_finder.trend = delta >= 2 ? 1 : delta <= -2 ? -1 : 0;
        }
    }
    bool finder = s_finder.on;
    int heat = s_finder.heat;
    bool target = s_finder.mode == 1 && s_finder.heat >= 85;
    cp_unlock();
    if (finder)
    {
        cp_sound_heat(connected ? heat : -1, target);
    }
}

static void finish_measure(void)
{
    cp_point_t point;
    memset(&point, 0, sizeof(point));
    cp_lock();
    snprintf(point.room, sizeof(point.room), "%s", s_measure.room);
    point.floor = s_m_floor;
    point.x = -1;
    point.y = -1;
    point.rssi = (int8_t)(s_m_samples ? s_m_sum / s_m_samples : s_live.rssi);
    point.rssi_min = (int8_t)s_m_min;
    point.rssi_max = (int8_t)s_m_max;
    point.channel = s_m_channel;
    memcpy(point.bssid, s_m_bssid, 6);
    point.ping = (uint16_t)(s_m_answered ? s_m_ping_sum / s_m_answered : 0);
    point.ping_max = (uint16_t)s_m_ping_max;
    point.loss = (uint8_t)(s_m_sent ? (s_m_sent - s_m_answered) * 100 / s_m_sent : 0);
    point.when = time(NULL);
    s_measure.phase = CP_SCANNING;
    s_measure.percent = 99;
    cp_changed(false);
    cp_unlock();

    scan(&point);
    uint32_t id = cp_point_add(&point);

    cp_lock();
    s_measure.phase = CP_IDLE;
    s_measure.percent = 100;
    s_measure.last_point = id;
    cp_changed(false);
    cp_unlock();
    ESP_LOGI(TAG, "%s: %d dBm, %u ms, %u%% lost, %u radios", point.room, point.rssi, point.ping, point.loss, point.radios);
}

static void probe_task(void *arg)
{
    (void)arg;
    bool held = false;
    int64_t last_scan = 0;
    for (;;)
    {
        if (s_quit)
        {
            if (s_ping != NULL)
            {
                esp_ping_stop(s_ping);
                esp_ping_delete_session(s_ping);
                s_ping = NULL;
            }
            if (held)
            {
                wifi_set_fast(false);
                wifi_release();
            }
            cp_store_flush();
            s_probe_running = false;
            vTaskDelete(NULL);
            return;
        }
        if (!held)
        {
            held = wifi_acquire(20000) == ESP_OK;
            if (held)
            {
                wifi_set_fast(true);   /* no modem sleep: honest latency */
            }
        }
        if (held && wifi_is_connected())
        {
            ping_start();
        }
        sample();

        cp_lock();
        bool measure_done = s_measure.phase == CP_MEASURING && esp_timer_get_time() >= s_measure_end;
        bool scan_wanted = s_scan_wanted && s_measure.phase == CP_IDLE;
        s_scan_wanted = false;
        cp_unlock();
        if (measure_done)
        {
            finish_measure();
        }
        else if (scan_wanted || (held && last_scan == 0 && wifi_is_connected()))
        {
            scan(NULL);
            last_scan = esp_timer_get_time();
        }
        cp_store_flush();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t cp_start(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
        cp_store_load();   /* once: the points stay in PSRAM between opens (internal app) */
    }
    s_quit = false;
    cp_sound_init();
    if (s_probe_running)
    {
        return ESP_OK;   /* still ending after a cp_stop(): it goes on */
    }
    s_probe_running = true;
    if (!sys_task_create(probe_task, "cp_probe", 6144, NULL, 4, SYS_CORE_ANY))
    {
        s_probe_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void cp_stop(void)
{
    s_quit = true;   /* the probe task releases Wi-Fi within 0.5 s */
    cp_sound_stop();
}
