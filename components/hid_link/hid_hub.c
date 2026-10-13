/*
 * Remote Hub transport (see include/hid_link/hid_hub.h).
 *
 * Radio: ESP-NOW on a fixed channel, without ESP-NOW encryption (no shared PMK
 * to get wrong). Every data frame is sealed here with AES-128-GCM:
 *
 *   0..1 "RH"  2 version 2  3 kind  4..15 random nonce  16..39 sealed packet  40..55 tag
 *   AAD = bytes 0..15 + sender MAC + receiver MAC (binds direction and peers)
 *
 * The 24-byte inner packet is the hub_state format (remote/hub/README.md):
 * sessions, sequences, merging and per-node release live there, tested on host.
 *
 * Pairing frames are plain (they carry only public values):
 *   BEACON hub->broadcast, JOIN satellite->hub: name, P-256 public key, nonce
 *   CONFIRM: HMAC tag proving the same ECDH secret, sent after the user confirmed
 * Both watches show a 6-digit code derived from the secret and the transcript:
 * matching codes rule out a man in the middle.
 *
 * Threads: the Wi-Fi callback only queues frames. One worker owns hub_state,
 * pairing and decryption. hid_tx (the router) and a heartbeat task send.
 */
#include "hid_link/hid_hub.h"
#include "hid_private.h"
#include "hub_state.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "hid_hub";

#define CHANNEL 1
#define FRAME_SIZE (16 + HUB_PACKET_SIZE + 16)
#define PUBLIC_KEY 65
#define PAIR_SIZE (4 + HID_HUB_NAME + PUBLIC_KEY + 16)
#define CONFIRM_SIZE (4 + 16)
#define RX_MAX 120
#define KIND_DATA 1
#define KIND_BEACON 0x80
#define KIND_JOIN 0x81
#define KIND_CONFIRM 0x82
#define HELLO 0x10
#define ACK 0x20
#define ACK_WAIT_MS 30
#define PAIR_WINDOW_MS 90000
#define FOUND_MAX 6

typedef struct
{
    uint8_t used;
    uint8_t mac[6];
    uint8_t key[16];
    char name[HID_HUB_NAME];
} peer_t;

typedef struct
{
    uint8_t schema;
    uint8_t role;
    uint8_t reserved[2];
    peer_t peers[HID_HUB_PEERS];
} config_t;

typedef struct
{
    uint8_t mac[6];
    int8_t rssi;
    uint8_t length;
    uint8_t data[RX_MAX];
} rx_t;

typedef struct
{
    unsigned node;
    uint8_t packet[HUB_PACKET_SIZE];
} local_t;

typedef struct
{
    uint8_t mac[6];
    char name[HID_HUB_NAME];
    int8_t rssi;
    uint8_t public_key[PUBLIC_KEY];
    uint8_t nonce[16];
    int64_t seen;
} found_t;

typedef struct
{
    hid_hub_pair_phase_t phase;
    int64_t deadline, next_send;
    psa_key_id_t ephemeral;
    uint8_t public_key[PUBLIC_KEY], nonce[16];
    found_t found[FOUND_MAX];
    int found_count;
    uint8_t peer_mac[6];
    char peer_name[HID_HUB_NAME];
    uint8_t peer_public[PUBLIC_KEY], peer_nonce[16];
    uint8_t key[16], confirm_key[16], transcript[32];
    uint32_t code;
    int select; /* satellite: index + 1 of the hub chosen in the UI, handled by the worker */
    bool local_ok, remote_ok;
    char message[64];
} pairing_t;

static config_t s_config;
static char s_name[HID_HUB_NAME];
static uint8_t s_mac[6];
static psa_key_id_t s_keys[HID_HUB_PEERS];
static hub_state_t s_state;
static pairing_t s_pair;
static SemaphoreHandle_t s_lock;   /* config, pairing and peer list for the UI */
static SemaphoreHandle_t s_sender; /* one transaction at a time (session, sequence) */
static QueueHandle_t s_rx, s_local, s_ack;
static esp_pm_lock_handle_t s_power;
static atomic_bool s_started;
static atomic_uint s_received, s_retries, s_dropped, s_rejected, s_connected_mask;
static uint64_t s_nonce[HUB_NODES]; /* worker-owned HELLO nonces */
static uint64_t s_boot_nonce, s_session;
static uint32_t s_sequence, s_sender_epoch;
static bool s_pending;
static uint8_t s_pending_packet[HUB_PACKET_SIZE];
static int8_t s_rssi[HID_HUB_PEERS];

static const uint8_t s_broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint64_t read_le(const uint8_t *data, unsigned length)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < length; i++)
        value |= (uint64_t)data[i] << (8 * i);
    return value;
}

static void write_le(uint8_t *data, unsigned length, uint64_t value)
{
    for (unsigned i = 0; i < length; i++)
        data[i] = (uint8_t)(value >> (8 * i));
}

static uint64_t random_session(void)
{
    uint64_t value;
    do
    {
        esp_fill_random(&value, sizeof(value));
    } while (value == 0);
    return value;
}

static bool is_hub(void) { return s_config.role == HID_HUB_HUB; }

/* ------------------------------------------------------------ config */

static void config_load(void)
{
    memset(&s_config, 0, sizeof(s_config));
    s_config.schema = 2;
    nvs_handle_t handle;
    if (nvs_open("remote_hub", NVS_READONLY, &handle) != ESP_OK)
        return;
    config_t saved;
    size_t size = sizeof(saved);
    if (nvs_get_blob(handle, "config2", &saved, &size) == ESP_OK && size == sizeof(saved) && saved.schema == 2 &&
        saved.role <= HID_HUB_SATELLITE)
    {
        s_config = saved;
        for (unsigned i = 0; i < HID_HUB_PEERS; i++)
            s_config.peers[i].name[HID_HUB_NAME - 1] = 0;
    }
    nvs_close(handle);
}

static esp_err_t config_save(const config_t *config)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("remote_hub", NVS_READWRITE, &handle);
    if (error != ESP_OK)
        return error;
    error = nvs_set_blob(handle, "config2", config, sizeof(*config));
    if (error == ESP_OK)
        error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

hid_hub_role_t hid_hub_role(void)
{
    if (!atomic_load(&s_started))
        config_load();
    return (hid_hub_role_t)s_config.role;
}

esp_err_t hid_hub_set_role(hid_hub_role_t role)
{
    if (role > HID_HUB_SATELLITE)
        return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&s_started))
        config_load();
    config_t next = s_config;
    if (next.role != role)
        memset(next.peers, 0, sizeof(next.peers)); /* pairs belong to a role */
    next.role = role;
    esp_err_t error = config_save(&next);
    memset(&next, 0, sizeof(next));
    return error; /* effective after an app restart */
}

/* ------------------------------------------------------------ crypto */

static psa_key_id_t import_aes(const uint8_t key[16])
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_key_id_t id = 0;
    if (psa_import_key(&attributes, key, 16, &id) != PSA_SUCCESS)
        id = 0;
    psa_reset_key_attributes(&attributes);
    return id;
}

static void aad(uint8_t out[28], const uint8_t *header, const uint8_t *from, const uint8_t *to)
{
    memcpy(out, header, 16);
    memcpy(out + 16, from, 6);
    memcpy(out + 22, to, 6);
}

static bool seal(unsigned peer, const uint8_t inner[HUB_PACKET_SIZE], uint8_t frame[FRAME_SIZE])
{
    if (s_keys[peer] == 0)
        return false;
    frame[0] = 'R';
    frame[1] = 'H';
    frame[2] = 2;
    frame[3] = KIND_DATA;
    esp_fill_random(frame + 4, 12);
    uint8_t data[28];
    aad(data, frame, s_mac, s_config.peers[peer].mac);
    size_t length = 0;
    return psa_aead_encrypt(s_keys[peer], PSA_ALG_GCM, frame + 4, 12, data, sizeof(data), inner, HUB_PACKET_SIZE,
                            frame + 16, FRAME_SIZE - 16, &length) == PSA_SUCCESS &&
           length == FRAME_SIZE - 16;
}

static bool open_frame(unsigned peer, const uint8_t *frame, uint8_t inner[HUB_PACKET_SIZE])
{
    if (s_keys[peer] == 0)
        return false;
    uint8_t data[28];
    aad(data, frame, s_config.peers[peer].mac, s_mac);
    size_t length = 0;
    return psa_aead_decrypt(s_keys[peer], PSA_ALG_GCM, frame + 4, 12, data, sizeof(data), frame + 16, FRAME_SIZE - 16,
                            inner, HUB_PACKET_SIZE, &length) == PSA_SUCCESS &&
           length == HUB_PACKET_SIZE;
}

static bool hmac(const uint8_t *key, size_t key_length, const uint8_t *message, size_t length, uint8_t out[32])
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, key_length * 8);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id = 0;
    size_t written = 0;
    bool ok = psa_import_key(&attributes, key, key_length, &id) == PSA_SUCCESS &&
              psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), message, length, out, 32, &written) == PSA_SUCCESS &&
              written == 32;
    psa_reset_key_attributes(&attributes);
    if (id)
        psa_destroy_key(id);
    return ok;
}

static bool confirm_tag(bool from_hub, uint8_t tag[32])
{
    uint8_t message[3 + 32];
    memcpy(message, from_hub ? "HUB" : "SAT", 3);
    memcpy(message + 3, s_pair.transcript, 32);
    return hmac(s_pair.confirm_key, 16, message, sizeof(message), tag);
}

/* Shared key, confirmation key and 6-digit code from ECDH and the transcript. */
static bool derive_pair(void)
{
    uint8_t secret[32], material[32], transcript[8 + 12 + 2 * PUBLIC_KEY + 32];
    size_t length = 0;
    if (psa_raw_key_agreement(PSA_ALG_ECDH, s_pair.ephemeral, s_pair.peer_public, PUBLIC_KEY, secret, sizeof(secret),
                              &length) != PSA_SUCCESS ||
        length != 32)
        return false;
    bool hub = is_hub();
    const uint8_t *hub_mac = hub ? s_mac : s_pair.peer_mac, *sat_mac = hub ? s_pair.peer_mac : s_mac;
    const uint8_t *hub_public = hub ? s_pair.public_key : s_pair.peer_public;
    const uint8_t *sat_public = hub ? s_pair.peer_public : s_pair.public_key;
    const uint8_t *hub_nonce = hub ? s_pair.nonce : s_pair.peer_nonce, *sat_nonce = hub ? s_pair.peer_nonce : s_pair.nonce;
    uint8_t *p = transcript;
    memcpy(p, "RH-PAIR2", 8); p += 8;
    memcpy(p, hub_mac, 6); p += 6;
    memcpy(p, sat_mac, 6); p += 6;
    memcpy(p, hub_public, PUBLIC_KEY); p += PUBLIC_KEY;
    memcpy(p, sat_public, PUBLIC_KEY); p += PUBLIC_KEY;
    memcpy(p, hub_nonce, 16); p += 16;
    memcpy(p, sat_nonce, 16);
    size_t hashed = 0;
    bool ok = psa_hash_compute(PSA_ALG_SHA_256, transcript, sizeof(transcript), s_pair.transcript, 32, &hashed) ==
                  PSA_SUCCESS &&
              hmac(secret, 32, s_pair.transcript, 32, material);
    if (ok)
    {
        memcpy(s_pair.key, material, 16);
        memcpy(s_pair.confirm_key, material + 16, 16);
        uint8_t message[3 + 32], code[32];
        memcpy(message, "SAS", 3);
        memcpy(message + 3, s_pair.transcript, 32);
        ok = hmac(s_pair.confirm_key, 16, message, sizeof(message), code);
        s_pair.code = ((uint32_t)code[0] << 24 | (uint32_t)code[1] << 16 | (uint32_t)code[2] << 8 | code[3]) % 1000000;
        memset(code, 0, sizeof(code));
    }
    memset(secret, 0, sizeof(secret));
    memset(material, 0, sizeof(material));
    return ok;
}

/* ------------------------------------------------------------ radio */

static void receive_frame(const esp_now_recv_info_t *info, const uint8_t *data, int length)
{
    if (!atomic_load(&s_started) || info == NULL || data == NULL || length < 4 || length > RX_MAX ||
        data[0] != 'R' || (data[1] != 'H' && data[1] != 'P'))
        return;
    rx_t frame = {.length = (uint8_t)length, .rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0};
    memcpy(frame.mac, info->src_addr, 6);
    memcpy(frame.data, data, length);
    if (xQueueSend(s_rx, &frame, 0) != pdTRUE)
        atomic_fetch_add(&s_dropped, 1);
}

static void add_radio_peer(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac))
        return;
    esp_now_peer_info_t peer = {.channel = CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false};
    memcpy(peer.peer_addr, mac, 6);
    esp_now_add_peer(&peer);
}

static int peer_index(const uint8_t mac[6])
{
    for (int i = 0; i < HID_HUB_PEERS; i++)
        if (s_config.peers[i].used && memcmp(s_config.peers[i].mac, mac, 6) == 0)
            return i;
    return -1;
}

/* ------------------------------------------------------------ sender (satellite radio / hub local) */

static void respond(unsigned node, const uint8_t reply[HUB_PACKET_SIZE])
{
    if (node == 0)
    {
        if (xQueueSend(s_ack, reply, 0) != pdTRUE)
            atomic_fetch_add(&s_dropped, 1);
        return;
    }
    uint8_t frame[FRAME_SIZE];
    if (seal(node - 1, reply, frame))
        esp_now_send(s_config.peers[node - 1].mac, frame, FRAME_SIZE);
}

/* Owner of s_sender: send one request and wait for its matching reply. */
static bool exchange(const uint8_t *packet, uint8_t *reply)
{
    for (unsigned attempt = 0; attempt < 3; attempt++)
    {
        if (attempt)
            atomic_fetch_add(&s_retries, 1);
        if (is_hub())
        {
            local_t event = {.node = 0};
            memcpy(event.packet, packet, HUB_PACKET_SIZE);
            if (xQueueSend(s_local, &event, 0) != pdTRUE)
                atomic_fetch_add(&s_dropped, 1);
        }
        else
        {
            uint8_t frame[FRAME_SIZE];
            if (!s_config.peers[0].used || !seal(0, packet, frame) ||
                esp_now_send(s_config.peers[0].mac, frame, FRAME_SIZE) != ESP_OK)
            {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }
        uint32_t deadline = now_ms() + ACK_WAIT_MS;
        do
        {
            if (xQueueReceive(s_ack, reply, pdMS_TO_TICKS(2)) != pdTRUE)
                continue;
            if (reply[0] != 'R' || reply[1] != 'A' || reply[2] != 1)
                continue;
            if (packet[3] == HELLO)
            {
                if (reply[3] == HELLO && read_le(reply + 16, 8) == s_boot_nonce)
                    return read_le(reply + 8, 8) != 0;
            }
            else if (reply[3] == (uint8_t)(ACK | packet[3]) && memcmp(reply + 4, packet + 4, 12) == 0)
            {
                if (reply[16] != 0)
                {
                    s_session = 0;
                    s_pending = false;
                    if (!is_hub())
                        hid_link_satellite_state(HID_LINK_WAITING);
                    return false;
                }
                return true;
            }
        } while ((int32_t)(deadline - now_ms()) > 0);
    }
    return false;
}

static bool handshake(void)
{
    /* A new handshake never restarts sequence zero inside an old session. */
    s_boot_nonce = random_session();
    uint8_t hello[HUB_PACKET_SIZE] = {'R', 'H', 1, HELLO};
    uint8_t reply[HUB_PACKET_SIZE];
    write_le(hello + 8, 8, s_boot_nonce);
    if (!exchange(hello, reply))
        return false;
    s_session = read_le(reply + 8, 8);
    s_sequence = 0;
    s_pending = false;
    if (!is_hub())
        hid_link_satellite_state(HID_LINK_CONNECTED);
    return true;
}

/* hid_link router: every report of this watch. */
static esp_err_t forward(uint8_t id, const uint8_t *data, uint16_t length)
{
    if (!atomic_load(&s_started) || id < 1 || id > 4 || data == NULL ||
        length != (id == 1 ? 8 : id == 2 ? 2 : id == 3 ? 5 : 4))
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_sender, pdMS_TO_TICKS(80)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (is_hub() && s_sender_epoch != hid_link_version())
    {
        s_sender_epoch = hid_link_version();
        s_session = 0;
        s_pending = false;
    }
    if (s_pending && (s_pending_packet[3] != id || memcmp(s_pending_packet + 16, data, length) != 0))
    {
        /* A failed transaction superseded by a newer report: re-key the session
         * first so delayed copies cannot recreate its held controls. */
        s_session = 0;
        s_pending = false;
    }
    esp_err_t error = ESP_ERR_TIMEOUT;
    if (s_session != 0 || handshake())
    {
        if (!s_pending)
        {
            memset(s_pending_packet, 0, sizeof(s_pending_packet));
            s_pending_packet[0] = 'R';
            s_pending_packet[1] = 'H';
            s_pending_packet[2] = 1;
            s_pending_packet[3] = id;
            write_le(s_pending_packet + 4, 4, ++s_sequence);
            write_le(s_pending_packet + 8, 8, s_session);
            memcpy(s_pending_packet + 16, data, length);
            s_pending = true;
        }
        uint8_t reply[HUB_PACKET_SIZE];
        if (exchange(s_pending_packet, reply))
        {
            s_pending = false;
            error = ESP_OK;
        }
    }
    xSemaphoreGive(s_sender);
    return error;
}

/* Satellite: keep the session alive (and find the hub again after a loss). */
static void heartbeat_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(150));
        if (!s_config.peers[0].used || xSemaphoreTake(s_sender, 0) != pdTRUE)
            continue;
        if (s_session == 0)
            handshake();
        else if (!s_pending)
        {
            uint8_t beat[HUB_PACKET_SIZE] = {'R', 'H', 1, 0};
            uint8_t reply[HUB_PACKET_SIZE];
            write_le(beat + 4, 4, ++s_sequence);
            write_le(beat + 8, 8, s_session);
            if (!exchange(beat, reply))
            {
                s_session = 0;
                hid_link_satellite_state(HID_LINK_WAITING);
            }
        }
        xSemaphoreGive(s_sender);
    }
}

/* ------------------------------------------------------------ hub merge */

static bool flush_host(void)
{
    /* Never consume another input until this state transition reached the host. */
    for (uint8_t id = 1; id <= 4; id++)
    {
        hub_report_t report;
        unsigned chunks = 0;
        while (hub_state_snapshot(&s_state, id, &report))
        {
            uint32_t deadline = now_ms() + 30;
            esp_err_t error;
            do
            {
                error = hid_link_transport_send(id, report.data, report.length);
                if (error != ESP_OK)
                    vTaskDelay(pdMS_TO_TICKS(2));
            } while (error != ESP_OK && hid_link_state() == HID_LINK_CONNECTED && (int32_t)(deadline - now_ms()) > 0);
            if (error != ESP_OK || !hub_state_commit(&s_state, &report) || ++chunks > 4)
                return false;
            vTaskDelay(pdMS_TO_TICKS(hid_link_transport() == HID_LINK_BLE ? 8 : 1));
        }
    }
    return true;
}

static void process_input(unsigned node, const uint8_t *packet)
{
    if (packet[0] != 'R' || packet[1] != 'H' || packet[2] != 1 || node >= HUB_NODES)
        return;
    uint8_t reply[HUB_PACKET_SIZE] = {'R', 'A', 1};
    memcpy(reply + 4, packet + 4, 12);
    if (packet[3] == HELLO)
    {
        uint64_t nonce = read_le(packet + 8, 8);
        if (nonce == 0 || read_le(packet + 4, 4) != 0 || read_le(packet + 16, 8) != 0)
            return;
        reply[3] = HELLO;
        write_le(reply + 16, 8, nonce);
        uint64_t session = 0;
        if (hid_link_state() == HID_LINK_CONNECTED)
        {
            if (s_nonce[node] != nonce || !s_state.nodes[node].authorized)
            {
                s_nonce[node] = nonce;
                hub_state_authorize(&s_state, node, random_session());
            }
            if (flush_host())
                session = s_state.nodes[node].session;
            else
                hub_state_revoke(&s_state, node);
        }
        write_le(reply + 8, 8, session);
    }
    else
    {
        if (packet[3] > 4)
            return;
        reply[3] = ACK | packet[3];
        hub_result_t result = hid_link_state() == HID_LINK_CONNECTED
                                  ? hub_state_receive(&s_state, node, packet, HUB_PACKET_SIZE, now_ms())
                                  : HUB_UNAUTHORIZED;
        bool accepted = (result == HUB_ACCEPTED || result == HUB_DUPLICATE) && flush_host();
        reply[16] = accepted ? 0 : 1;
        if (!accepted)
            hub_state_revoke(&s_state, node);
    }
    atomic_fetch_add(&s_received, 1);
    respond(node, reply);
}

/* ------------------------------------------------------------ pairing (worker) */

static void pair_fail(const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pair.phase = HID_HUB_PAIR_FAILED;
    snprintf(s_pair.message, sizeof(s_pair.message), "%s", message);
    memset(s_pair.key, 0, sizeof(s_pair.key));
    memset(s_pair.confirm_key, 0, sizeof(s_pair.confirm_key));
    xSemaphoreGive(s_lock);
    if (s_pair.ephemeral)
    {
        psa_destroy_key(s_pair.ephemeral);
        s_pair.ephemeral = 0;
    }
}

static void pair_frame(uint8_t kind, uint8_t *frame)
{
    frame[0] = 'R';
    frame[1] = 'P';
    frame[2] = 2;
    frame[3] = kind;
    memcpy(frame + 4, s_name, HID_HUB_NAME);
    memcpy(frame + 4 + HID_HUB_NAME, s_pair.public_key, PUBLIC_KEY);
    memcpy(frame + 4 + HID_HUB_NAME + PUBLIC_KEY, s_pair.nonce, 16);
}

static void send_confirm(void)
{
    uint8_t frame[CONFIRM_SIZE] = {'R', 'P', 2, KIND_CONFIRM}, tag[32];
    if (!confirm_tag(is_hub(), tag))
        return;
    memcpy(frame + 4, tag, 16);
    esp_now_send(s_pair.peer_mac, frame, sizeof(frame));
    memset(tag, 0, sizeof(tag));
}

/* Both sides confirmed: store the pair and start using it. */
static void pair_complete(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    config_t next = s_config;
    int slot = peer_index(s_pair.peer_mac);
    if (!is_hub())
        slot = 0;
    for (int i = 0; slot < 0 && i < HID_HUB_PEERS; i++)
        if (!next.peers[i].used)
            slot = i;
    esp_err_t error = ESP_ERR_NO_MEM;
    if (slot >= 0)
    {
        peer_t *peer = &next.peers[slot];
        memset(peer, 0, sizeof(*peer));
        peer->used = 1;
        memcpy(peer->mac, s_pair.peer_mac, 6);
        memcpy(peer->key, s_pair.key, 16);
        memcpy(peer->name, s_pair.peer_name, HID_HUB_NAME);
        peer->name[HID_HUB_NAME - 1] = 0;
        error = config_save(&next);
    }
    if (error == ESP_OK)
    {
        if (s_keys[slot])
            psa_destroy_key(s_keys[slot]);
        s_config = next;
        s_keys[slot] = import_aes(s_config.peers[slot].key);
        add_radio_peer(s_config.peers[slot].mac);
        s_pair.phase = HID_HUB_PAIR_DONE;
        snprintf(s_pair.message, sizeof(s_pair.message), "Collegato a %s", s_pair.peer_name);
    }
    memset(&next, 0, sizeof(next));
    memset(s_pair.key, 0, sizeof(s_pair.key));
    xSemaphoreGive(s_lock);
    if (error != ESP_OK)
        pair_fail(slot < 0 ? "Hub pieno: rimuovi un dispositivo" : "Salvataggio non riuscito");
    else if (!is_hub())
    {
        /* New hub: start a fresh session with it. */
        xSemaphoreTake(s_sender, portMAX_DELAY);
        s_session = 0;
        s_pending = false;
        xSemaphoreGive(s_sender);
    }
}

static void pair_receive(const rx_t *rx)
{
    const uint8_t *d = rx->data;
    if (d[2] != 2)
        return;
    int64_t now = esp_timer_get_time();
    if ((d[3] == KIND_BEACON || d[3] == KIND_JOIN) && rx->length == PAIR_SIZE)
    {
        bool beacon = d[3] == KIND_BEACON;
        if (beacon && !is_hub() && s_pair.phase == HID_HUB_PAIR_OPEN)
        {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            int index = -1;
            for (int i = 0; i < s_pair.found_count; i++)
                if (memcmp(s_pair.found[i].mac, rx->mac, 6) == 0)
                    index = i;
            if (index < 0 && s_pair.found_count < FOUND_MAX)
                index = s_pair.found_count++;
            if (index >= 0)
            {
                found_t *f = &s_pair.found[index];
                memcpy(f->mac, rx->mac, 6);
                memcpy(f->name, d + 4, HID_HUB_NAME);
                f->name[HID_HUB_NAME - 1] = 0;
                memcpy(f->public_key, d + 4 + HID_HUB_NAME, PUBLIC_KEY);
                memcpy(f->nonce, d + 4 + HID_HUB_NAME + PUBLIC_KEY, 16);
                f->rssi = rx->rssi;
                f->seen = now;
            }
            xSemaphoreGive(s_lock);
        }
        else if (!beacon && is_hub() && s_pair.phase == HID_HUB_PAIR_OPEN)
        {
            memcpy(s_pair.peer_mac, rx->mac, 6);
            memcpy(s_pair.peer_name, d + 4, HID_HUB_NAME);
            s_pair.peer_name[HID_HUB_NAME - 1] = 0;
            memcpy(s_pair.peer_public, d + 4 + HID_HUB_NAME, PUBLIC_KEY);
            memcpy(s_pair.peer_nonce, d + 4 + HID_HUB_NAME + PUBLIC_KEY, 16);
            if (!derive_pair())
            {
                pair_fail("Chiave non valida");
                return;
            }
            add_radio_peer(rx->mac);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_pair.phase = HID_HUB_PAIR_CODE;
            s_pair.deadline = now + 60000000;
            snprintf(s_pair.message, sizeof(s_pair.message), "Confronta il codice con %s", s_pair.peer_name);
            xSemaphoreGive(s_lock);
        }
    }
    else if (d[3] == KIND_CONFIRM && rx->length == CONFIRM_SIZE && s_pair.phase == HID_HUB_PAIR_DONE &&
             memcmp(rx->mac, s_pair.peer_mac, 6) == 0)
    {
        send_confirm(); /* our last confirmation was lost: the other side is still waiting */
    }
    else if (d[3] == KIND_CONFIRM && rx->length == CONFIRM_SIZE &&
             (s_pair.phase == HID_HUB_PAIR_CODE || s_pair.phase == HID_HUB_PAIR_CONFIRMED) &&
             memcmp(rx->mac, s_pair.peer_mac, 6) == 0)
    {
        uint8_t expected[32];
        bool valid = confirm_tag(!is_hub(), expected);
        uint8_t difference = 0;
        for (int i = 0; i < 16; i++)
            difference |= expected[i] ^ d[4 + i];
        memset(expected, 0, sizeof(expected));
        if (!valid || difference)
        {
            pair_fail("Codice diverso: abbinamento annullato");
            return;
        }
        s_pair.remote_ok = true;
        if (s_pair.local_ok)
        {
            send_confirm(); /* the other side may have missed ours */
            pair_complete();
        }
    }
}

static void pair_select(int index)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = false;
    if (s_pair.phase == HID_HUB_PAIR_OPEN && index >= 0 && index < s_pair.found_count)
    {
        found_t *f = &s_pair.found[index];
        memcpy(s_pair.peer_mac, f->mac, 6);
        memcpy(s_pair.peer_name, f->name, HID_HUB_NAME);
        memcpy(s_pair.peer_public, f->public_key, PUBLIC_KEY);
        memcpy(s_pair.peer_nonce, f->nonce, 16);
        ok = derive_pair();
        if (ok)
        {
            add_radio_peer(f->mac);
            s_pair.phase = HID_HUB_PAIR_CODE;
            s_pair.next_send = 0;
            s_pair.deadline = esp_timer_get_time() + 60000000;
            snprintf(s_pair.message, sizeof(s_pair.message), "Confronta il codice con %s", s_pair.peer_name);
        }
    }
    xSemaphoreGive(s_lock);
    if (!ok)
        pair_fail("Hub non piu disponibile");
}

static void pair_tick(void)
{
    if (s_pair.select)
    {
        int index = s_pair.select - 1;
        s_pair.select = 0;
        pair_select(index);
    }
    int64_t now = esp_timer_get_time();
    hid_hub_pair_phase_t phase = s_pair.phase;
    if (phase == HID_HUB_PAIR_IDLE || phase == HID_HUB_PAIR_DONE || phase == HID_HUB_PAIR_FAILED)
        return;
    if (now > s_pair.deadline)
    {
        pair_fail(phase == HID_HUB_PAIR_OPEN ? "Nessun dispositivo trovato" : "Tempo scaduto");
        return;
    }
    if (now < s_pair.next_send)
        return;
    s_pair.next_send = now + (phase == HID_HUB_PAIR_OPEN ? 300000 : 500000);
    uint8_t frame[PAIR_SIZE];
    if (phase == HID_HUB_PAIR_OPEN && is_hub())
    {
        pair_frame(KIND_BEACON, frame);
        esp_now_send(s_broadcast, frame, sizeof(frame));
    }
    else if (phase == HID_HUB_PAIR_CODE && !is_hub())
    {
        pair_frame(KIND_JOIN, frame); /* until the hub shows the code too */
        esp_now_send(s_pair.peer_mac, frame, sizeof(frame));
    }
    else if (phase == HID_HUB_PAIR_CONFIRMED)
        send_confirm();
}

/* ------------------------------------------------------------ worker */

static void worker(void *arg)
{
    (void)arg;
    uint32_t epoch = UINT32_MAX;
    for (;;)
    {
        if (is_hub())
        {
            if (epoch != hid_link_version())
            {
                epoch = hid_link_version();
                hub_state_disconnect(&s_state);
                memset(s_nonce, 0, sizeof(s_nonce));
            }
            /* Local controls belong to this process: never time them out. */
            if (s_state.nodes[0].authorized && s_state.nodes[0].seen)
                s_state.nodes[0].last_ms = now_ms();
            hub_state_expire(&s_state, now_ms());
            if (hid_link_state() == HID_LINK_CONNECTED && !flush_host())
            {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            local_t local;
            while (xQueueReceive(s_local, &local, 0) == pdTRUE)
                process_input(0, local.packet);
            unsigned mask = 0;
            for (unsigned node = 1; node < HUB_NODES; node++)
                if (s_state.nodes[node].authorized && s_state.nodes[node].seen)
                    mask |= 1u << (node - 1);
            atomic_store(&s_connected_mask, mask);
        }
        pair_tick();
        rx_t rx;
        if (xQueueReceive(s_rx, &rx, pdMS_TO_TICKS(is_hub() ? 2 : 10)) != pdTRUE)
            continue;
        if (rx.data[1] == 'P')
        {
            pair_receive(&rx);
            continue;
        }
        int peer = peer_index(rx.mac);
        uint8_t inner[HUB_PACKET_SIZE];
        if (peer < 0 || rx.length != FRAME_SIZE || rx.data[2] != 2 || rx.data[3] != KIND_DATA ||
            !open_frame(peer, rx.data, inner))
        {
            atomic_fetch_add(&s_rejected, 1);
            continue;
        }
        s_rssi[peer] = rx.rssi;
        if (is_hub())
            process_input(peer + 1, inner);
        else if (peer == 0 && xQueueSend(s_ack, inner, 0) != pdTRUE)
            atomic_fetch_add(&s_dropped, 1);
    }
}

/* ------------------------------------------------------------ start */

esp_err_t hid_hub_start(const char *name)
{
    if (atomic_load(&s_started))
        return ESP_ERR_INVALID_STATE;
    config_load();
    if (s_config.role == HID_HUB_OFF)
        return ESP_ERR_NOT_SUPPORTED;
    snprintf(s_name, sizeof(s_name), "%s", name ? name : "Remote");
    s_lock = xSemaphoreCreateMutex();
    s_sender = xSemaphoreCreateMutex();
    s_rx = xQueueCreate(16, sizeof(rx_t));
    s_local = xQueueCreate(8, sizeof(local_t));
    s_ack = xQueueCreate(8, HUB_PACKET_SIZE);
    if (!s_lock || !s_sender || !s_rx || !s_local || !s_ack || psa_crypto_init() != PSA_SUCCESS)
        return ESP_ERR_NO_MEM;
    esp_err_t error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
        return error;
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    if ((error = esp_wifi_init(&wifi)) != ESP_OK || (error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (error = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK || (error = esp_wifi_start()) != ESP_OK ||
        (error = esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE)) != ESP_OK ||
        (error = esp_now_init()) != ESP_OK || (error = esp_now_register_recv_cb(receive_frame)) != ESP_OK)
    {
        ESP_LOGE(TAG, "Radio start: %s", esp_err_to_name(error));
        return error;
    }
    /* Modem sleep would miss frames; with Bluetooth the coexistence scheduler shares the radio. */
    esp_wifi_set_ps(hid_link_transport() == HID_LINK_BLE ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    esp_read_mac(s_mac, ESP_MAC_WIFI_STA);
    add_radio_peer(s_broadcast);
    for (int i = 0; i < HID_HUB_PEERS; i++)
        if (s_config.peers[i].used)
        {
            s_keys[i] = import_aes(s_config.peers[i].key);
            add_radio_peer(s_config.peers[i].mac);
        }
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "remote_hub", &s_power) == ESP_OK)
        esp_pm_lock_acquire(s_power);
    hub_state_init(&s_state);
    s_boot_nonce = random_session();
    s_sender_epoch = hid_link_version();
    atomic_store(&s_started, true);
    if (xTaskCreatePinnedToCore(worker, "hub", 5120, NULL, 5, NULL, 0) != pdPASS ||
        (!is_hub() && xTaskCreatePinnedToCore(heartbeat_task, "hub_beat", 3072, NULL, 4, NULL, 0) != pdPASS))
        return ESP_ERR_NO_MEM;
    hid_link_set_router(forward);
    ESP_LOGI(TAG, "%s on channel %d, %s", is_hub() ? "Hub" : "Satellite", CHANNEL,
             hid_link_transport() == HID_LINK_BLE ? "Bluetooth" : hid_link_transport() == HID_LINK_USB ? "USB" : "radio");
    return ESP_OK;
}

/* ------------------------------------------------------------ UI API */

int hid_hub_peers(hid_hub_peer_info_t *out, int max)
{
    if (!atomic_load(&s_started))
        config_load();
    int count = 0;
    unsigned mask = atomic_load(&s_connected_mask);
    if (s_lock)
        xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < HID_HUB_PEERS && count < max; i++)
    {
        if (!s_config.peers[i].used)
            continue;
        hid_hub_peer_info_t *info = &out[count++];
        memcpy(info->name, s_config.peers[i].name, HID_HUB_NAME);
        memcpy(info->mac, s_config.peers[i].mac, 6);
        info->rssi = s_rssi[i];
        info->connected = is_hub() ? (mask & (1u << i)) != 0 : hid_link_state() == HID_LINK_CONNECTED;
    }
    if (s_lock)
        xSemaphoreGive(s_lock);
    return count;
}

esp_err_t hid_hub_forget(int index)
{
    if (!atomic_load(&s_started))
        config_load();
    if (s_lock)
        xSemaphoreTake(s_lock, portMAX_DELAY);
    int seen = 0, slot = -1;
    for (int i = 0; i < HID_HUB_PEERS; i++)
        if (s_config.peers[i].used && seen++ == index)
            slot = i;
    esp_err_t error = ESP_ERR_NOT_FOUND;
    if (slot >= 0)
    {
        config_t next = s_config;
        memset(&next.peers[slot], 0, sizeof(peer_t));
        error = config_save(&next);
        if (error == ESP_OK)
        {
            if (atomic_load(&s_started))
                esp_now_del_peer(s_config.peers[slot].mac);
            s_config = next;
            if (s_keys[slot])
                psa_destroy_key(s_keys[slot]);
            s_keys[slot] = 0;
        }
        memset(&next, 0, sizeof(next));
    }
    if (s_lock)
        xSemaphoreGive(s_lock);
    return error;
}

esp_err_t hid_hub_pair_open(void)
{
    if (!atomic_load(&s_started))
        return ESP_ERR_INVALID_STATE;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pair.ephemeral)
        psa_destroy_key(s_pair.ephemeral);
    memset(&s_pair, 0, sizeof(s_pair));
    size_t length = 0;
    esp_err_t error = ESP_FAIL;
    if (psa_generate_key(&attributes, &s_pair.ephemeral) == PSA_SUCCESS &&
        psa_export_public_key(s_pair.ephemeral, s_pair.public_key, PUBLIC_KEY, &length) == PSA_SUCCESS &&
        length == PUBLIC_KEY)
    {
        esp_fill_random(s_pair.nonce, sizeof(s_pair.nonce));
        s_pair.deadline = esp_timer_get_time() + (int64_t)PAIR_WINDOW_MS * 1000;
        s_pair.phase = HID_HUB_PAIR_OPEN;
        snprintf(s_pair.message, sizeof(s_pair.message), "%s",
                 is_hub() ? "Sul satellite: Remote > Hub > Cerca hub" : "Cerco hub vicini...");
        error = ESP_OK;
    }
    psa_reset_key_attributes(&attributes);
    xSemaphoreGive(s_lock);
    return error;
}

int hid_hub_pair_found(hid_hub_peer_info_t *out, int max)
{
    if (!atomic_load(&s_started))
        return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int count = 0;
    for (int i = 0; i < s_pair.found_count && count < max; i++)
    {
        memcpy(out[count].name, s_pair.found[i].name, HID_HUB_NAME);
        memcpy(out[count].mac, s_pair.found[i].mac, 6);
        out[count].rssi = s_pair.found[i].rssi;
        out[count].connected = false;
        count++;
    }
    xSemaphoreGive(s_lock);
    return count;
}

esp_err_t hid_hub_pair_select(int index)
{
    if (!atomic_load(&s_started) || is_hub())
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_pair.phase == HID_HUB_PAIR_OPEN && index >= 0 && index < s_pair.found_count)
    {
        s_pair.select = index + 1; /* ECDH runs in the worker, never in the UI task */
        snprintf(s_pair.message, sizeof(s_pair.message), "Collegamento a %s...", s_pair.found[index].name);
        error = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return error;
}

void hid_hub_pair_confirm(bool accept)
{
    if (!atomic_load(&s_started))
        return;
    if (!accept)
    {
        pair_fail("Abbinamento rifiutato");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_pair.phase == HID_HUB_PAIR_CODE;
    if (ready)
    {
        s_pair.local_ok = true;
        s_pair.phase = HID_HUB_PAIR_CONFIRMED;
        s_pair.next_send = 0;
        snprintf(s_pair.message, sizeof(s_pair.message), "Conferma anche su %s", s_pair.peer_name);
    }
    xSemaphoreGive(s_lock);
    /* The worker sends CONFIRM and completes when the other side's tag arrives;
     * if it already arrived, finish here (pair_complete takes the locks it needs). */
    if (ready && s_pair.remote_ok)
    {
        send_confirm();
        pair_complete();
    }
}

void hid_hub_pair_cancel(void)
{
    if (!atomic_load(&s_started))
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_pair.phase != HID_HUB_PAIR_IDLE && s_pair.phase != HID_HUB_PAIR_DONE;
    xSemaphoreGive(s_lock);
    if (active)
        pair_fail("Annullato");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pair.phase = HID_HUB_PAIR_IDLE;
    xSemaphoreGive(s_lock);
}

void hid_hub_pairing(hid_hub_pairing_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!atomic_load(&s_started))
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->phase = s_pair.phase;
    memcpy(out->peer, s_pair.peer_name, HID_HUB_NAME);
    out->code = s_pair.phase == HID_HUB_PAIR_CODE || s_pair.phase == HID_HUB_PAIR_CONFIRMED ? s_pair.code : 0;
    int64_t left = s_pair.deadline - esp_timer_get_time();
    out->seconds_left = left > 0 ? (int)(left / 1000000) : 0;
    snprintf(out->message, sizeof(out->message), "%s", s_pair.message);
    xSemaphoreGive(s_lock);
}

void hid_hub_stats(hid_hub_stats_t *stats)
{
    stats->received = atomic_load(&s_received);
    stats->retries = atomic_load(&s_retries);
    stats->dropped = atomic_load(&s_dropped);
    stats->rejected = atomic_load(&s_rejected);
}
