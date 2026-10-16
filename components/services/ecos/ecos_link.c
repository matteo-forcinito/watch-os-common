#include "core/recovery.h"
/* Famiglia: the paired devices talk to each other over ESP-NOW.
 *
 *   ecos_family_hold() / _touch()      the UI or the dashboard looks
 *   power cable + screen off           this device answers the others
 *          │
 *          ▼
 *   link task (only while needed) ── owns the radio, ESP-NOW, the keys
 *          │   search: STATUS_REQ to every paired device on every channel,
 *          │   the MAC-level ACK says "it is here", it answers with STATUS
 *          ▼
 *   s_info (PSRAM): last known info of each device, read by ecos_family()
 *
 * Every frame is sealed with AES-128-GCM. Each direction of a pair has its
 * own key, derived from the pairing credential: HMAC(credential, "ELNK" ||
 * sender MAC). Durable boot epochs and sequences survive task restarts;
 * commands are checkpointed before acting, including across receiver reboots.
 *
 * Idle cost: one esp_timer (15 s, checks two numbers) and a few hundred bytes
 * of PSRAM. No task, no radio. */
#include "services/ecos.h"
#include "ecos_internal.h"
#include "ecos_link_wire.h"
#include "services/notify.h"
#include "services/wifi.h"
#include "core/power.h"
#include "core/state.h"
#include "core/sys.h"
#include "hardware/board.h"
#include "hardware/pmu.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/platform_util.h"
#include "psa/crypto.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LINK_TX_POWER 44                     /* 11 dBm (0.25 dBm units): no saturation side by side */
#define WEB_HOLD_US (90LL * 1000000)
#define IDLE_STOP_US (3LL * 1000000)
#define AUTO_CHECK_US (15LL * 1000000)
#define ACK_WAIT_MS 60
#define REPLY_WAIT_MS 150
#define CLOCK_VALID 1700000000
#define CLOCK_WINDOW_S 120

static const char *TAG = "ecos_link";
static const uint8_t s_hops[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};

/* ---- shared state (s_lock) */

typedef struct
{
    uint8_t mac[6];
    bool used, known, reachable;
    int64_t seen_at;
    int rssi;
    uint8_t channel;
    ecos_link_status_t status;
    uint8_t fingerprint[16];
    uint64_t last_boot;
    uint32_t last_seq;
} member_info_t;

typedef enum
{
    CMD_NONE,
    CMD_RING,
    CMD_STOP,
    CMD_MESSAGE,
} cmd_kind_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR member_info_t s_info[ECOS_PEERS_MAX + 2];
static EXT_RAM_BSS_ATTR char s_message[96];
static EXT_RAM_BSS_ATTR char s_cmd_text[ECOS_LINK_TEXT_MAX];
static uint8_t s_cmd_mac[6];
static cmd_kind_t s_cmd;
static bool s_refresh, s_running, s_suspend, s_reload, s_busy, s_listening, s_ringing_here;
static int s_holds;
static int64_t s_web_until;
static void (*s_handler)(const ecos_event_t *event);
static esp_timer_handle_t s_auto_timer;
static uint64_t s_boot_epoch;
static uint32_t s_tx_sequence;

/* ---- radio queues (only while the task runs) */

typedef struct
{
    uint8_t mac[6];
    uint8_t length;
    int8_t rssi;
    uint8_t data[ECOS_LINK_FRAME_MAX];
} link_frame_t;

typedef struct
{
    uint8_t mac[6];
    bool ok;
} link_sent_t;

static QueueHandle_t s_rx, s_sent;

static void on_radio(const esp_now_recv_info_t *info, const uint8_t *data, int length)
{
    QueueHandle_t rx = s_rx;
    if (rx == NULL || info == NULL || length <= 0 || length > ECOS_LINK_FRAME_MAX)
    {
        return;
    }
    link_frame_t frame;
    memcpy(frame.mac, info->src_addr, 6);
    frame.length = (uint8_t)length;
    frame.rssi = info->rx_ctrl != NULL ? (int8_t)info->rx_ctrl->rssi : 0;
    memcpy(frame.data, data, (size_t)length);
    xQueueSend(rx, &frame, 0);
}

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    QueueHandle_t sent = s_sent;
    if (sent == NULL || info == NULL || info->des_addr == NULL)
    {
        return;
    }
    link_sent_t result = {.ok = status == ESP_NOW_SEND_SUCCESS};
    memcpy(result.mac, info->des_addr, 6);
    xQueueSend(sent, &result, 0);
}

/* ---- the task's private session (PSRAM) */

typedef struct
{
    uint8_t mac[6];
    char name[ECOS_NAME_SIZE];
    psa_key_id_t send_key, recv_key;
    uint8_t fingerprint[16];
    uint64_t last_boot;
    uint32_t last_seq;
} link_peer_t;

typedef struct
{
    uint8_t mac[6];
    uint64_t boot;
    int count;
    link_peer_t peers[ECOS_PEERS_MAX];
    uint8_t store[ECOS_STORE_SIZE];
} session_t;

static void set_message(const char *format, const char *name)
{
    char text[sizeof(s_message)];
    snprintf(text, sizeof(text), format, name != NULL ? name : "");
    portENTER_CRITICAL(&s_lock);
    memcpy(s_message, text, sizeof(text));
    portEXIT_CRITICAL(&s_lock);
}

static bool powered(void)
{
    return !pmu_present() || state_get(STATE_USB) != 0;
}

/* Answer the others: on the cable, with nobody using the screen. */
static bool auto_wanted(void)
{
    return recovery_active() || (powered() && state_get(STATE_SCREEN) >= POWER_SCREEN_AOD);
}

static bool wanted(void)
{
    portENTER_CRITICAL(&s_lock);
    bool want = s_holds > 0 || esp_timer_get_time() < s_web_until || s_refresh || s_cmd != CMD_NONE;
    portEXIT_CRITICAL(&s_lock);
    return want || auto_wanted();
}

static member_info_t *info_slot(const uint8_t mac[6])
{
    member_info_t *oldest = NULL;
    for (size_t i = 0; i < sizeof(s_info) / sizeof(s_info[0]); i++)
    {
        if (s_info[i].used && memcmp(s_info[i].mac, mac, 6) == 0)
        {
            return &s_info[i];
        }
        if (oldest == NULL || !s_info[i].used || (oldest->used && s_info[i].seen_at < oldest->seen_at))
        {
            oldest = &s_info[i];
        }
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->used = true;
    memcpy(oldest->mac, mac, 6);
    return oldest;
}

static bool hmac16(const uint8_t key[32], const uint8_t *message, size_t length, uint8_t out[16])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id = 0;
    uint8_t full[32] = {0};
    size_t written = 0;
    bool ok = psa_import_key(&attr, key, 32, &id) == PSA_SUCCESS &&
              psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), message, length, full, sizeof(full), &written) ==
                  PSA_SUCCESS &&
              written == 32;
    if (id != 0)
    {
        psa_destroy_key(id);
    }
    psa_reset_key_attributes(&attr);
    memcpy(out, full, 16);
    mbedtls_platform_zeroize(full, sizeof(full));
    return ok;
}

static bool aes_key(const uint8_t raw[16], psa_key_usage_t usage, psa_key_id_t *out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, usage);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    bool ok = psa_import_key(&attr, raw, 16, out) == PSA_SUCCESS;
    psa_reset_key_attributes(&attr);
    return ok;
}

/* Direction key: HMAC(credential, "ELNK" || sender MAC). */
static bool direction_key(const uint8_t credential[32], const uint8_t sender[6], psa_key_usage_t usage,
                          psa_key_id_t *out)
{
    uint8_t input[10], raw[16];
    memcpy(input, "ELNK", 4);
    memcpy(input + 4, sender, 6);
    bool ok = hmac16(credential, input, sizeof(input), raw) && aes_key(raw, usage, out);
    mbedtls_platform_zeroize(raw, sizeof(raw));
    return ok;
}

/* One durable epoch per boot, not per radio session; no writes for telemetry.
 * Fail closed if NVS cannot reserve it. Resetting ECOS credentials also removes
 * their keys, so a new epoch must never be reused with a retained credential. */
static bool reserve_epoch(void)
{
    if (s_boot_epoch != 0)
    {
        return true;
    }
    nvs_handle_t h;
    if (nvs_open("ecos", NVS_READWRITE, &h) != ESP_OK)
    {
        return false;
    }
    uint64_t epoch = 0;
    esp_err_t err = nvs_get_u64(h, "link_epoch", &epoch);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        err = ESP_OK;
    }
    if (err == ESP_OK && epoch != UINT64_MAX)
    {
        err = nvs_set_u64(h, "link_epoch", epoch + 1);
        if (err == ESP_OK)
        {
            err = nvs_commit(h);
        }
        if (err == ESP_OK)
        {
            s_boot_epoch = epoch + 1;
        }
    }
    nvs_close(h);
    return s_boot_epoch != 0;
}

/* Commands are checkpointed before acting, so a reboot cannot replay a ring
 * or a notification. Fingerprint binds the checkpoint to this pairing key. */
static bool replay_record(link_peer_t *peer, bool write, uint64_t boot, uint32_t seq)
{
    char key[14];
    snprintf(key, sizeof(key), "r%02x%02x%02x%02x%02x%02x", peer->mac[0], peer->mac[1], peer->mac[2],
             peer->mac[3], peer->mac[4], peer->mac[5]);
    nvs_handle_t h;
    esp_err_t err = nvs_open("ecos", write ? NVS_READWRITE : NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        return false;
    }
    uint8_t record[28];
    if (write)
    {
        memcpy(record, peer->fingerprint, 16);
        ecos_link_put64(record + 16, boot);
        ecos_link_put32(record + 24, seq);
        err = nvs_set_blob(h, key, record, sizeof(record));
        if (err == ESP_OK)
        {
            err = nvs_commit(h);
        }
    }
    else
    {
        size_t length = sizeof(record);
        err = nvs_get_blob(h, key, record, &length);
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            err = ESP_OK;
        }
        else if (err == ESP_OK && length != sizeof(record))
        {
            err = ESP_FAIL;
        }
        else if (err == ESP_OK && memcmp(record, peer->fingerprint, 16) == 0)
        {
            peer->last_boot = ecos_link_get64(record + 16);
            peer->last_seq = ecos_link_get32(record + 24);
        }
    }
    nvs_close(h);
    return err == ESP_OK;
}

static bool load_peers(session_t *s)
{
    if (!ecos_store_load(s->store))
    {
        return false;
    }
    s->count = 0;
    for (unsigned i = 0; i < s->store[1] && i < ECOS_PEERS_MAX; i++)
    {
        const uint8_t *record = s->store + 2 + i * ECOS_RECORD_SIZE;
        link_peer_t *peer = &s->peers[s->count];
        memcpy(peer->mac, record, 6);
        memcpy(peer->name, record + 6, ECOS_NAME_SIZE);
        peer->name[ECOS_NAME_SIZE - 1] = 0;
        const uint8_t *credential = record + 6 + ECOS_NAME_SIZE;
        if (direction_key(credential, s->mac, PSA_KEY_USAGE_ENCRYPT, &peer->send_key) &&
            direction_key(credential, peer->mac, PSA_KEY_USAGE_DECRYPT, &peer->recv_key) &&
            hmac16(credential, (const uint8_t *)"REPLAY", 6, peer->fingerprint) && replay_record(peer, false, 0, 0))
        {
            portENTER_CRITICAL(&s_lock);
            member_info_t *info = info_slot(peer->mac);
            if (memcmp(info->fingerprint, peer->fingerprint, 16) == 0 &&
                ecos_link_newer(info->last_boot, info->last_seq, peer->last_boot, peer->last_seq))
            {
                peer->last_boot = info->last_boot;
                peer->last_seq = info->last_seq;
            }
            portEXIT_CRITICAL(&s_lock);
            s->count++;
        }
        else
        {
            psa_destroy_key(peer->send_key);
            psa_destroy_key(peer->recv_key);
            memset(peer, 0, sizeof(*peer));
            mbedtls_platform_zeroize(s->store, sizeof(s->store));
            return false;
        }
    }
    mbedtls_platform_zeroize(s->store, sizeof(s->store));
    return true;
}

static link_peer_t *find_peer(session_t *s, const uint8_t mac[6])
{
    for (int i = 0; i < s->count; i++)
    {
        if (memcmp(s->peers[i].mac, mac, 6) == 0)
        {
            return &s->peers[i];
        }
    }
    return NULL;
}

static void my_status(ecos_link_status_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", sys_nickname());
    snprintf(out->product, sizeof(out->product), "%s", board_info()->product);
    snprintf(out->version, sizeof(out->version), "%.15s", esp_app_get_description()->version);
    int battery = pmu_present() ? (int)state_get(STATE_BATTERY) : -1;
    out->battery = (int8_t)(battery < -1 || battery > 100 ? -1 : battery);
    bool ringing;
    portENTER_CRITICAL(&s_lock);
    ringing = s_ringing_here;
    portEXIT_CRITICAL(&s_lock);
    out->flags = (state_get(STATE_CHARGING) ? ECOS_LINK_F_CHARGING : 0) | (powered() ? ECOS_LINK_F_USB : 0) |
                 (auto_wanted() ? ECOS_LINK_F_LISTENING : 0) | (ringing ? ECOS_LINK_F_RINGING : 0);
    if (wifi_is_connected())
    {
        char ip[16];
        struct in_addr address;
        out->flags |= ECOS_LINK_F_WIFI;
        wifi_current_ssid(out->ssid, sizeof(out->ssid));
        if (wifi_get_ip(ip, sizeof(ip)) && inet_pton(AF_INET, ip, &address) == 1)
        {
            memcpy(out->ip, &address.s_addr, 4);
        }
    }
    out->uptime_min = (uint32_t)(esp_timer_get_time() / 60000000);
    size_t free_kb = sys_internal_free() / 1024;
    out->ram_kb = (uint16_t)(free_kb > 65535 ? 65535 : free_kb);
}

/* Seal and send one frame. With wait_ack: true when the device acknowledged it. */
static bool seal_send(session_t *s, link_peer_t *peer, uint8_t type, const uint8_t *payload, size_t length,
                      bool wait_ack)
{
    uint8_t frame[ECOS_LINK_FRAME_MAX], nonce[12] = {0};
    if (length > ECOS_LINK_PAYLOAD_MAX || s_tx_sequence == UINT32_MAX)
    {
        return false;
    }
    ecos_link_header(frame, type, s->mac, peer->mac, s->boot, ++s_tx_sequence);
    memcpy(nonce, frame + 17, sizeof(nonce));
    size_t written = 0;
    if (psa_aead_encrypt(peer->send_key, PSA_ALG_GCM, nonce, sizeof(nonce), frame, ECOS_LINK_HEADER, payload,
                         length, frame + ECOS_LINK_HEADER, sizeof(frame) - ECOS_LINK_HEADER, &written) != PSA_SUCCESS)
    {
        return false;
    }
    link_sent_t result;
    while (xQueueReceive(s_sent, &result, 0) == pdTRUE)
    {
    }
    if (esp_now_send(peer->mac, frame, ECOS_LINK_HEADER + written) != ESP_OK)
    {
        return false;
    }
    if (!wait_ack)
    {
        return true;
    }
    int64_t until = esp_timer_get_time() + ACK_WAIT_MS * 1000;
    while (esp_timer_get_time() < until)
    {
        if (xQueueReceive(s_sent, &result, pdMS_TO_TICKS(10)) == pdTRUE && memcmp(result.mac, peer->mac, 6) == 0)
        {
            return result.ok;
        }
    }
    return false;
}

static bool send_status(session_t *s, link_peer_t *peer, uint8_t type, bool wait_ack)
{
    uint8_t payload[ECOS_LINK_TIME_SIZE + ECOS_LINK_STATUS_SIZE];
    ecos_link_status_t status;
    my_status(&status);
    time_t now = time(NULL);
    ecos_link_put32(payload, now > CLOCK_VALID ? (uint32_t)now : 0);
    ecos_link_put_status(payload + ECOS_LINK_TIME_SIZE, &status);
    return seal_send(s, peer, type, payload, sizeof(payload), wait_ack);
}

static void raise_event(ecos_event_kind_t kind, const char *from)
{
    void (*handler)(const ecos_event_t *event);
    portENTER_CRITICAL(&s_lock);
    handler = s_handler;
    portEXIT_CRITICAL(&s_lock);
    if (handler != NULL)
    {
        ecos_event_t event = {.kind = kind};
        snprintf(event.from, sizeof(event.from), "%s", from);
        handler(&event);
    }
}

static void deliver_message(const char *from, const char *text)
{
    static uint32_t s_next_id = 0xEC000000;
    notify_t n = {.id = s_next_id++, .time = time(NULL)};
    snprintf(n.app, sizeof(n.app), "Ecosistema");
    snprintf(n.title, sizeof(n.title), "%s", from);
    snprintf(n.body, sizeof(n.body), "%s", text);
    notify_add(&n);
}

/* One received frame: open it, check it is fresh, act. */
static void handle_frame(session_t *s, const link_frame_t *frame)
{
    const uint8_t *p = frame->data;
    size_t n = frame->length;
    if (!ecos_link_valid(p, n) || memcmp(p + 5, frame->mac, 6) != 0 || memcmp(p + 11, s->mac, 6) != 0)
    {
        return;
    }
    link_peer_t *peer = find_peer(s, frame->mac);
    if (peer == NULL)
    {
        return;
    }
    uint8_t plain[ECOS_LINK_PAYLOAD_MAX], nonce[12] = {0};
    size_t length = 0;
    memcpy(nonce, p + 17, sizeof(nonce));
    if (psa_aead_decrypt(peer->recv_key, PSA_ALG_GCM, nonce, sizeof(nonce), p, ECOS_LINK_HEADER,
                         p + ECOS_LINK_HEADER, n - ECOS_LINK_HEADER, plain, sizeof(plain), &length) != PSA_SUCCESS ||
        length < ECOS_LINK_TIME_SIZE)
    {
        return;
    }
    uint64_t boot = ecos_link_get64(p + 17);
    uint32_t seq = ecos_link_get32(p + 25);
    if (!ecos_link_newer(boot, seq, peer->last_boot, peer->last_seq))
    {
        return;
    }
    uint8_t type = p[4];
    const uint8_t *body = plain + ECOS_LINK_TIME_SIZE;
    size_t body_length = length - ECOS_LINK_TIME_SIZE;
    uint32_t sent_at = ecos_link_get32(plain);
    time_t now = time(NULL);
    if (sent_at > CLOCK_VALID && now > CLOCK_VALID &&
        (now > (time_t)sent_at + CLOCK_WINDOW_S || (time_t)sent_at > now + CLOCK_WINDOW_S))
    {
        return;
    }
    if (((type == ECOS_LINK_STATUS_REQ || type == ECOS_LINK_STATUS) && body_length != ECOS_LINK_STATUS_SIZE) ||
        (type == ECOS_LINK_RING && (body_length != 1 || body[0] > 1)) ||
        (type == ECOS_LINK_MESSAGE && (body_length == 0 || body_length >= ECOS_LINK_TEXT_MAX)))
    {
        return;
    }
    if ((type == ECOS_LINK_RING || type == ECOS_LINK_MESSAGE) && !replay_record(peer, true, boot, seq))
    {
        return;
    }
    peer->last_boot = boot;
    peer->last_seq = seq;

    uint8_t channel = wifi_radio_channel();
    portENTER_CRITICAL(&s_lock);
    member_info_t *info = info_slot(peer->mac);
    memcpy(info->fingerprint, peer->fingerprint, 16);
    info->last_boot = boot;
    info->last_seq = seq;
    info->seen_at = esp_timer_get_time();
    info->rssi = frame->rssi;
    info->channel = channel;
    info->reachable = true;
    if ((type == ECOS_LINK_STATUS_REQ || type == ECOS_LINK_STATUS) && body_length >= ECOS_LINK_STATUS_SIZE)
    {
        ecos_link_get_status(body, &info->status);
        info->known = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (type == ECOS_LINK_STATUS_REQ)
    {
        send_status(s, peer, ECOS_LINK_STATUS, false);
    }
    else if (type == ECOS_LINK_RING && body_length >= 1)
    {
        ESP_LOGI(TAG, "%s %s", peer->name, body[0] ? "asks to ring" : "stops the ringing");
        if (body[0])
        {
            portENTER_CRITICAL(&s_lock);
            s_ringing_here = true;
            portEXIT_CRITICAL(&s_lock);
        }
        raise_event(body[0] ? ECOS_EVENT_RING : ECOS_EVENT_RING_STOP, peer->name);
        send_status(s, peer, ECOS_LINK_STATUS, false);
    }
    else if (type == ECOS_LINK_MESSAGE && body_length >= 1)
    {
        char text[ECOS_LINK_TEXT_MAX];
        size_t n = body_length < sizeof(text) - 1 ? body_length : sizeof(text) - 1;
        memcpy(text, body, n);
        text[n] = 0;
        for (size_t i = 0; i < n; i++)
        {
            if ((unsigned char)text[i] < 32)
            {
                text[i] = ' ';
            }
        }
        ESP_LOGI(TAG, "Message from %s", peer->name);
        deliver_message(peer->name, text);
    }
}

static void receive_for(session_t *s, int ms)
{
    link_frame_t frame;
    int64_t until = esp_timer_get_time() + ms * 1000LL;
    do
    {
        int64_t left = (until - esp_timer_get_time()) / 1000;
        if (xQueueReceive(s_rx, &frame, pdMS_TO_TICKS(left > 0 ? left : 0)) == pdTRUE)
        {
            handle_frame(s, &frame);
        }
    } while (esp_timer_get_time() < until);
}

/* Free radio: wait where the joined devices are (the home network's channel). */
static uint8_t park_channel(void)
{
    uint8_t home = wifi_home_channel();
    return home >= 1 && home <= 13 ? home : 1;
}

static void park(void)
{
    if (!wifi_is_connected())
    {
        wifi_radio_set_channel(park_channel());
    }
}

/* Look for `only` (or every paired device): each channel, STATUS_REQ, ACK. */
static int search(session_t *s, link_peer_t *only)
{
    int64_t started = esp_timer_get_time();
    bool reached[ECOS_PEERS_MAX] = {0};
    int wanted_count = only != NULL ? 1 : s->count, found = 0;
    uint8_t channels[sizeof(s_hops) + 1];
    size_t channel_count = 0;
    if (wifi_is_connected())
    {
        channels[channel_count++] = wifi_radio_channel();   /* tied to the network's channel */
    }
    else
    {
        channels[channel_count++] = park_channel();
        for (size_t i = 0; i < sizeof(s_hops); i++)
        {
            if (s_hops[i] != channels[0])
            {
                channels[channel_count++] = s_hops[i];
            }
        }
    }
    for (size_t c = 0; c < channel_count && found < wanted_count; c++)
    {
        portENTER_CRITICAL(&s_lock);
        bool suspended = s_suspend;
        portEXIT_CRITICAL(&s_lock);
        if (suspended)
        {
            break;
        }
        if (!wifi_is_connected())
        {
            wifi_radio_set_channel(channels[c]);
        }
        bool any = false;
        for (int i = 0; i < s->count; i++)
        {
            link_peer_t *peer = &s->peers[i];
            if (reached[i] || (only != NULL && peer != only))
            {
                continue;
            }
            if (send_status(s, peer, ECOS_LINK_STATUS_REQ, true))
            {
                any = true;
            }
        }
        receive_for(s, any ? REPLY_WAIT_MS : 5);   /* their STATUS answers */
        portENTER_CRITICAL(&s_lock);
        for (int i = 0; i < s->count; i++)
        {
            if (!reached[i] && (only == NULL || &s->peers[i] == only) &&
                info_slot(s->peers[i].mac)->seen_at >= started)
            {
                reached[i] = true;
                found++;
            }
        }
        portEXIT_CRITICAL(&s_lock);
    }
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s->count; i++)
    {
        if (!reached[i] && (only == NULL || &s->peers[i] == only))
        {
            info_slot(s->peers[i].mac)->reachable = false;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    park();
    return found;
}

/* Ring / stop / message: try the last known channel, then search. */
static void act(session_t *s, cmd_kind_t kind, const uint8_t mac[6], const char *text)
{
    link_peer_t *peer = find_peer(s, mac);
    if (peer == NULL)
    {
        set_message("Dispositivo non abbinato", NULL);
        return;
    }
    uint8_t payload[ECOS_LINK_TIME_SIZE + ECOS_LINK_TEXT_MAX];
    time_t now = time(NULL);
    ecos_link_put32(payload, now > CLOCK_VALID ? (uint32_t)now : 0);
    size_t length = ECOS_LINK_TIME_SIZE;
    uint8_t type = ECOS_LINK_RING;
    if (kind == CMD_MESSAGE)
    {
        size_t n = strnlen(text, ECOS_LINK_TEXT_MAX - 1);
        memcpy(payload + length, text, n);
        length += n;
        type = ECOS_LINK_MESSAGE;
    }
    else
    {
        payload[length++] = kind == CMD_RING ? 1 : 0;
    }
    uint8_t channel;
    portENTER_CRITICAL(&s_lock);
    channel = info_slot(mac)->channel;
    portEXIT_CRITICAL(&s_lock);
    bool sent = false;
    if (channel != 0 && (wifi_is_connected() ? wifi_radio_channel() == channel : wifi_radio_set_channel(channel)))
    {
        sent = seal_send(s, peer, type, payload, length, true);
    }
    if (!sent && search(s, peer) > 0)
    {
        portENTER_CRITICAL(&s_lock);
        channel = info_slot(mac)->channel;
        portEXIT_CRITICAL(&s_lock);
        if (wifi_is_connected() || wifi_radio_set_channel(channel))
        {
            sent = seal_send(s, peer, type, payload, length, true);
        }
    }
    if (sent)
    {
        receive_for(s, REPLY_WAIT_MS);   /* the ringing status comes back */
    }
    park();
    set_message(!sent ? "%s non raggiungibile: deve essere in carica a schermo spento, o con Ecosistema aperto"
                : kind == CMD_MESSAGE ? "Messaggio inviato a %s" : "Richiesta inviata a %s",
                peer->name);
}

static void link_task(void *arg);

static void start_link(void)
{
    portENTER_CRITICAL(&s_lock);
    bool start = !s_running && !s_suspend;
    if (start)
    {
        s_running = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (start && !sys_task_create(link_task, "ecos_link", 6144, NULL, 2, SYS_CORE_ANY))
    {
        portENTER_CRITICAL(&s_lock);
        s_running = false;
        portEXIT_CRITICAL(&s_lock);
        set_message("Memoria insufficiente per la radio", NULL);
    }
}

static void start_job(void *arg)
{
    (void)arg;
    start_link();
}

static void link_task(void *arg)
{
    (void)arg;
    session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM);
    bool radio = false, fast = false, espnow = false, lowered = false, restart = false;
    int8_t tx_power = 0;
    if (s == NULL || psa_crypto_init() != PSA_SUCCESS || esp_read_mac(s->mac, ESP_MAC_WIFI_STA) != ESP_OK)
    {
        set_message("Memoria insufficiente", NULL);
        goto done;
    }
    if (!load_peers(s))
    {
        set_message("Archivio ECOS non leggibile", NULL);
        goto done;
    }
    if (s->count == 0)
    {
        set_message("Nessun dispositivo abbinato", NULL);
        goto done;
    }
    if (!reserve_epoch())
    {
        set_message("Impossibile riservare la sessione in NVS", NULL);
        goto done;
    }
    s->boot = s_boot_epoch;
    if (wifi_radio_acquire() != ESP_OK)
    {
        set_message("Radio non disponibile (memoria)", NULL);
        goto done;
    }
    radio = true;
    wifi_set_fast(true);   /* modem sleep would miss the others' frames */
    fast = true;
    s_rx = xQueueCreateWithCaps(8, sizeof(link_frame_t), MALLOC_CAP_SPIRAM);
    s_sent = xQueueCreateWithCaps(8, sizeof(link_sent_t), MALLOC_CAP_SPIRAM);
    if (s_rx == NULL || s_sent == NULL || esp_now_init() != ESP_OK)
    {
        set_message("ESP-NOW non disponibile", NULL);
        goto done;
    }
    espnow = true;
    esp_now_register_recv_cb(on_radio);
    esp_now_register_send_cb(on_sent);
    for (int i = 0; i < s->count; i++)
    {
        esp_now_peer_info_t peer = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};
        memcpy(peer.peer_addr, s->peers[i].mac, 6);
        esp_now_add_peer(&peer);
    }
    esp_wifi_get_max_tx_power(&tx_power);
    park();
    ESP_LOGI(TAG, "Radio on, %d paired, channel %u", s->count, wifi_radio_channel());

    int64_t idle_since = 0;
    while (true)
    {
        portENTER_CRITICAL(&s_lock);
        bool stop = s_suspend, reload = s_reload, refresh = s_refresh;
        cmd_kind_t cmd = s_cmd;
        uint8_t mac[6];
        char text[ECOS_LINK_TEXT_MAX];
        memcpy(mac, s_cmd_mac, 6);
        memcpy(text, s_cmd_text, sizeof(text));
        s_reload = false;
        portEXIT_CRITICAL(&s_lock);
        if (stop || reload)
        {
            restart = reload && !stop;
            break;
        }
        if (!wanted())
        {
            if (idle_since == 0)
            {
                idle_since = esp_timer_get_time();
            }
            else if (esp_timer_get_time() - idle_since > IDLE_STOP_US)
            {
                break;
            }
        }
        else
        {
            idle_since = 0;
        }
        /* Reduced power without a network (no saturation side by side); a
         * joined network keeps full power for its own sake. */
        bool joined = wifi_is_connected();
        if (!joined && !lowered && tx_power > LINK_TX_POWER)
        {
            lowered = esp_wifi_set_max_tx_power(LINK_TX_POWER) == ESP_OK;
        }
        else if (joined && lowered)
        {
            esp_wifi_set_max_tx_power(tx_power);
            lowered = false;
        }
        portENTER_CRITICAL(&s_lock);
        s_listening = true;
        portEXIT_CRITICAL(&s_lock);
        if (refresh)
        {
            portENTER_CRITICAL(&s_lock);
            s_busy = true;
            s_refresh = false;
            portEXIT_CRITICAL(&s_lock);
            int found = search(s, NULL);
            char counts[24];
            snprintf(counts, sizeof(counts), "%d su %d", found, s->count);
            set_message(found > 0 ? "Raggiungibili: %s" : "Nessun dispositivo raggiungibile ora (%s)", counts);
            portENTER_CRITICAL(&s_lock);
            s_busy = false;
            portEXIT_CRITICAL(&s_lock);
        }
        else if (cmd != CMD_NONE)
        {
            portENTER_CRITICAL(&s_lock);
            s_busy = true;
            s_cmd = CMD_NONE;
            portEXIT_CRITICAL(&s_lock);
            act(s, cmd, mac, text);
            portENTER_CRITICAL(&s_lock);
            s_busy = false;
            portEXIT_CRITICAL(&s_lock);
        }
        receive_for(s, 100);
    }

done:
    portENTER_CRITICAL(&s_lock);
    s_listening = false;
    s_busy = false;
    portEXIT_CRITICAL(&s_lock);
    if (lowered)
    {
        esp_wifi_set_max_tx_power(tx_power);
    }
    if (espnow)
    {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        esp_now_deinit();
    }
    QueueHandle_t rx = s_rx, sent = s_sent;
    s_rx = NULL;
    s_sent = NULL;
    if (rx != NULL)
    {
        vQueueDeleteWithCaps(rx);
    }
    if (sent != NULL)
    {
        vQueueDeleteWithCaps(sent);
    }
    if (fast)
    {
        wifi_set_fast(false);
    }
    if (radio)
    {
        wifi_release();
    }
    if (s != NULL)
    {
        for (int i = 0; i < s->count; i++)
        {
            psa_destroy_key(s->peers[i].send_key);
            psa_destroy_key(s->peers[i].recv_key);
        }
        mbedtls_platform_zeroize(s, sizeof(*s));
        free(s);
    }
    portENTER_CRITICAL(&s_lock);
    s_running = false;
    /* Requests that came too late for this run, or keys to reload. */
    bool again = restart || s_refresh || s_cmd != CMD_NONE;
    if (!radio)
    {
        s_refresh = false;   /* nothing can be done without the radio: do not loop */
        s_cmd = CMD_NONE;
        again = false;
    }
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Radio off");
    sys_heap_log("ecos link stopped");
    if (again && !s_suspend)
    {
        sys_post(start_job, NULL);
    }
    vTaskDelete(NULL);
}

static void auto_check(void *arg)
{
    (void)arg;
    bool idle;
    portENTER_CRITICAL(&s_lock);
    idle = !s_running && !s_suspend;
    portEXIT_CRITICAL(&s_lock);
    if (idle && auto_wanted())
    {
        ecos_snapshot_t snapshot;
        ecos_snapshot(&snapshot);
        if (snapshot.count > 0)
        {
            sys_post(start_job, NULL);   /* the task is created by the sys worker, not here */
        }
    }
}

void ecos_link_init(void)
{
    esp_timer_create_args_t args = {.callback = auto_check, .name = "ecos_auto"};
    if (s_auto_timer == NULL && esp_timer_create(&args, &s_auto_timer) == ESP_OK)
    {
        esp_timer_start_periodic(s_auto_timer, AUTO_CHECK_US);
    }
}

void ecos_link_suspend(bool suspend)
{
    portENTER_CRITICAL(&s_lock);
    s_suspend = suspend;
    portEXIT_CRITICAL(&s_lock);
    if (!suspend && wanted())
    {
        sys_post(start_job, NULL);   /* someone was looking before the pairing */
    }
}

bool ecos_link_running(void)
{
    portENTER_CRITICAL(&s_lock);
    bool running = s_running;
    portEXIT_CRITICAL(&s_lock);
    return running;
}

void ecos_link_peers_changed(void)
{
    portENTER_CRITICAL(&s_lock);
    s_reload = s_running;
    portEXIT_CRITICAL(&s_lock);
}

/* ---- public API */

static bool parse_id(const char *id, uint8_t mac[6])
{
    if (id == NULL || strlen(id) != 12 || strspn(id, "0123456789abcdefABCDEF") != 12)
    {
        return false;
    }
    for (int i = 0; i < 6; i++)
    {
        char hex[3] = {id[i * 2], id[i * 2 + 1], 0};
        char *end = NULL;
        mac[i] = (uint8_t)strtoul(hex, &end, 16);
        if (end != hex + 2)
        {
            return false;
        }
    }
    return true;
}

void ecos_family(ecos_family_t *out)
{
    if (out == NULL)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    ecos_snapshot_t *snap = heap_caps_malloc(sizeof(*snap), MALLOC_CAP_SPIRAM);
    member_info_t *infos = heap_caps_malloc(sizeof(s_info), MALLOC_CAP_SPIRAM);
    if (snap == NULL || infos == NULL)
    {
        free(snap);
        free(infos);
        snprintf(out->message, sizeof(out->message), "Memoria insufficiente");
        return;
    }
    ecos_snapshot(snap);
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    out->radio = s_running;
    out->busy = s_busy || s_refresh || s_cmd != CMD_NONE;
    out->reachable_here = s_listening;
    memcpy(out->message, s_message, sizeof(out->message));
    memcpy(infos, s_info, sizeof(s_info));
    portEXIT_CRITICAL(&s_lock);
    out->message[sizeof(out->message) - 1] = 0;
    for (int i = 0; i < snap->count && i < ECOS_PEERS_MAX; i++)
    {
        ecos_member_t *m = &out->members[out->count++];
        snprintf(m->id, sizeof(m->id), "%s", snap->peers[i].id);
        snprintf(m->name, sizeof(m->name), "%s", snap->peers[i].name);
        m->seen_ago = UINT32_MAX;
        m->battery = -1;
        uint8_t mac[6];
        if (!parse_id(m->id, mac))
        {
            continue;
        }
        for (size_t k = 0; k < sizeof(s_info) / sizeof(s_info[0]); k++)
        {
            const member_info_t *info = &infos[k];
            if (!info->used || memcmp(info->mac, mac, 6) != 0)
            {
                continue;
            }
            m->known = info->known;
            m->reachable = info->reachable;
            m->seen_ago = info->seen_at != 0 ? (uint32_t)((now - info->seen_at) / 1000000) : UINT32_MAX;
            m->rssi = info->rssi;
            m->channel = info->channel;
            if (info->known)
            {
                const ecos_link_status_t *st = &info->status;
                snprintf(m->product, sizeof(m->product), "%s", st->product);
                snprintf(m->version, sizeof(m->version), "%s", st->version);
                m->battery = st->battery;
                m->charging = (st->flags & ECOS_LINK_F_CHARGING) != 0;
                m->usb = (st->flags & ECOS_LINK_F_USB) != 0;
                m->listening = (st->flags & ECOS_LINK_F_LISTENING) != 0;
                m->wifi = (st->flags & ECOS_LINK_F_WIFI) != 0;
                m->ringing = (st->flags & ECOS_LINK_F_RINGING) != 0;
                snprintf(m->ssid, sizeof(m->ssid), "%s", st->ssid);
                if (st->ip[0] != 0)
                {
                    snprintf(m->ip, sizeof(m->ip), "%u.%u.%u.%u", st->ip[0], st->ip[1], st->ip[2], st->ip[3]);
                }
                m->uptime_min = st->uptime_min;
                m->ram_kb = st->ram_kb;
                if (st->name[0] != 0)
                {
                    snprintf(m->name, sizeof(m->name), "%s", st->name);
                }
            }
            break;
        }
    }
    free(infos);
    free(snap);
}

void ecos_family_hold(bool on)
{
    portENTER_CRITICAL(&s_lock);
    s_holds += on ? 1 : (s_holds > 0 ? -1 : 0);
    portEXIT_CRITICAL(&s_lock);
    if (on)
    {
        start_link();
    }
}

void ecos_family_touch(void)
{
    portENTER_CRITICAL(&s_lock);
    s_web_until = esp_timer_get_time() + WEB_HOLD_US;
    portEXIT_CRITICAL(&s_lock);
    start_link();
}

esp_err_t ecos_family_refresh(void)
{
    portENTER_CRITICAL(&s_lock);
    bool suspended = s_suspend;
    s_refresh = !suspended;
    portEXIT_CRITICAL(&s_lock);
    if (suspended)
    {
        return ESP_ERR_INVALID_STATE;
    }
    set_message("Cerco i dispositivi...", NULL);
    start_link();
    return ESP_OK;
}

static esp_err_t command(cmd_kind_t kind, const char *id, const char *text)
{
    uint8_t mac[6];
    if (!parse_id(id, mac))
    {
        return ESP_ERR_INVALID_ARG;
    }
    char copy[ECOS_LINK_TEXT_MAX] = {0};
    snprintf(copy, sizeof(copy), "%s", text != NULL ? text : "");
    portENTER_CRITICAL(&s_lock);
    bool free_slot = s_cmd == CMD_NONE && !s_suspend;
    if (free_slot)
    {
        s_cmd = kind;
        memcpy(s_cmd_mac, mac, 6);
        memcpy(s_cmd_text, copy, sizeof(copy));
    }
    portEXIT_CRITICAL(&s_lock);
    if (!free_slot)
    {
        return ESP_ERR_INVALID_STATE;
    }
    set_message(kind == CMD_MESSAGE ? "Invio il messaggio..." : "Invio...", NULL);
    start_link();
    return ESP_OK;
}

esp_err_t ecos_family_ring(const char *id, bool on)
{
    return command(on ? CMD_RING : CMD_STOP, id, NULL);
}

esp_err_t ecos_family_message(const char *id, const char *text)
{
    if (text == NULL || text[0] == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return command(CMD_MESSAGE, id, text);
}

void ecos_family_ring_done(void)
{
    portENTER_CRITICAL(&s_lock);
    s_ringing_here = false;
    portEXIT_CRITICAL(&s_lock);
}

void ecos_family_set_handler(void (*handler)(const ecos_event_t *event))
{
    portENTER_CRITICAL(&s_lock);
    s_handler = handler;
    portEXIT_CRITICAL(&s_lock);
}
