#include "core/recovery.h"
/* Famiglia: the paired devices talk to each other over ESP-NOW.
 *
 *   ecos_family_hold() / _touch()      the UI or the dashboard looks
 *   explicit background opt-in         this device answers the others
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
#include "ecos_access_wire.h"
#include "ecos_orbit_wire.h"
#include "ecos_orbit_fragment.h"
#include "ecos_stream_slot.h"
#include "services/orbit_state.h"
#include "ecos_bus.h"
#include "services/web_server.h"
#include "services/motion.h"
#include <math.h>
#include "services/notify.h"
#include "services/wifi.h"
#include "core/power.h"
#include "core/state.h"
#include "core/sys.h"
#include "core/settings.h"
#include "services/storage.h"
#include "services/fw_update.h"
#include "hardware/audio.h"
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
#include "freertos/semphr.h"
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

_Static_assert(ECOS_ORBIT_MAX == ECOS_ORBIT_DATA_MAX, "Orbit API and wire bounds must agree");
_Static_assert(ECOS_LINK_TIME_SIZE + ECOS_ORBIT_ENVELOPE_MAX <= ECOS_LINK_PAYLOAD_MAX,
               "Orbit envelope must fit the encrypted EL02 payload");

static const char *TAG = "ecos_link";
static const uint8_t s_hops[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};

/* ---- shared state (s_lock) */

typedef struct
{
    uint8_t mac[6];
    bool used, known, reachable;
    int64_t seen_at, status_seen_at;
    int rssi;
    uint8_t channel;
    ecos_link_status_t status;
    uint8_t fingerprint[16];
    uint64_t last_boot;
    uint32_t last_seq;
    uint32_t permissions;
    uint64_t pending_request;
    uint8_t pending_operation;
    int64_t pending_until, access_at;
    ecos_access_wire_t access;
} member_info_t;

typedef enum
{
    CMD_NONE,
    CMD_RING,
    CMD_STOP,
    CMD_MESSAGE,
    CMD_WEB_CODE,
    CMD_RESOURCES,
    CMD_PET,
    CMD_ORBIT,
} cmd_kind_t;

/* Pet journey step in flight (one at a time). */
static EXT_RAM_BSS_ATTR uint8_t s_pet_step[ECOS_PET_MAX], s_pet_reply[ECOS_PET_MAX];
static size_t s_pet_step_len;
static int s_pet_reply_len = -1;
static uint8_t s_pet_mac[6];
static bool s_pet_waiting;
static SemaphoreHandle_t s_pet_done, s_pet_call;
static ecos_pet_handler_t s_pet_handler;

/* One bounded RPC slot. Buffers stay in PSRAM; no extra task or radio lease.
 * The caller owns the slot until its timeout; the worker copies before send. */
static EXT_RAM_BSS_ATTR uint8_t s_orbit_request[ECOS_ORBIT_MAX], s_orbit_reply[ECOS_ORBIT_MAX];
static size_t s_orbit_length;
static uint64_t s_orbit_request_id;
static uint8_t s_orbit_mac[6];
static int s_orbit_result = -1;
static bool s_orbit_calling, s_orbit_waiting, s_orbit_processing;
static ecos_orbit_handler_t s_orbit_handler;
static ecos_stream_handler_t s_stream_handler;
static EXT_RAM_BSS_ATTR ecos_stream_slot_t s_stream;
static StaticSemaphore_t s_stream_gate_storage;
static SemaphoreHandle_t s_stream_gate;

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
static bool parse_id(const char *id, uint8_t mac[6]);
static void id_text(const uint8_t mac[6], char text[13])
{
    snprintf(text, 13, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

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
static unsigned s_rx_users;

typedef struct
{
    uint8_t mac[6];
    uint8_t local_mac[6];
    ecos_family_ble_send_t send;
    void *context;
    unsigned users;
    bool active;
    bool closing;
    bool hold;
} ble_link_t;

static ble_link_t s_ble_link;

static bool queue_frame(const uint8_t mac[6], int8_t rssi, const uint8_t *data, size_t length)
{
    if (mac == NULL || data == NULL || length == 0 || length > ECOS_LINK_FRAME_MAX)
    {
        return false;
    }
    QueueHandle_t rx;
    portENTER_CRITICAL(&s_lock);
    rx = s_rx;
    if (rx != NULL)
    {
        ++s_rx_users;
    }
    portEXIT_CRITICAL(&s_lock);
    if (rx == NULL)
    {
        return false;
    }
    link_frame_t frame;
    memcpy(frame.mac, mac, sizeof(frame.mac));
    frame.length = (uint8_t)length;
    frame.rssi = rssi;
    memcpy(frame.data, data, length);
    bool queued = xQueueSend(rx, &frame, 0) == pdTRUE;
    portENTER_CRITICAL(&s_lock);
    --s_rx_users;
    portEXIT_CRITICAL(&s_lock);
    return queued;
}

static QueueHandle_t unpublish_rx(void)
{
    portENTER_CRITICAL(&s_lock);
    QueueHandle_t rx = s_rx;
    s_rx = NULL;
    portEXIT_CRITICAL(&s_lock);
    for (;;)
    {
        portENTER_CRITICAL(&s_lock);
        unsigned users = s_rx_users;
        portEXIT_CRITICAL(&s_lock);
        if (users == 0)
        {
            return rx;
        }
        vTaskDelay(1);
    }
}

static void on_radio(const esp_now_recv_info_t *info, const uint8_t *data, int length)
{
    if (info == NULL || info->src_addr == NULL || length <= 0)
    {
        return;
    }
    int8_t rssi = info->rx_ctrl != NULL ? (int8_t)info->rx_ctrl->rssi : 0;
    queue_frame(info->src_addr, rssi, data, (size_t)length);
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
    int64_t last_access_us;
    /* Re-answer an identical authenticated RPC retransmission without running
     * its side effect again. Session lives in PSRAM and is wiped on close. */
    ecos_orbit_cache_t orbit;
    ecos_orbit_fragment_t fragment;
} link_peer_t;

typedef struct
{
    uint8_t mac[6];
    uint64_t boot;
    int count;
    link_peer_t peers[ECOS_PEERS_MAX];
    uint8_t store[ECOS_STORE_SIZE];
    uint8_t orbit_tx[ECOS_LINK_FRAME_MAX];
    size_t orbit_tx_length;
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
static bool background_busy(void)
{
    return storage_files_busy() || audio_is_running() ||
        fw_update_phase() == FW_UPDATE_WRITING || fw_update_phase() == FW_UPDATE_RESTARTING;
}
static bool family_disabled(void) { return !recovery_active() && settings_get()->ecos_mode == 2; }
static bool auto_wanted(void)
{
    if (recovery_active()) return true;
    if (family_disabled() || background_busy()) return false;
    /* Charging is not consent to run the radio indefinitely. Automatic mode
     * serves explicit UI/web leases; only Background is an always-on opt-in. */
    return settings_get()->ecos_mode == 1 || web_server_running();
}

static bool wanted(void)
{
    if (family_disabled()) return false;
    portENTER_CRITICAL(&s_lock);
    bool want = s_holds > 0 || esp_timer_get_time() < s_web_until || s_refresh || s_cmd != CMD_NONE || s_orbit_waiting;
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

/* Grants are bound to the pairing credential, not just the MAC address. */
static uint32_t permissions_read(const uint8_t mac[6], const uint8_t fingerprint[16])
{
    char key[13];
    id_text(mac, key);
    nvs_handle_t handle;
    uint8_t record[18] = {0};
    size_t size = sizeof(record);
    uint32_t permissions = 0;
    if (nvs_open("ecos_rights", NVS_READONLY, &handle) == ESP_OK)
    {
        if (nvs_get_blob(handle, key, record, &size) == ESP_OK && size == sizeof(record) &&
            record[0] == 1 && memcmp(record + 1, fingerprint, 16) == 0)
        {
            permissions = record[17] & (ECOS_PERMISSION_WEB_CODE | ECOS_PERMISSION_RESOURCES);
        }
        nvs_close(handle);
    }
    return permissions;
}

esp_err_t ecos_permissions_set(const char *id, uint32_t permissions)
{
    uint8_t mac[6], store[ECOS_STORE_SIZE], record[18] = {1};
    if (!parse_id(id, mac) || (permissions & ~(ECOS_PERMISSION_WEB_CODE | ECOS_PERMISSION_RESOURCES)) != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ecos_store_load(store))
    {
        return ESP_FAIL;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (unsigned i = 0; i < store[1]; i++)
    {
        const uint8_t *peer = store + 2 + i * ECOS_RECORD_SIZE;
        if (memcmp(peer, mac, 6) != 0)
        {
            continue;
        }
        if (psa_crypto_init() != PSA_SUCCESS ||
            !hmac16(peer + 6 + ECOS_NAME_SIZE, (const uint8_t *)"REPLAY", 6, record + 1))
        {
            err = ESP_FAIL;
            break;
        }
        char key[13];
        id_text(mac, key);
        record[17] = (uint8_t)permissions;
        nvs_handle_t handle;
        err = nvs_open("ecos_rights", NVS_READWRITE, &handle);
        if (err == ESP_OK)
        {
            err = nvs_set_blob(handle, key, record, sizeof(record));
            if (err == ESP_OK)
            {
                err = nvs_commit(handle);
            }
            nvs_close(handle);
        }
        if (err == ESP_OK)
        {
            portENTER_CRITICAL(&s_lock);
            member_info_t *info = info_slot(mac);
            info->permissions = permissions;
            memcpy(info->fingerprint, record + 1, 16);
            portEXIT_CRITICAL(&s_lock);
        }
        break;
    }
    mbedtls_platform_zeroize(store, sizeof(store));
    mbedtls_platform_zeroize(record, sizeof(record));
    return err;
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
            uint32_t permissions = permissions_read(peer->mac, peer->fingerprint);
            portENTER_CRITICAL(&s_lock);
            member_info_t *info = info_slot(peer->mac);
            if (memcmp(info->fingerprint, peer->fingerprint, 16) != 0)
            {
                memset(info, 0, sizeof(*info));
                info->used = true;
                memcpy(info->mac, peer->mac, 6);
                memcpy(info->fingerprint, peer->fingerprint, 16);
            }
            info->permissions = permissions;
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
    snprintf(out->name, sizeof(out->name), "%s", settings_device_name());
    snprintf(out->product, sizeof(out->product), "%s", board_info()->product);
    snprintf(out->version, sizeof(out->version), "%.15s", esp_app_get_description()->version);
    int battery = pmu_present() ? (int)state_get(STATE_BATTERY) : -1;
    out->battery = (int8_t)(battery < -1 || battery > 100 ? -1 : battery);
    bool ringing;
    portENTER_CRITICAL(&s_lock);
    ringing = s_ringing_here;
    portEXIT_CRITICAL(&s_lock);
    out->flags = (state_get(STATE_CHARGING) ? ECOS_LINK_F_CHARGING : 0) |
                 (state_get(STATE_USB) ? ECOS_LINK_F_USB : 0) | (powered() ? ECOS_LINK_F_POWERED : 0) |
                 (auto_wanted() ? ECOS_LINK_F_LISTENING : 0) | (ringing ? ECOS_LINK_F_RINGING : 0) |
                 (recovery_active() ? ECOS_LINK_F_RECOVERY : 0) |
                 (ecos_family_companion(NULL) ? ECOS_LINK_F_COMPANION : 0);
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

/* 0 = use ESP-NOW, 1 = BLE enqueue accepted, -1 = BLE enqueue rejected. */
static int ble_send_bound(const uint8_t mac[6], const uint8_t *frame, size_t length)
{
    ecos_family_ble_send_t send = NULL;
    void *context = NULL;
    portENTER_CRITICAL(&s_lock);
    if (s_ble_link.active && memcmp(s_ble_link.mac, mac, 6) == 0)
    {
        ++s_ble_link.users;
        send = s_ble_link.send;
        context = s_ble_link.context;
    }
    portEXIT_CRITICAL(&s_lock);
    if (send == NULL)
    {
        return 0;
    }

    bool accepted = send(frame, length, context);
    portENTER_CRITICAL(&s_lock);
    --s_ble_link.users;
    portEXIT_CRITICAL(&s_lock);
    return accepted ? 1 : -1;
}

static int send_frame(const uint8_t mac[6], const uint8_t *frame, size_t length)
{
    int ble = ble_send_bound(mac, frame, length);
    if (ble != 0)
    {
        return ble;
    }
    return ecos_bus_send(mac, frame, length) == ESP_OK ? 0 : -1;
}

/* With wait_ack, BLE confirms local enqueue; ESP-NOW waits for the MAC ACK. */
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
    if (type == ECOS_LINK_ORBIT)
    {
        s->orbit_tx_length = ECOS_LINK_HEADER + written;
        memcpy(s->orbit_tx, frame, s->orbit_tx_length);
    }
    while (xQueueReceive(s_sent, &result, 0) == pdTRUE)
    {
    }
    int transport = send_frame(peer->mac, frame, ECOS_LINK_HEADER + written);
    if (transport < 0)
    {
        return false;
    }
    if (transport > 0 || !wait_ack)
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


static int16_t scaled_sensor(float value, float scale)
{
    if (!isfinite(value))
    {
        return 0;
    }
    float scaled = value * scale;
    return scaled > INT16_MAX ? INT16_MAX : scaled < INT16_MIN ? INT16_MIN : (int16_t)scaled;
}

static void answer_access(session_t *s, link_peer_t *peer, const uint8_t *request)
{
    uint8_t payload[ECOS_LINK_TIME_SIZE + ECOS_ACCESS_REPLY_SIZE];
    ecos_access_wire_t reply = {.request = ecos_link_get64(request), .operation = request[8],
                               .result = ECOS_ACCESS_DENIED, .battery = -1, .sample_age_ms = UINT32_MAX};
    uint32_t permissions = permissions_read(peer->mac, peer->fingerprint);
    uint32_t needed = reply.operation == ECOS_ACCESS_WEB ? ECOS_PERMISSION_WEB_CODE : ECOS_PERMISSION_RESOURCES;
    if ((permissions & needed) != 0)
    {
        ecos_link_status_t status;
        my_status(&status);
        if (reply.operation == ECOS_ACCESS_WEB)
        {
            bool recovery = false;
            char code[7] = {0};
            reply.result = ECOS_ACCESS_UNAVAILABLE;
            if (web_server_pairing_snapshot(code, &recovery))
            {
                reply.result = ECOS_ACCESS_OK;
                if (recovery)
                {
                    reply.flags |= ECOS_ACCESS_RECOVERY;
                }
                else
                {
                    memcpy(reply.code, code, 6);
                }
            }
            mbedtls_platform_zeroize(code, sizeof(code));
        }
        else
        {
            reply.result = ECOS_ACCESS_OK;
            reply.battery = status.battery;
            reply.battery_mv = pmu_present() ? (uint16_t)pmu_battery_mv() : 0;
            reply.ram_kb = status.ram_kb;
            if (board_info()->has_sdcard) reply.flags |= ECOS_ACCESS_SD;
            if (web_server_running()) reply.flags |= ECOS_ACCESS_WEB_READY;
            if (imu_supported())
            {
                reply.flags |= ECOS_ACCESS_IMU;
            }
            motion_snapshot_t motion;
            motion_snapshot(&motion);
            if (motion.running && motion.valid && motion.sample_monotonic_us > 0)
            {
                int64_t age = (esp_timer_get_time() - motion.sample_monotonic_us) / 1000;
                if (age >= 0 && age <= 1000 && imu_supported())
                {
                    reply.flags |= ECOS_ACCESS_IMU_VALID;
                    reply.sample_age_ms = (uint32_t)age;
                    for (int i = 0; i < 3; i++)
                    {
                        reply.acceleration_mg[i] = scaled_sensor(motion.sample.acceleration_g[i], 1000);
                        reply.angular_dps[i] = scaled_sensor(motion.sample.angular_dps[i], 1);
                    }
                }
            }
            if (status.flags & ECOS_LINK_F_CHARGING) reply.flags |= ECOS_ACCESS_CHARGING;
            if (status.flags & ECOS_LINK_F_USB) reply.flags |= ECOS_ACCESS_USB;
        }
        if (status.flags & ECOS_LINK_F_WIFI) reply.flags |= ECOS_ACCESS_WIFI;
        memcpy(reply.ip, status.ip, 4);
    }
    time_t now = time(NULL);
    ecos_link_put32(payload, now > CLOCK_VALID ? (uint32_t)now : 0);
    ecos_access_encode(payload + ECOS_LINK_TIME_SIZE, &reply);
    seal_send(s, peer, ECOS_LINK_ACCESS_REPLY, payload, sizeof(payload), false);
    mbedtls_platform_zeroize(&reply, sizeof(reply));
    mbedtls_platform_zeroize(payload, sizeof(payload));
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
        /* The AEAD was checked above. Only the exact last handled RPC can
         * retrieve its cached answer; stale commands are never executed. */
        if (p[4] == ECOS_LINK_ORBIT && ecos_orbit_cache_matches(&peer->orbit, boot, seq))
        {
            uint8_t answer[ECOS_LINK_TIME_SIZE + ECOS_ORBIT_ENVELOPE_MAX];
            time_t now = time(NULL);
            ecos_link_put32(answer, now > CLOCK_VALID ? (uint32_t)now : 0);
            memcpy(answer + ECOS_LINK_TIME_SIZE, peer->orbit.envelope, peer->orbit.length);
            seal_send(s, peer, ECOS_LINK_ORBIT_REPLY, answer, ECOS_LINK_TIME_SIZE + peer->orbit.length, true);
        }
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
        (type == ECOS_LINK_MESSAGE && (body_length == 0 || body_length >= ECOS_LINK_TEXT_MAX)) ||
        ((type == ECOS_LINK_PET || type == ECOS_LINK_PET_REPLY) && (body_length == 0 || body_length > ECOS_PET_MAX)) ||
        ((type == ECOS_LINK_ORBIT || type == ECOS_LINK_ORBIT_REPLY) &&
         !ecos_orbit_envelope_valid(body, body_length)) ||
        (type == ECOS_LINK_STREAM && (body_length == 0 || body_length > ECOS_STREAM_MAX)) ||
        (type == ECOS_LINK_ACCESS_REQ && !ecos_access_request_valid(body, body_length)) ||
        (type == ECOS_LINK_ACCESS_REPLY && body_length != ECOS_ACCESS_REPLY_SIZE))
    {
        return;
    }
    int64_t access_now = esp_timer_get_time();
    if (type == ECOS_LINK_ACCESS_REQ && peer->last_access_us != 0 &&
        access_now - peer->last_access_us < 2000000)
    {
        return;
    }
    if ((type == ECOS_LINK_RING || type == ECOS_LINK_MESSAGE || type == ECOS_LINK_PET || type == ECOS_LINK_ORBIT ||
         (type == ECOS_LINK_ACCESS_REQ && body[8] == ECOS_ACCESS_WEB)) &&
        !replay_record(peer, true, boot, seq))
    {
        return;
    }
    peer->last_boot = boot;
    peer->last_seq = seq;

    uint8_t channel = wifi_radio_channel();
    int64_t seen_at = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    member_info_t *info = info_slot(peer->mac);
    memcpy(info->fingerprint, peer->fingerprint, 16);
    info->last_boot = boot;
    info->last_seq = seq;
    info->seen_at = seen_at;
    info->rssi = frame->rssi;
    info->channel = channel;
    info->reachable = true;
    if ((type == ECOS_LINK_STATUS_REQ || type == ECOS_LINK_STATUS) && body_length >= ECOS_LINK_STATUS_SIZE)
    {
        ecos_link_get_status(body, &info->status);
        info->known = true;
        info->status_seen_at = seen_at;
    }
    portEXIT_CRITICAL(&s_lock);

    if (type == ECOS_LINK_STREAM)
    {
        portENTER_CRITICAL(&s_lock);
        ecos_stream_handler_t handler = ecos_stream_matches(&s_stream, peer->mac) ? s_stream_handler : NULL;
        portEXIT_CRITICAL(&s_lock);
        if (handler != NULL)
        {
            char id[13];
            id_text(peer->mac, id);
            handler(id, body, body_length);
        }
    }
    else if (type == ECOS_LINK_ORBIT)
    {
        ecos_orbit_handler_t handler;
        portENTER_CRITICAL(&s_lock);
        handler = s_orbit_handler;
        portEXIT_CRITICAL(&s_lock);
        uint8_t answer[ECOS_LINK_TIME_SIZE + 8 + ECOS_ORBIT_MAX];
        char id[13];
        id_text(peer->mac, id);
        uint8_t *output = answer + ECOS_LINK_TIME_SIZE + 8;
        size_t count = 0;
        if (body_length >= 12 && memcmp(body + 8, "OF01", 4) == 0)
        {
            ecos_orbit_fragment_result_t result = ecos_orbit_fragment_accept(&peer->fragment, body + 8, body_length - 8);
            if (result == ECOS_ORBIT_FRAGMENT_MORE)
            {
                count = ecos_orbit_fragment_ack(output, ECOS_ORBIT_MAX, peer->fragment.used);
            }
            else if (result == ECOS_ORBIT_FRAGMENT_READY)
            {
                count = handler ? handler(id, peer->fragment.data, peer->fragment.used, output, ECOS_ORBIT_MAX) : 0;
                if (count <= ECOS_ORBIT_MAX)
                {
                    memcpy(peer->fragment.reply, output, count);
                    peer->fragment.reply_length = count;
                }
            }
            else if (result == ECOS_ORBIT_FRAGMENT_REPLIED)
            {
                count = peer->fragment.reply_length;
                memcpy(output, peer->fragment.reply, count);
            }
        }
        else if (handler != NULL)
        {
            count = handler(id, body + 8, body_length - 8, output, ECOS_ORBIT_MAX);
        }
        if (count > 0 && count <= ECOS_ORBIT_MAX)
        {
            ecos_link_put32(answer, now > CLOCK_VALID ? (uint32_t)now : 0);
            memcpy(answer + ECOS_LINK_TIME_SIZE, body, 8);
            ecos_orbit_cache_store(&peer->orbit, boot, seq, answer + ECOS_LINK_TIME_SIZE, 8 + count);
            seal_send(s, peer, ECOS_LINK_ORBIT_REPLY, answer, ECOS_LINK_TIME_SIZE + 8 + count, true);
        }
    }
    else if (type == ECOS_LINK_ORBIT_REPLY)
    {
        portENTER_CRITICAL(&s_lock);
        if (s_orbit_waiting && ecos_orbit_ids_match(s_orbit_request_id, ecos_orbit_get_request_id(body)) &&
            memcmp(s_orbit_mac, peer->mac, 6) == 0)
        {
            memcpy(s_orbit_reply, body + 8, body_length - 8);
            s_orbit_result = (int)(body_length - 8);
            s_orbit_waiting = false;
        }
        portEXIT_CRITICAL(&s_lock);
    }
    else if (type == ECOS_LINK_ACCESS_REQ)
    {
        peer->last_access_us = access_now;
        answer_access(s, peer, body);
    }
    else if (type == ECOS_LINK_ACCESS_REPLY)
    {
        ecos_access_wire_t access;
        if (ecos_access_decode(body, body_length, &access))
        {
            portENTER_CRITICAL(&s_lock);
            info = info_slot(peer->mac);
            if (info->pending_request == access.request && info->pending_request != 0 &&
                info->pending_operation == access.operation && access_now <= info->pending_until)
            {
                info->access = access;
                info->access_at = access_now;
                info->pending_request = 0;
            }
            portEXIT_CRITICAL(&s_lock);
            mbedtls_platform_zeroize(&access, sizeof(access));
        }
    }
    else if (type == ECOS_LINK_STATUS_REQ)
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
    else if (type == ECOS_LINK_PET)
    {
        ecos_pet_handler_t handler;
        portENTER_CRITICAL(&s_lock);
        handler = s_pet_handler;
        portEXIT_CRITICAL(&s_lock);
        uint8_t answer[ECOS_LINK_TIME_SIZE + ECOS_PET_MAX];
        char id[13];
        id_text(peer->mac, id);
        size_t n = handler ? handler(id, peer->name, body, body_length, answer + ECOS_LINK_TIME_SIZE, ECOS_PET_MAX) : 0;
        if (n > 0 && n <= ECOS_PET_MAX)
        {
            time_t t = time(NULL);
            ecos_link_put32(answer, t > CLOCK_VALID ? (uint32_t)t : 0);
            seal_send(s, peer, ECOS_LINK_PET_REPLY, answer, ECOS_LINK_TIME_SIZE + n, true);
        }
    }
    else if (type == ECOS_LINK_PET_REPLY)
    {
        portENTER_CRITICAL(&s_lock);
        if (s_pet_waiting && memcmp(s_pet_mac, peer->mac, 6) == 0)
        {
            memcpy(s_pet_reply, body, body_length);
            s_pet_reply_len = (int)body_length;
            s_pet_waiting = false;
        }
        portEXIT_CRITICAL(&s_lock);
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
    uint8_t pinned = wifi_radio_pinned_channel();
    if (pinned >= 1 && pinned <= 13) return pinned;
    uint8_t home = wifi_home_channel();
    uint8_t shared = 0;
    size_t shared_length = 0;
    static const uint8_t key[] = "ecos/channel";
    if (orbit_state_get(key, sizeof(key) - 1, &shared, sizeof(shared), &shared_length, NULL, NULL) &&
        shared_length == 1 && shared >= 1 && shared <= 13) home = shared;
    portENTER_CRITICAL(&s_lock);
    if (s_stream.active)
    {
        for (size_t i = 0; i < sizeof(s_info) / sizeof(s_info[0]); ++i)
        {
            if (memcmp(s_info[i].mac, s_stream.mac, 6) == 0 && s_info[i].channel >= 1 && s_info[i].channel <= 13)
            {
                home = s_info[i].channel;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return home >= 1 && home <= 13 ? home : wifi_radio_common_channel();
}

static void park(void)
{
    if (!wifi_is_connected())
    {
        uint8_t channel = park_channel();
        if (wifi_radio_set_channel(channel) && !ecos_family_stream_active() &&
            wifi_radio_common_channel_set(channel))
        {
            static uint8_t announced;
            static const uint8_t key[] = "ecos/channel";
            uint8_t shared = 0;
            size_t length = 0;
            bool present = orbit_state_get(key, sizeof(key) - 1, &shared, sizeof(shared), &length, NULL, NULL);
            if ((present && (length != 1 || shared != channel)) || (!present && announced != channel))
            {
                if (orbit_state_put(key, sizeof(key) - 1, &channel, 1)) announced = channel;
            }
        }
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
            if (!wifi_radio_set_channel(channels[c])) continue;
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
    uint8_t payload[ECOS_LINK_PAYLOAD_MAX];
    time_t now = time(NULL);
    ecos_link_put32(payload, now > CLOCK_VALID ? (uint32_t)now : 0);
    size_t length = ECOS_LINK_TIME_SIZE;
    uint8_t type = ECOS_LINK_RING;
    if (kind == CMD_WEB_CODE || kind == CMD_RESOURCES)
    {
        uint64_t request;
        do
        {
            esp_fill_random(&request, sizeof(request));
        } while (request == 0);
        uint8_t operation = kind == CMD_WEB_CODE ? ECOS_ACCESS_WEB : ECOS_ACCESS_RESOURCES;
        ecos_link_put64(payload + length, request);
        payload[length + 8] = operation;
        length += ECOS_ACCESS_REQUEST_SIZE;
        type = ECOS_LINK_ACCESS_REQ;
        portENTER_CRITICAL(&s_lock);
        member_info_t *info = info_slot(mac);
        memset(&info->access, 0, sizeof(info->access));
        info->access_at = 0;
        info->pending_request = request;
        info->pending_operation = operation;
        info->pending_until = esp_timer_get_time() + 10000000;
        portEXIT_CRITICAL(&s_lock);
    }
    else if (kind == CMD_ORBIT)
    {
        s->orbit_tx_length = 0;
        portENTER_CRITICAL(&s_lock);
        bool waiting = s_orbit_waiting;
        bool packed = ecos_orbit_pack(payload + length, sizeof(payload) - length, s_orbit_request_id,
                                      s_orbit_request, s_orbit_length);
        length += ECOS_ORBIT_REQUEST_ID_SIZE + s_orbit_length;
        portEXIT_CRITICAL(&s_lock);
        if (!waiting || !packed)
        {
            return;   /* caller timed out before this command was taken */
        }
        type = ECOS_LINK_ORBIT;
    }
    else if (kind == CMD_PET)
    {
        portENTER_CRITICAL(&s_lock);
        memcpy(payload + length, s_pet_step, s_pet_step_len);
        length += s_pet_step_len;
        memcpy(s_pet_mac, mac, 6);
        s_pet_reply_len = -1;
        s_pet_waiting = true;
        portEXIT_CRITICAL(&s_lock);
        type = ECOS_LINK_PET;
    }
    else if (kind == CMD_MESSAGE)
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
    if (kind == CMD_ORBIT)
    {
        /* Discover before issuing a side effect. Never retransmit an Orbit
         * command with a new sequence after an ambiguous radio result. */
        if (search(s, peer) <= 0)
        {
            return;
        }
        portENTER_CRITICAL(&s_lock);
        channel = info_slot(mac)->channel;
        portEXIT_CRITICAL(&s_lock);
    }
    if (kind == CMD_ORBIT)
    {
        portENTER_CRITICAL(&s_lock);
        bool waiting = s_orbit_waiting;
        portEXIT_CRITICAL(&s_lock);
        if (!waiting)
        {
            park();
            return;
        }
    }
    if (channel != 0 && (wifi_is_connected() ? wifi_radio_channel() == channel : wifi_radio_set_channel(channel)))
    {
        sent = seal_send(s, peer, type, payload, length, true);
    }
    if (!sent && kind != CMD_ORBIT && search(s, peer) > 0)
    {
        portENTER_CRITICAL(&s_lock);
        channel = info_slot(mac)->channel;
        portEXIT_CRITICAL(&s_lock);
        if (wifi_is_connected() || wifi_radio_set_channel(channel))
        {
            sent = seal_send(s, peer, type, payload, length, true);
        }
    }
    if (kind == CMD_ORBIT)
    {
        /* Application replies, rather than radio ACKs, confirm completion.
         * Retransmit the exact sealed frame: same nonce, ciphertext and ID;
         * the receiver's cache replays the answer without another side effect. */
        for (unsigned attempt = 0; s->orbit_tx_length > 0 && attempt < 3; ++attempt)
        {
            int64_t until = esp_timer_get_time() + REPLY_WAIT_MS * (int64_t)(attempt + 1) * 1000;
            do
            {
                receive_for(s, 10);
                portENTER_CRITICAL(&s_lock);
                bool pending = s_orbit_waiting;
                portEXIT_CRITICAL(&s_lock);
                if (!pending) break;
            } while (esp_timer_get_time() < until);
            portENTER_CRITICAL(&s_lock);
            bool waiting = s_orbit_waiting;
            portEXIT_CRITICAL(&s_lock);
            if (!waiting)
            {
                break;
            }
            if (attempt < 2)
            {
                send_frame(peer->mac, s->orbit_tx, s->orbit_tx_length);
            }
        }
        park();
        return;
    }
    if (sent)
    {
        /* The ringing status comes back; a pet step waits for its answer. */
        for (int i = 0; i < (kind == CMD_PET ? 10 : 1); i++)
        {
            receive_for(s, REPLY_WAIT_MS);
            portENTER_CRITICAL(&s_lock);
            bool answered = s_pet_reply_len >= 0;
            portEXIT_CRITICAL(&s_lock);
            if (kind != CMD_PET || answered)
                break;
        }
    }
    if (kind == CMD_PET)
    {
        portENTER_CRITICAL(&s_lock);
        s_pet_waiting = false;
        portEXIT_CRITICAL(&s_lock);
        xSemaphoreGive(s_pet_done);
        park();
        return;
    }
    park();
    set_message(!sent ? "%s non raggiungibile: deve essere in carica a schermo spento, o con Ecosistema aperto"
                : kind == CMD_MESSAGE ? "Messaggio inviato a %s" : "Richiesta inviata a %s",
                peer->name);
}

static void link_task(void *arg);

static void start_link(void)
{
    if (family_disabled())
    {
        portENTER_CRITICAL(&s_lock);
        if (s_cmd == CMD_ORBIT)
        {
            s_cmd = CMD_NONE;
            s_orbit_waiting = false;
        }
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    portENTER_CRITICAL(&s_lock);
    bool start = !s_running && !s_suspend;
    if (!s_running && s_suspend && s_cmd == CMD_ORBIT)
    {
        s_cmd = CMD_NONE;
        s_orbit_waiting = false;
    }
    if (start)
    {
        s_running = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (start && !sys_task_create(link_task, "ecos_link", 6144, NULL, 2, SYS_CORE_ANY))
    {
        portENTER_CRITICAL(&s_lock);
        s_running = false;
        if (s_cmd == CMD_ORBIT)
        {
            s_cmd = CMD_NONE;
            s_orbit_waiting = false;
        }
        portEXIT_CRITICAL(&s_lock);
        set_message("Memoria insufficiente per la radio", NULL);
    }
}

static void start_job(void *arg)
{
    (void)arg;
    if (wanted()) start_link();
}

static bool ble_termination_required(bool radio, bool policy_off, bool suspended, bool restart)
{
    return !radio || policy_off || suspended || restart;
}

static void link_task(void *arg)
{
    (void)arg;
    session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM);
    bool radio = false, fast = false, espnow = false, lowered = false, restart = false;
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
    QueueHandle_t rx_queue = xQueueCreateWithCaps(8, sizeof(link_frame_t), MALLOC_CAP_SPIRAM);
    QueueHandle_t sent_queue = xQueueCreateWithCaps(8, sizeof(link_sent_t), MALLOC_CAP_SPIRAM);
    portENTER_CRITICAL(&s_lock);
    s_rx = rx_queue;
    s_sent = sent_queue;
    portEXIT_CRITICAL(&s_lock);
    if (rx_queue == NULL || sent_queue == NULL || ecos_bus_open("EL02", on_radio, on_sent, 0) != ESP_OK)
    {
        set_message("ESP-NOW non disponibile", NULL);
        goto done;
    }
    espnow = true;
    wifi_set_fast(true);
    fast = true;
    for (int i = 0; i < s->count; i++)
    {
        if (ecos_bus_peer(s->peers[i].mac) != ESP_OK)
        {
            set_message("Elenco radio non disponibile", NULL);
            goto done;
        }
    }
    radio = true;   /* startup completed: queued work may now be retried */
    park();
    ESP_LOGI(TAG, "Radio on, %d paired, channel %u", s->count, wifi_radio_channel());

    int64_t idle_since = 0;
    while (true)
    {
        portENTER_CRITICAL(&s_lock);
        bool stop = s_suspend, reload = s_reload, refresh = s_refresh;
        cmd_kind_t cmd = s_cmd;
        if (cmd == CMD_ORBIT)
        {
            s_busy = true;   /* claim before a caller can time out and replace the slot */
            s_orbit_processing = !refresh;
        }
        uint8_t mac[6];
        char text[ECOS_LINK_TEXT_MAX];
        memcpy(mac, s_cmd_mac, 6);
        memcpy(text, s_cmd_text, sizeof(text));
        s_reload = false;
        portEXIT_CRITICAL(&s_lock);
        bool policy_off = family_disabled();
        if (policy_off) {
            portENTER_CRITICAL(&s_lock);
            s_cmd = CMD_NONE; s_refresh = false;
            portEXIT_CRITICAL(&s_lock);
        }
        if (stop || reload || policy_off || (!recovery_active() && background_busy() && !wanted()))
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
                /* Commit idle shutdown under the same lock as conditional
                 * holds, so gossip cannot acquire a radio already closing. */
                portENTER_CRITICAL(&s_lock);
                bool close = s_holds == 0 && s_cmd == CMD_NONE && !s_refresh &&
                             esp_timer_get_time() >= s_web_until && !s_orbit_waiting;
                if (close) s_listening = false;
                portEXIT_CRITICAL(&s_lock);
                if (close) break;
                idle_since = 0;
            }
        }
        else
        {
            idle_since = 0;
        }
        /* Reduced power without a network (no saturation side by side); a
         * joined network or recovery hotspot keeps full power. */
        bool joined = wifi_is_connected() || recovery_active();
        if (!joined && !lowered)
        {
            lowered = ecos_bus_power("EL02", LINK_TX_POWER) == ESP_OK;
        }
        else if (joined && lowered)
        {
            lowered = ecos_bus_power("EL02", 0) != ESP_OK;
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
            s_orbit_processing = cmd == CMD_ORBIT;
            portEXIT_CRITICAL(&s_lock);
            act(s, cmd, mac, text);
            portENTER_CRITICAL(&s_lock);
            s_busy = false;
            s_orbit_processing = false;
            portEXIT_CRITICAL(&s_lock);
        }
        /* Audio has a 15 ms cadence; the same family worker drains its lossy
         * mailbox without allocating a second radio task or blocking for ACKs. */
        uint8_t stream_mac[6], stream_payload[ECOS_LINK_TIME_SIZE + ECOS_STREAM_MAX];
        portENTER_CRITICAL(&s_lock);
        bool streaming = s_stream.active;
        size_t stream_length = ecos_stream_take(&s_stream, stream_mac, stream_payload + ECOS_LINK_TIME_SIZE, ECOS_STREAM_MAX);
        portEXIT_CRITICAL(&s_lock);
        if (stream_length > 0)
        {
            link_peer_t *peer = find_peer(s, stream_mac);
            time_t now = time(NULL);
            ecos_link_put32(stream_payload, now > CLOCK_VALID ? (uint32_t)now : 0);
            park();
            if (peer != NULL) seal_send(s, peer, ECOS_LINK_STREAM, stream_payload,
                                        ECOS_LINK_TIME_SIZE + stream_length, false);
        }
        receive_for(s, streaming ? 5 : 100);
    }

done:
    portENTER_CRITICAL(&s_lock);
    s_listening = false;
    s_busy = false;
    s_orbit_processing = false;
    portEXIT_CRITICAL(&s_lock);
    if (s_stream_gate != NULL)
    {
        xSemaphoreTake(s_stream_gate, portMAX_DELAY);
        portENTER_CRITICAL(&s_lock);
        memset(&s_stream, 0, sizeof(s_stream));
        portEXIT_CRITICAL(&s_lock);
        wifi_radio_channel_release("OA01");
        xSemaphoreGive(s_stream_gate);
    }
    if (espnow)
    {
        ecos_bus_close("EL02");   /* restores the TX power when nobody else needs it lower */
    }
    bool policy_off = family_disabled();
    uint8_t ble_mac[6];
    portENTER_CRITICAL(&s_lock);
    bool ble_terminate = ble_termination_required(radio, policy_off, s_suspend, restart) && s_ble_link.active;
    void *ble_context = ble_terminate ? s_ble_link.context : NULL;
    if (ble_terminate)
    {
        memcpy(ble_mac, s_ble_link.mac, sizeof(ble_mac));
    }
    portEXIT_CRITICAL(&s_lock);
    if (ble_context != NULL)
    {
        /* A null frame with zero length is the terminal lease notification. */
        ble_send_bound(ble_mac, NULL, 0);
        ecos_family_ble_close(ble_context);
    }
    QueueHandle_t rx = unpublish_rx();
    portENTER_CRITICAL(&s_lock);
    QueueHandle_t sent = s_sent;
    s_sent = NULL;
    portEXIT_CRITICAL(&s_lock);
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
    /* No caller must wait out its whole timeout after failed startup,
     * suspension or a policy stop. No response means an unknown outcome. */
    s_orbit_waiting = false;
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
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < sizeof(s_info) / sizeof(s_info[0]); i++)
    {
        if (s_info[i].access_at != 0 && now - s_info[i].access_at > 60000000)
        {
            memset(&s_info[i].access, 0, sizeof(s_info[i].access));
            s_info[i].access_at = 0;
        }
    }
    idle = !s_running && !s_suspend;
    portEXIT_CRITICAL(&s_lock);
    orbit_state_tick();
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
    if (s_stream_gate == NULL) s_stream_gate = xSemaphoreCreateMutexStatic(&s_stream_gate_storage);
    if (!orbit_state_init()) ESP_LOGW(TAG, "Orbit state unavailable");
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
            m->permissions = info->permissions;
            m->access_pending = info->pending_request != 0 && now <= info->pending_until;
            m->access_known = info->access_at != 0 && now - info->access_at <= 60000000;
            m->access_age_s = info->access_at != 0 ? (uint32_t)((now - info->access_at) / 1000000) : 0;
            if (m->access_known)
            {
                m->access_operation = info->access.operation;
                m->access_result = info->access.result;
                m->resource_flags = info->access.flags;
                memcpy(m->web_code, info->access.code, sizeof(m->web_code));
                m->battery_mv = info->access.battery_mv;
                m->resource_battery = info->access.battery;
                m->resource_ram_kb = info->access.ram_kb;
                m->sample_age_ms = info->access.sample_age_ms;
                memcpy(m->acceleration_mg, info->access.acceleration_mg, sizeof(m->acceleration_mg));
                memcpy(m->angular_dps, info->access.angular_dps, sizeof(m->angular_dps));
            }
            else if (info->pending_request != 0 && now > info->pending_until)
            {
                m->access_known = true;
                m->access_operation = info->pending_operation;
                m->access_result = 3; /* No supported reply within the request window. */
            }
            m->known = info->known;
            m->reachable = info->reachable && info->seen_at != 0 && now - info->seen_at <= 90000000;
            m->seen_ago = info->seen_at != 0 ? (uint32_t)((now - info->seen_at) / 1000000) : UINT32_MAX;
            m->rssi = info->rssi;
            m->channel = info->channel;
            if (info->known)
            {
                const ecos_link_status_t *st = &info->status;
                snprintf(m->product, sizeof(m->product), "%s", st->product);
                snprintf(m->version, sizeof(m->version), "%s", st->version);
                m->battery = st->battery;
                m->recovery = (st->flags & ECOS_LINK_F_RECOVERY) != 0;
                m->companion = (st->flags & ECOS_LINK_F_COMPANION) != 0;
                m->charging = (st->flags & ECOS_LINK_F_CHARGING) != 0;
                m->usb = (st->flags & ECOS_LINK_F_USB) != 0;
                m->powered = (st->flags & ECOS_LINK_F_POWERED) != 0;
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

bool ecos_family_hold_active(void)
{
    if (family_disabled()) return false;
    portENTER_CRITICAL(&s_lock);
    bool active = s_running && s_listening && !s_suspend;
    if (active) ++s_holds;
    portEXIT_CRITICAL(&s_lock);
    return active;
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
    if (family_disabled()) return ESP_ERR_INVALID_STATE;
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
    if (family_disabled()) return ESP_ERR_INVALID_STATE;
    uint8_t mac[6];
    if (!parse_id(id, mac))
    {
        return ESP_ERR_INVALID_ARG;
    }
    char copy[ECOS_LINK_TEXT_MAX] = {0};
    snprintf(copy, sizeof(copy), "%s", text != NULL ? text : "");
    portENTER_CRITICAL(&s_lock);
    bool free_slot = s_cmd == CMD_NONE && !s_suspend && !s_busy;
    if (free_slot && (kind == CMD_WEB_CODE || kind == CMD_RESOURCES))
    {
        member_info_t *info = info_slot(mac);
        int64_t now = esp_timer_get_time();
        free_slot = (info->pending_request == 0 || now > info->pending_until) &&
                    (info->access_at == 0 || now - info->access_at >= 2000000);
    }
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

esp_err_t ecos_family_request(const char *id, uint8_t operation)
{
    if (operation != ECOS_ACCESS_WEB && operation != ECOS_ACCESS_RESOURCES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return command(operation == ECOS_ACCESS_WEB ? CMD_WEB_CODE : CMD_RESOURCES, id, NULL);
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

void ecos_family_set_pet_handler(ecos_pet_handler_t handler)
{
    portENTER_CRITICAL(&s_lock);
    s_pet_handler = handler;
    portEXIT_CRITICAL(&s_lock);
}

int ecos_family_pet(const char *id, const uint8_t *step, size_t length, uint8_t *reply, size_t max, int timeout_ms)
{
    if (length == 0 || length > ECOS_PET_MAX)
        return -1;
    if (s_pet_call == NULL)
    {
        SemaphoreHandle_t call = xSemaphoreCreateMutex(), done = xSemaphoreCreateBinary();
        if (call == NULL || done == NULL)
            return -1;
        s_pet_call = call;
        s_pet_done = done;
    }
    xSemaphoreTake(s_pet_call, portMAX_DELAY);
    xSemaphoreTake(s_pet_done, 0);
    portENTER_CRITICAL(&s_lock);
    memcpy(s_pet_step, step, length);
    s_pet_step_len = length;
    s_pet_reply_len = -1;
    portEXIT_CRITICAL(&s_lock);
    int result = -1;
    /* The worker may be busy with a search: retry the slot for a while. */
    int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (command(CMD_PET, id, NULL) != ESP_OK)
    {
        if (esp_timer_get_time() > until)
            goto out;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    int64_t left = (until - esp_timer_get_time()) / 1000;
    if (xSemaphoreTake(s_pet_done, pdMS_TO_TICKS(left > 500 ? left : 500)) == pdTRUE)
    {
        portENTER_CRITICAL(&s_lock);
        if (s_pet_reply_len >= 0)
        {
            result = s_pet_reply_len < (int)max ? s_pet_reply_len : (int)max;
            memcpy(reply, s_pet_reply, (size_t)result);
        }
        portEXIT_CRITICAL(&s_lock);
    }
out:
    xSemaphoreGive(s_pet_call);
    return result;
}

void ecos_family_set_orbit_handler(ecos_orbit_handler_t handler)
{
    portENTER_CRITICAL(&s_lock);
    s_orbit_handler = handler;
    portEXIT_CRITICAL(&s_lock);
}

void ecos_family_set_stream_handler(ecos_stream_handler_t handler)
{
    portENTER_CRITICAL(&s_lock);
    s_stream_handler = handler;
    portEXIT_CRITICAL(&s_lock);
}

bool ecos_family_stream_active(void)
{
    portENTER_CRITICAL(&s_lock);
    bool active = s_stream.active;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

bool ecos_family_stream_open(const char *id)
{
    uint8_t mac[6];
    if (!parse_id(id, mac) || family_disabled() || s_stream_gate == NULL) return false;
    xSemaphoreTake(s_stream_gate, portMAX_DELAY);
    portENTER_CRITICAL(&s_lock);
    bool available = s_running && s_listening && !s_suspend && !s_stream.active;
    uint8_t channel = 0;
    for (size_t i = 0; i < sizeof(s_info) / sizeof(s_info[0]); ++i)
    {
        if (s_info[i].used && memcmp(s_info[i].mac, mac, 6) == 0) channel = s_info[i].channel;
    }
    portEXIT_CRITICAL(&s_lock);
    if (channel < 1 || channel > 13) channel = wifi_radio_channel();
    bool reserved = available && wifi_radio_channel_reserve("OA01", channel);
    portENTER_CRITICAL(&s_lock);
    bool opened = reserved && s_running && s_listening && !s_suspend && ecos_stream_open(&s_stream, mac);
    portEXIT_CRITICAL(&s_lock);
    if (reserved && !opened) wifi_radio_channel_release("OA01");
    xSemaphoreGive(s_stream_gate);
    return opened;
}

void ecos_family_stream_close(const char *id)
{
    uint8_t mac[6];
    if (!parse_id(id, mac) || s_stream_gate == NULL) return;
    xSemaphoreTake(s_stream_gate, portMAX_DELAY);
    portENTER_CRITICAL(&s_lock);
    bool owned = ecos_stream_matches(&s_stream, mac);
    ecos_stream_close(&s_stream, mac);
    portEXIT_CRITICAL(&s_lock);
    if (owned) wifi_radio_channel_release("OA01");
    xSemaphoreGive(s_stream_gate);
}

bool ecos_family_stream_send(const char *id, const uint8_t *data, size_t length)
{
    uint8_t mac[6];
    if (!parse_id(id, mac) || family_disabled()) return false;
    portENTER_CRITICAL(&s_lock);
    bool accepted = s_running && s_listening && !s_suspend && ecos_stream_put(&s_stream, mac, data, length);
    portEXIT_CRITICAL(&s_lock);
    return accepted;
}

int ecos_family_orbit(const char *id, const uint8_t *request, size_t length,
                      uint8_t *reply, size_t max, int timeout_ms)
{
    uint8_t mac[6];
    if (request == NULL || reply == NULL || length == 0 || length > ECOS_ORBIT_MAX ||
        max == 0 || timeout_ms <= 0 || timeout_ms > 30000 || !parse_id(id, mac) || family_disabled())
    {
        return -1;
    }
    uint64_t request_id;
    do
    {
        esp_fill_random(&request_id, sizeof(request_id));
    } while (request_id == 0);
    portENTER_CRITICAL(&s_lock);
    bool available = !s_orbit_calling && s_cmd == CMD_NONE && !s_busy && !s_suspend;
    if (available)
    {
        s_orbit_calling = true;
        s_orbit_waiting = true;
        s_orbit_result = -1;
        s_orbit_request_id = request_id;
        memcpy(s_orbit_request, request, length);
        s_orbit_length = length;
        memcpy(s_orbit_mac, mac, 6);
        memcpy(s_cmd_mac, mac, 6);
        s_cmd = CMD_ORBIT;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!available)
    {
        return -1;
    }
    start_link();
    int result = -1;
    int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < until)
    {
        portENTER_CRITICAL(&s_lock);
        bool answered = !s_orbit_waiting && !s_orbit_processing;
        portEXIT_CRITICAL(&s_lock);
        if (answered)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    portENTER_CRITICAL(&s_lock);
    if (s_orbit_result > 0 && (size_t)s_orbit_result <= max)
    {
        result = s_orbit_result;
        memcpy(reply, s_orbit_reply, (size_t)result);
    }
    s_orbit_waiting = false;
    /* Cancel work not yet taken. A worker already in act() has a local copy;
     * s_busy prevents the next caller from overwriting its request metadata. */
    if (s_cmd == CMD_ORBIT)
    {
        s_cmd = CMD_NONE;
    }
    s_orbit_calling = false;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

bool ecos_family_ble_open(ecos_family_ble_send_t send, void *context, const uint8_t mac[6])
{
    if (send == NULL || context == NULL || mac == NULL || family_disabled())
    {
        return false;
    }

    uint8_t store[ECOS_STORE_SIZE];
    if (!ecos_store_load(store))
    {
        return false;
    }
    bool paired = false;
    for (unsigned i = 0; i < store[1] && i < ECOS_PEERS_MAX; ++i)
    {
        const uint8_t *record = store + 2 + i * ECOS_RECORD_SIZE;
        if (memcmp(record, mac, 6) == 0)
        {
            paired = true;
            break;
        }
    }
    mbedtls_platform_zeroize(store, sizeof(store));
    if (!paired)
    {
        return false;
    }

    uint8_t local_mac[6];
    if (esp_read_mac(local_mac, ESP_MAC_WIFI_STA) != ESP_OK)
    {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    bool reserved = !s_ble_link.active && !s_ble_link.closing;
    if (reserved)
    {
        memcpy(s_ble_link.mac, mac, 6);
        memcpy(s_ble_link.local_mac, local_mac, 6);
        s_ble_link.send = send;
        s_ble_link.context = context;
        s_ble_link.users = 0;
        s_ble_link.active = true;
        s_ble_link.hold = true;
        ++s_holds;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!reserved)
    {
        return false;
    }

    start_link();
    portENTER_CRITICAL(&s_lock);
    bool started = s_running && s_ble_link.active && s_ble_link.context == context && !s_suspend;
    portEXIT_CRITICAL(&s_lock);
    if (!started || family_disabled())
    {
        ecos_family_ble_close(context);
        return false;
    }
    return true;
}

bool ecos_family_ble_receive(const uint8_t *frame, size_t length, void *context)
{
    if (context == NULL || frame == NULL || !ecos_link_valid(frame, length))
    {
        return false;
    }
    uint8_t peer_mac[6], local_mac[6];
    portENTER_CRITICAL(&s_lock);
    bool matched = s_ble_link.active && s_ble_link.context == context;
    if (matched)
    {
        ++s_ble_link.users;
        memcpy(peer_mac, s_ble_link.mac, 6);
        memcpy(local_mac, s_ble_link.local_mac, 6);
    }
    portEXIT_CRITICAL(&s_lock);
    if (!matched)
    {
        return false;
    }

    bool valid = memcmp(frame + 5, peer_mac, 6) == 0 && memcmp(frame + 11, local_mac, 6) == 0;
    bool queued = valid && queue_frame(peer_mac, 0, frame, length);
    portENTER_CRITICAL(&s_lock);
    --s_ble_link.users;
    portEXIT_CRITICAL(&s_lock);
    return queued;
}

bool ecos_family_companion(char id[13])
{
    int64_t now = esp_timer_get_time();
    uint8_t peer_mac[6];
    bool available = false;
    portENTER_CRITICAL(&s_lock);
    if (s_ble_link.active)
    {
        for (size_t i = 0; i < sizeof(s_info) / sizeof(s_info[0]); ++i)
        {
            const member_info_t *info = &s_info[i];
            if (info->used && memcmp(info->mac, s_ble_link.mac, sizeof(peer_mac)) == 0 && info->known &&
                info->reachable && info->status_seen_at != 0 && now >= info->status_seen_at &&
                now - info->status_seen_at <= 30000000 &&
                (info->status.flags & ECOS_LINK_F_COMPANION) != 0)
            {
                memcpy(peer_mac, info->mac, sizeof(peer_mac));
                available = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (available && id != NULL)
    {
        id_text(peer_mac, id);
    }
    return available;
}

void ecos_family_ble_close(void *context)
{
    if (context == NULL)
    {
        return;
    }
    bool owner = false;
    bool release_hold = false;
    portENTER_CRITICAL(&s_lock);
    if (s_ble_link.context == context && s_ble_link.active)
    {
        owner = true;
        s_ble_link.active = false;
        s_ble_link.closing = true;
        release_hold = s_ble_link.hold;
        s_ble_link.hold = false;
        for (size_t i = 0; i < sizeof(s_info) / sizeof(s_info[0]); ++i)
        {
            if (s_info[i].used && memcmp(s_info[i].mac, s_ble_link.mac, 6) == 0)
            {
                s_info[i].reachable = false;
                break;
            }
        }
    }
    else if (s_ble_link.context != context || !s_ble_link.closing)
    {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    portEXIT_CRITICAL(&s_lock);

    if (release_hold)
    {
        ecos_family_hold(false);
    }
    for (;;)
    {
        portENTER_CRITICAL(&s_lock);
        bool same = s_ble_link.context == context;
        unsigned users = same ? s_ble_link.users : 0;
        if (owner && same && users == 0 && s_ble_link.closing)
        {
            memset(&s_ble_link, 0, sizeof(s_ble_link));
            same = false;
        }
        portEXIT_CRITICAL(&s_lock);
        if (!same || (!owner && users == 0))
        {
            return;
        }
        vTaskDelay(1);
    }
}
