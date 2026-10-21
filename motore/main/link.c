/*
 * The adapter link: ELM327 text over Bluetooth LE or TCP.
 *
 * Bluetooth LE adapters differ only in where their serial port lives
 * (FFF0/FFF1+FFF2, FFE0/FFE1, Vgate's 128-bit service, OBDLink...). After
 * connecting, every characteristic is discovered and the pair is chosen by
 * what it can do: one that notifies (answers in) and one that can be written
 * (commands out), in the same service when possible, known layouts first.
 * Standard services (GAP, GATT, device info, battery) are skipped.
 *
 * Wi-Fi adapters are a TCP socket, usually 192.168.0.10:35000 on the
 * adapter's own network (the device joins it like any known network).
 *
 * Every answer ends with the ELM prompt ">": mt_link_command() collects the
 * text up to it. One command at a time (the OBD task).
 */
#include "motore.h"

#include "services/wifi.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "mt_link";

#define RX_MAX        2048
#define BIT_SYNC      (1 << 0)
#define BIT_CONN      (1 << 1)
#define BIT_FAIL      (1 << 2)
#define BIT_DONE      (1 << 3)   /* a GATT procedure finished */
#define BIT_PROMPT    (1 << 4)   /* ">" received */
#define BIT_GONE      (1 << 5)
#define BIT_SCAN_END  (1 << 6)

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_rx_lock;
static char s_rx[RX_MAX + 1];
static size_t s_rx_len;

static bool s_ble_started;
static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = 23;
static int s_sock = -1;
static uint8_t s_kind = MT_LINK_BLE;

/* Discovery. */
#define SVC_MAX 8
#define CHR_MAX 24
typedef struct { uint16_t start, end; ble_uuid_any_t uuid; } svc_t;
typedef struct { uint16_t def, val, end; uint8_t props; uint8_t svc; ble_uuid_any_t uuid; } chr_t;
static svc_t s_svc[SVC_MAX];
static int s_svcs;
static chr_t s_chr[CHR_MAX];
static int s_chrs;
static int s_disc_svc;
static int s_gatt_status;
static uint16_t s_rx_handle, s_tx_handle, s_cccd;
static bool s_tx_no_rsp;

/* Scan. */
static mt_found_t s_found[MT_SCAN_MAX];
static int s_found_count;
static bool s_scanning;
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------ helpers */

static void rx_append(const char *data, size_t len)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    for (size_t i = 0; i < len; i++)
    {
        char c = data[i];
        if (c == 0)
        {
            continue;   /* some clones pad with zeros */
        }
        if (s_rx_len < RX_MAX)
        {
            s_rx[s_rx_len++] = c;
        }
        if (c == '>')
        {
            xEventGroupSetBits(s_events, BIT_PROMPT);
        }
    }
    s_rx[s_rx_len] = 0;
    xSemaphoreGive(s_rx_lock);
}

static void rx_clear(void)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    s_rx_len = 0;
    s_rx[0] = 0;
    xSemaphoreGive(s_rx_lock);
    xEventGroupClearBits(s_events, BIT_PROMPT);
}

static bool name_looks_obd(const char *name)
{
    static const char *const words[] = {"OBD", "ELM", "VGATE", "ICAR", "VLINK", "V-LINK", "VEEPEAK", "KONNWEI",
                                        "VIECAR", "CARISTA", "LELINK", "BLUEDRIVER", "THINKCAR", "CARLY", "OBDLINK"};
    char up[32];
    size_t n = 0;
    for (; name[n] != 0 && n < sizeof(up) - 1; n++)
    {
        up[n] = (char)toupper((unsigned char)name[n]);
    }
    up[n] = 0;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
    {
        if (strstr(up, words[i]) != NULL)
        {
            return true;
        }
    }
    return false;
}

static bool uuid16_is(const ble_uuid_any_t *u, uint16_t value)
{
    return u->u.type == BLE_UUID_TYPE_16 && u->u16.value == value;
}

static bool standard_service(const ble_uuid_any_t *u)
{
    return uuid16_is(u, 0x1800) || uuid16_is(u, 0x1801) || uuid16_is(u, 0x180A) || uuid16_is(u, 0x180F);
}

/* ------------------------------------------------------------ Bluetooth LE */

static int gap_event(struct ble_gap_event *event, void *arg);

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    xEventGroupSetBits(s_events, BIT_SYNC);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "Bluetooth reset: %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_start(void)
{
    if (s_ble_started)
    {
        return ESP_OK;
    }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK)
    {
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    s_ble_started = true;
    if (!(xEventGroupWaitBits(s_events, BIT_SYNC, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000)) & BIT_SYNC))
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void found(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0)
    {
        return;
    }
    char name[32] = "";
    if (f.name != NULL && f.name_len > 0)
    {
        size_t n = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
        memcpy(name, f.name, n);
        name[n] = 0;
    }
    bool likely = name[0] != 0 && name_looks_obd(name);
    for (int i = 0; i < f.num_uuids16; i++)
    {
        uint16_t u = f.uuids16[i].value;
        likely |= u == 0xFFF0 || u == 0xFFE0 || u == 0x18F0;
    }
    taskENTER_CRITICAL(&s_scan_mux);
    int slot = -1;
    for (int i = 0; i < s_found_count; i++)
    {
        if (memcmp(s_found[i].addr + 1, d->addr.val, 6) == 0)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0 && s_found_count < MT_SCAN_MAX && (name[0] != 0 || likely))
    {
        slot = s_found_count++;
        memset(&s_found[slot], 0, sizeof(s_found[slot]));
        s_found[slot].addr[0] = d->addr.type;
        memcpy(s_found[slot].addr + 1, d->addr.val, 6);
    }
    if (slot >= 0)
    {
        if (name[0] != 0)
        {
            snprintf(s_found[slot].name, sizeof(s_found[slot].name), "%s", name);
        }
        s_found[slot].rssi = d->rssi;
        s_found[slot].likely |= likely;
    }
    taskEXIT_CRITICAL(&s_scan_mux);
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn;
    (void)chr_val;
    (void)arg;
    if (error->status == 0 && dsc != NULL)
    {
        if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(0x2902)) == 0 && s_cccd == 0)
        {
            s_cccd = dsc->handle;
        }
        return 0;
    }
    s_gatt_status = error->status == BLE_HS_EDONE ? 0 : error->status;
    xEventGroupSetBits(s_events, BIT_DONE);
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn;
    (void)arg;
    if (error->status == 0 && chr != NULL)
    {
        if (s_chrs < CHR_MAX)
        {
            chr_t *c = &s_chr[s_chrs++];
            c->def = chr->def_handle;
            c->val = chr->val_handle;
            c->props = chr->properties;
            c->svc = (uint8_t)s_disc_svc;
            c->end = s_svc[s_disc_svc].end;
            c->uuid = chr->uuid;
        }
        return 0;
    }
    s_gatt_status = error->status == BLE_HS_EDONE ? 0 : error->status;
    xEventGroupSetBits(s_events, BIT_DONE);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn;
    (void)arg;
    if (error->status == 0 && svc != NULL)
    {
        if (s_svcs < SVC_MAX && !standard_service(&svc->uuid))
        {
            s_svc[s_svcs].start = svc->start_handle;
            s_svc[s_svcs].end = svc->end_handle;
            s_svc[s_svcs].uuid = svc->uuid;
            s_svcs++;
        }
        return 0;
    }
    s_gatt_status = error->status == BLE_HS_EDONE ? 0 : error->status;
    xEventGroupSetBits(s_events, BIT_DONE);
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    (void)conn;
    (void)arg;
    if (error->status == 0)
    {
        s_mtu = mtu;
    }
    xEventGroupSetBits(s_events, BIT_DONE);
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type)
    {
    case BLE_GAP_EVENT_DISC:
        found(&event->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        xEventGroupSetBits(s_events, BIT_SCAN_END);
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0)
        {
            s_conn = event->connect.conn_handle;
            xEventGroupSetBits(s_events, BIT_CONN);
        }
        else
        {
            xEventGroupSetBits(s_events, BIT_FAIL);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        xEventGroupSetBits(s_events, BIT_GONE | BIT_FAIL);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
    {
        char buf[256];
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        len = len < sizeof(buf) ? len : sizeof(buf);
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, len, &len) == 0)
        {
            rx_append(buf, len);
        }
        break;
    }
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        break;
    default:
        break;
    }
    return 0;
}

static bool wait_done(int ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_DONE | BIT_GONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(ms));
    return (bits & BIT_DONE) && !(bits & BIT_GONE) && s_gatt_status == 0;
}

/* Known serial layouts score higher; then same service, then any pair. */
static int score_rx(const chr_t *c)
{
    if (!(c->props & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)))
    {
        return -1;
    }
    return uuid16_is(&c->uuid, 0xFFF1) || uuid16_is(&c->uuid, 0xFFE1) || c->uuid.u.type == BLE_UUID_TYPE_128 ? 2 : 1;
}

static int score_tx(const chr_t *c)
{
    if (!(c->props & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP)))
    {
        return -1;
    }
    return uuid16_is(&c->uuid, 0xFFF2) || uuid16_is(&c->uuid, 0xFFE1) || c->uuid.u.type == BLE_UUID_TYPE_128 ? 2 : 1;
}

static bool choose_pair(void)
{
    int best = -1, rx = -1, tx = -1;
    for (int i = 0; i < s_chrs; i++)
    {
        int a = score_rx(&s_chr[i]);
        for (int j = 0; a >= 0 && j < s_chrs; j++)
        {
            int b = score_tx(&s_chr[j]);
            if (b < 0)
            {
                continue;
            }
            int score = a + b + (s_chr[i].svc == s_chr[j].svc ? 4 : 0);
            if (score > best)
            {
                best = score;
                rx = i;
                tx = j;
            }
        }
    }
    if (rx < 0)
    {
        return false;
    }
    s_rx_handle = s_chr[rx].val;
    s_tx_handle = s_chr[tx].val;
    s_tx_no_rsp = (s_chr[tx].props & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0;
    /* Descriptors live between the value handle and the next characteristic. */
    uint16_t end = s_chr[rx].end;
    for (int i = 0; i < s_chrs; i++)
    {
        if (s_chr[i].def > s_chr[rx].val && s_chr[i].def - 1 < end)
        {
            end = s_chr[i].def - 1;
        }
    }
    s_chr[rx].end = end;
    s_cccd = 0;
    if (s_chr[rx].val < end)
    {
        ble_gattc_disc_all_dscs(s_conn, s_chr[rx].val, end, on_dsc, NULL);
        wait_done(4000);
    }
    if (s_cccd == 0)
    {
        s_cccd = s_chr[rx].val + 1;   /* the usual place */
    }
    uint8_t on[2] = {(s_chr[rx].props & BLE_GATT_CHR_PROP_NOTIFY) ? 1 : 2, 0};
    return ble_gattc_write_flat(s_conn, s_cccd, on, sizeof(on), NULL, NULL) == 0;
}

static esp_err_t ble_open(const mt_settings_t *s, char *why, size_t size)
{
    bool any = false;
    for (int i = 1; i < 7; i++)
    {
        any |= s->ble_addr[i] != 0;
    }
    if (!any)
    {
        snprintf(why, size, "Scegli l'adattatore");
        return ESP_ERR_NOT_FOUND;   /* no Bluetooth stack until it is needed */
    }
    esp_err_t err = ble_start();
    if (err != ESP_OK)
    {
        snprintf(why, size, "Bluetooth non avviato");
        return err;
    }
    if (ble_gap_disc_active())
    {
        ble_gap_disc_cancel();
    }
    ble_addr_t addr = {.type = s->ble_addr[0]};
    memcpy(addr.val, s->ble_addr + 1, 6);
    xEventGroupClearBits(s_events, BIT_CONN | BIT_FAIL | BIT_GONE | BIT_DONE);
    if (ble_gap_connect(s_own_addr_type, &addr, 10000, NULL, gap_event, NULL) != 0)
    {
        snprintf(why, size, "Connessione Bluetooth rifiutata");
        return ESP_FAIL;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONN | BIT_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(12000));
    if (!(bits & BIT_CONN))
    {
        ble_gap_conn_cancel();
        snprintf(why, size, "%s non risponde: è acceso e vicino?", s->ble_name[0] ? s->ble_name : "L'adattatore");
        return ESP_ERR_TIMEOUT;
    }
    ble_gattc_exchange_mtu(s_conn, on_mtu, NULL);
    wait_done(3000);
    s_svcs = 0;
    s_chrs = 0;
    ble_gattc_disc_all_svcs(s_conn, on_svc, NULL);
    bool ok = wait_done(6000);
    for (int i = 0; ok && i < s_svcs; i++)
    {
        s_disc_svc = i;
        ble_gattc_disc_all_chrs(s_conn, s_svc[i].start, s_svc[i].end, on_chr, NULL);
        ok = wait_done(6000);
    }
    if (!ok || !choose_pair())
    {
        snprintf(why, size, "Servizio seriale non trovato: è un adattatore OBD?");
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "%s: rx %u tx %u%s mtu %u", s->ble_name, s_rx_handle, s_tx_handle, s_tx_no_rsp ? " (no rsp)" : "", s_mtu);
    return ESP_OK;
}

static esp_err_t ble_send(const char *text, size_t len)
{
    size_t chunk = s_mtu > 3 ? s_mtu - 3u : 20u;
    for (size_t sent = 0; sent < len; sent += chunk)
    {
        size_t n = len - sent < chunk ? len - sent : chunk;
        int rc = s_tx_no_rsp ? ble_gattc_write_no_rsp_flat(s_conn, s_tx_handle, text + sent, (uint16_t)n)
                             : ble_gattc_write_flat(s_conn, s_tx_handle, text + sent, (uint16_t)n, NULL, NULL);
        if (rc != 0)
        {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------ Wi-Fi (TCP) */

static void tcp_reader(void *arg)
{
    int sock = (int)(intptr_t)arg;
    char buf[256];
    for (;;)
    {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            break;
        }
        rx_append(buf, (size_t)n);
    }
    xEventGroupSetBits(s_events, BIT_GONE);
    vTaskDelete(NULL);
}

static esp_err_t tcp_open(const mt_settings_t *s, char *why, size_t size)
{
    if (s->host[0] == 0)
    {
        snprintf(why, size, "Indirizzo dell'adattatore Wi-Fi mancante");
        return ESP_ERR_NOT_FOUND;
    }
    if (wifi_acquire(20000) != ESP_OK)
    {
        snprintf(why, size, "Rete dell'adattatore non raggiungibile");
        return ESP_ERR_TIMEOUT;
    }
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%u", s->port ? s->port : 35000);
    if (getaddrinfo(s->host, port, &hints, &res) != 0 || res == NULL)
    {
        wifi_release();
        snprintf(why, size, "Indirizzo %s non valido", s->host);
        return ESP_ERR_INVALID_ARG;
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno == EINPROGRESS)
    {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(sock, &w);
        struct timeval tv = {.tv_sec = 6};
        int error = 0;
        socklen_t len = sizeof(error);
        rc = select(sock + 1, NULL, &w, NULL, &tv) == 1 && getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0 ? 0 : -1;
    }
    if (rc != 0)
    {
        close(sock);
        wifi_release();
        snprintf(why, size, "L'adattatore %s:%s non risponde", s->host, port);
        return ESP_ERR_TIMEOUT;
    }
    fcntl(sock, F_SETFL, flags);
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    s_sock = sock;
    xEventGroupClearBits(s_events, BIT_GONE);
    xTaskCreate(tcp_reader, "mt_tcp", 3072, (void *)(intptr_t)sock, 5, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------ public */

esp_err_t mt_link_open(const mt_settings_t *s, char *why, size_t size)
{
    if (s_events == NULL)
    {
        s_events = xEventGroupCreate();
        s_rx_lock = xSemaphoreCreateMutex();
    }
    mt_link_close();
    rx_clear();
    s_kind = s->link;
    return s->link == MT_LINK_WIFI ? tcp_open(s, why, size) : ble_open(s, why, size);
}

void mt_link_close(void)
{
    if (s_sock >= 0)
    {
        shutdown(s_sock, SHUT_RDWR);
        close(s_sock);
        s_sock = -1;
        wifi_release();
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE)
    {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        xEventGroupWaitBits(s_events, BIT_GONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
        s_conn = BLE_HS_CONN_HANDLE_NONE;
    }
}

bool mt_link_up(void)
{
    if (s_events == NULL || (xEventGroupGetBits(s_events) & BIT_GONE))
    {
        return false;
    }
    return s_kind == MT_LINK_WIFI ? s_sock >= 0 : s_conn != BLE_HS_CONN_HANDLE_NONE;
}

esp_err_t mt_link_command(const char *command, char *out, size_t size, int timeout_ms)
{
    if (!mt_link_up())
    {
        return ESP_ERR_INVALID_STATE;
    }
    rx_clear();
    char line[48];
    int len = snprintf(line, sizeof(line), "%s\r", command);
    esp_err_t err = s_kind == MT_LINK_WIFI ? (send(s_sock, line, (size_t)len, 0) == len ? ESP_OK : ESP_FAIL)
                                           : ble_send(line, (size_t)len);
    if (err != ESP_OK)
    {
        return err;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_PROMPT | BIT_GONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    /* Copy without the prompt, the echo and blank lines; \r becomes \n. */
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    size_t o = 0;
    const char *text = s_rx;
    size_t clen = strlen(command);
    if (strncmp(text, command, clen) == 0)
    {
        text += clen;   /* echo (before ATE0) */
    }
    for (; *text != 0 && *text != '>' && o + 1 < size; text++)
    {
        char c = *text == '\r' ? '\n' : *text;
        if (c == '\n' && (o == 0 || out[o - 1] == '\n'))
        {
            continue;
        }
        out[o++] = c;
    }
    while (o > 0 && (out[o - 1] == '\n' || out[o - 1] == ' '))
    {
        o--;
    }
    out[o] = 0;
    xSemaphoreGive(s_rx_lock);
    if (bits & BIT_GONE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return (bits & BIT_PROMPT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void mt_link_scan(void)
{
    if (s_events == NULL)
    {
        s_events = xEventGroupCreate();
        s_rx_lock = xSemaphoreCreateMutex();
    }
    if (ble_start() != ESP_OK || s_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_disc_active())
    {
        return;
    }
    taskENTER_CRITICAL(&s_scan_mux);
    s_found_count = 0;
    taskEXIT_CRITICAL(&s_scan_mux);
    struct ble_gap_disc_params p = {.passive = 0, .filter_duplicates = 0, .itvl = 0x50, .window = 0x30};
    s_scanning = ble_gap_disc(s_own_addr_type, 8000, &p, gap_event, NULL) == 0;
}

bool mt_link_scanning(void)
{
    return s_scanning;
}

int mt_link_scan_results(mt_found_t *out, int max)
{
    taskENTER_CRITICAL(&s_scan_mux);
    int n = s_found_count < max ? s_found_count : max;
    memcpy(out, s_found, (size_t)n * sizeof(*out));
    taskEXIT_CRITICAL(&s_scan_mux);
    /* Likely adapters first, then by signal. */
    for (int i = 1; i < n; i++)
    {
        mt_found_t key = out[i];
        int j = i - 1;
        while (j >= 0 && (out[j].likely < key.likely || (out[j].likely == key.likely && out[j].rssi < key.rssi)))
        {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}
