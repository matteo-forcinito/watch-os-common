/* Pairing, two transports for the same protocol and frames:
 *
 *   ESP-NOW (default): device to device, no router, no internet. The radio is
 *   switched on without joining a network; both sides meet on channel 1, or on
 *   the channel of the network one of them is already joined to (the free side
 *   hops channels until it hears the other, then stays).
 *   LAN (an IPv4 given in the dashboard): UDP over the home network, as before.
 *
 * Only an explicit start holds the radio, for at most 180 seconds. The
 * ephemeral session and peer secrets live in PSRAM, wiped before release. */
#include "services/ecos.h"
#include "ecos_wire.h"
#include "services/wifi.h"
#include "core/sys.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "mbedtls/platform_util.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define WINDOW_US (180LL * 1000000)
#define RECORD_SIZE (6 + ECOS_NAME_SIZE + 32)
#define STORE_SIZE (2 + ECOS_PEERS_MAX * RECORD_SIZE)
#define FIRST_DWELL_US (4LL * 1000000)    /* ESP-NOW: wait on channel 1 first */
#define HOP_DWELL_US (700LL * 1000)       /* then visit the channels */
#define PAIRING_TX_POWER 32               /* 0.25 dBm units: 8 dBm */

/* ESP-NOW receive: the Wi-Fi task only copies the frame into this queue. */
typedef struct
{
    uint8_t mac[6];
    uint8_t length;
    uint8_t data[ECOS_FRAME_MAX];
} radio_frame_t;

static QueueHandle_t s_rx;
static const uint8_t s_broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t s_hops[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};

static void on_radio(const esp_now_recv_info_t *info, const uint8_t *data, int length)
{
    QueueHandle_t rx = s_rx;
    if (rx == NULL || info == NULL || length <= 0 || length > ECOS_FRAME_MAX)
    {
        return;
    }
    radio_frame_t frame;
    memcpy(frame.mac, info->src_addr, 6);
    frame.length = (uint8_t)length;
    memcpy(frame.data, data, (size_t)length);
    xQueueSend(rx, &frame, 0);   /* full: dropped, the sender repeats */
}

typedef struct
{
    uint8_t mac[6], peer[6];
    uint8_t hello[ECOS_HELLO_SIZE], other[ECOS_HELLO_SIZE];
    uint8_t transcript[32], key[32];
    uint8_t commitment[32], peer_commitment[32];
    uint8_t store[STORE_SIZE];
    psa_key_id_t ephemeral;
    struct sockaddr_in destination;
    bool espnow;                 /* the transport of this window */
    bool committed, selected, local_ok, remote_ok, saved, remote_saved;
    int64_t deadline, done_at;
    ecos_snapshot_t view;
} session_t;

static const char *TAG = "ecos";
static portMUX_TYPE s_guard = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR ecos_snapshot_t s_view;
static bool s_busy, s_cancel, s_confirm;

static void id_text(const uint8_t mac[6], char out[13])
{
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool load_store(uint8_t store[STORE_SIZE])
{
    memset(store, 0, STORE_SIZE);
    store[0] = 1;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("ecos", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return true;
    }
    if (err != ESP_OK)
    {
        return false;
    }
    size_t size = STORE_SIZE;
    err = nvs_get_blob(handle, "peers", store, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        memset(store, 0, STORE_SIZE);
        store[0] = 1;
        return true;
    }
    return err == ESP_OK && size == STORE_SIZE && store[0] == 1 && store[1] <= ECOS_PEERS_MAX;
}

static void peer_view(ecos_snapshot_t *view, const uint8_t store[STORE_SIZE])
{
    view->count = store[1];
    for (int i = 0; i < view->count; i++)
    {
        const uint8_t *p = store + 2 + i * RECORD_SIZE;
        id_text(p, view->peers[i].id);
        memcpy(view->peers[i].name, p + 6, ECOS_NAME_SIZE);
        view->peers[i].name[ECOS_NAME_SIZE - 1] = 0;
    }
}

void ecos_init(void)
{
    /* No secrets retained in the idle snapshot. */
    uint8_t store[STORE_SIZE];
    ecos_snapshot_t view = {0};
    if (load_store(store))
    {
        peer_view(&view, store);
        snprintf(view.message, sizeof(view.message), "Pronto per un nuovo abbinamento");
    }
    else
    {
        snprintf(view.message, sizeof(view.message), "Archivio ECOS non leggibile");
    }
    mbedtls_platform_zeroize(store, sizeof(store));
    portENTER_CRITICAL(&s_guard);
    s_view = view;
    portEXIT_CRITICAL(&s_guard);
}

static void publish(session_t *s, const char *message)
{
    s->view.comparing = s->selected;
    s->view.local_ok = s->local_ok;
    s->view.remote_ok = s->remote_ok;
    s->view.saved = s->saved;
    int64_t left = s->deadline - esp_timer_get_time();
    s->view.seconds_left = left > 0 ? (unsigned)((left + 999999) / 1000000) : 0;
    if (message != NULL)
    {
        snprintf(s->view.message, sizeof(s->view.message), "%s", message);
    }
    portENTER_CRITICAL(&s_guard);
    s_view = s->view;
    portEXIT_CRITICAL(&s_guard);
}

static bool mac(const uint8_t key[32], const uint8_t *message, size_t length, uint8_t out[32])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id = 0;
    size_t written = 0;
    bool ok = psa_import_key(&attr, key, 32, &id) == PSA_SUCCESS &&
              psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), message, length, out, 32, &written) == PSA_SUCCESS &&
              written == 32;
    if (id != 0)
    {
        psa_destroy_key(id);
    }
    psa_reset_key_attributes(&attr);
    return ok;
}

/* Commit before revealing either public key/nonce, to prevent adaptive SAS
 * grinding. See the hash-commitment rationale in RFC 6189 section 4.4.1.1.
 * This is not an implementation of ZRTP. */
static bool hello_commitment(const uint8_t sender[6], const uint8_t hello[ECOS_HELLO_SIZE], uint8_t out[32])
{
    uint8_t input[8 + 6 + ECOS_HELLO_SIZE];
    memcpy(input, "EC-COMM1", 8);
    memcpy(input + 8, sender, 6);
    memcpy(input + 14, hello, ECOS_HELLO_SIZE);
    size_t length = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, input, sizeof(input), out, 32, &length) == PSA_SUCCESS && length == 32;
}

static bool generate(session_t *s)
{
    if (psa_crypto_init() != PSA_SUCCESS || esp_read_mac(s->mac, ESP_MAC_WIFI_STA) != ESP_OK)
    {
        return false;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    size_t length = 0;
    bool ok = psa_generate_key(&attr, &s->ephemeral) == PSA_SUCCESS &&
              psa_export_public_key(s->ephemeral, s->hello + 16, 65, &length) == PSA_SUCCESS && length == 65;
    psa_reset_key_attributes(&attr);
    esp_fill_random(s->hello, 16);
    snprintf((char *)s->hello + 81, ECOS_NAME_SIZE, "%s", sys_nickname());
    return ok && hello_commitment(s->mac, s->hello, s->commitment);
}

static bool derive(session_t *s)
{
    uint8_t secret[32], transcript[ECOS_TRANSCRIPT_SIZE], material[32];
    ecos_wire_transcript(transcript, s->mac, s->peer, s->hello, s->other);
    size_t length = 0;
    bool ok = psa_raw_key_agreement(PSA_ALG_ECDH, s->ephemeral, s->other + 16, 65, secret, 32, &length) == PSA_SUCCESS &&
              length == 32 &&
              psa_hash_compute(PSA_ALG_SHA_256, transcript, sizeof(transcript), s->transcript, 32, &length) == PSA_SUCCESS &&
              length == 32 && mac(secret, s->transcript, 32, s->key);
    if (ok)
    {
        uint8_t input[35];
        memcpy(input, "SAS", 3);
        memcpy(input + 3, s->transcript, 32);
        ok = mac(s->key, input, sizeof(input), material);
        if (ok)
        {
            s->view.code = ((uint32_t)material[0] << 24 | (uint32_t)material[1] << 16 |
                            (uint32_t)material[2] << 8 | material[3]) % 1000000;
        }
    }
    mbedtls_platform_zeroize(secret, sizeof(secret));
    mbedtls_platform_zeroize(material, sizeof(material));
    return ok;
}

static bool tag(session_t *s, uint8_t kind, const uint8_t sender[6], uint8_t out[32])
{
    uint8_t input[39];
    input[0] = kind;
    memcpy(input + 1, sender, 6);
    memcpy(input + 7, s->transcript, 32);
    return mac(s->key, input, sizeof(input), out);
}

static void send_packet(session_t *s, int fd, uint8_t kind)
{
    uint8_t frame[ECOS_FRAME_MAX];
    static const uint8_t nobody[6];
    ecos_wire_header(frame, kind, s->mac, s->committed ? s->peer : nobody);
    size_t length = ECOS_HEADER + 32;
    if (kind == ECOS_COMMIT)
    {
        memcpy(frame + ECOS_HEADER, s->commitment, 32);
    }
    else if (kind == ECOS_HELLO)
    {
        memcpy(frame + ECOS_HEADER, s->hello, ECOS_HELLO_SIZE);
        length = sizeof(frame);
    }
    else if (!tag(s, kind, s->mac, frame + ECOS_HEADER))
    {
        return;
    }
    if (s->espnow)
    {
        esp_now_send(s_broadcast, frame, length);
    }
    else
    {
        sendto(fd, frame, length, 0, (struct sockaddr *)&s->destination, sizeof(s->destination));
    }
}

static bool save_peer(session_t *s)
{
    unsigned slot;
    for (slot = 0; slot < s->store[1]; slot++)
    {
        if (memcmp(s->store + 2 + slot * RECORD_SIZE, s->peer, 6) == 0)
        {
            break;
        }
    }
    if (slot == ECOS_PEERS_MAX)
    {
        return false;
    }
    uint8_t *record = s->store + 2 + slot * RECORD_SIZE;
    memcpy(record, s->peer, 6);
    memcpy(record + 6, s->other + 81, ECOS_NAME_SIZE);
    /* Distinct domain from SAS and confirmation tags. Future transport keys
     * must also use separate derivation labels, never the handshake key. */
    uint8_t input[36];
    memcpy(input, "LINK", 4);
    memcpy(input + 4, s->transcript, 32);
    if (!mac(s->key, input, sizeof(input), record + 6 + ECOS_NAME_SIZE))
    {
        return false;
    }
    if (slot == s->store[1])
    {
        s->store[1]++;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("ecos", NVS_READWRITE, &handle);
    if (err == ESP_OK)
    {
        err = nvs_set_blob(handle, "peers", s->store, STORE_SIZE);
        if (err == ESP_OK)
        {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err == ESP_OK)
    {
        peer_view(&s->view, s->store);
    }
    return err == ESP_OK;
}

static bool receive_packet(session_t *s, const uint8_t *p, size_t n, const struct sockaddr_in *source,
                           const uint8_t *radio_mac)
{
    static const uint8_t nobody[6];
    if (!ecos_wire_valid(p, n) ||
        memcmp(p + 5, s->mac, 6) == 0 || memcmp(p + 5, nobody, 6) == 0 ||
        (p[5] & 1) != 0 ||
        (memcmp(p + 11, nobody, 6) != 0 && memcmp(p + 11, s->mac, 6) != 0))
    {
        return true;
    }
    /* ESP-NOW: the radio sender must be the MAC written in the frame. */
    if (radio_mac != NULL && memcmp(radio_mac, p + 5, 6) != 0)
    {
        return true;
    }
    if (s->committed && (memcmp(p + 5, s->peer, 6) != 0 ||
                       (source != NULL && (source->sin_addr.s_addr != s->destination.sin_addr.s_addr ||
                                           source->sin_port != s->destination.sin_port))))
    {
        return true;
    }
    if (p[4] == ECOS_COMMIT)
    {
        if (!s->committed)
        {
            memcpy(s->peer, p + 5, 6);
            memcpy(s->peer_commitment, p + ECOS_HEADER, 32);
            if (source != NULL)
            {
                s->destination = *source;
            }
            s->committed = true;
            publish(s, "Dispositivo trovato: scambio delle chiavi...");
        }
        /* A commitment cannot change during a window. */
        return true;
    }
    if (p[4] == ECOS_HELLO && s->committed)
    {
        if (s->selected)
        {
            return true;
        }
        const uint8_t *hello = p + ECOS_HEADER;
        uint8_t expected[32];
        if (!hello_commitment(s->peer, hello, expected) || !ecos_wire_equal(expected, s->peer_commitment, 32))
        {
            return false;
        }
        if (hello[16] != 4 || hello[ECOS_HELLO_SIZE - 1] != 0)
        {
            return true;
        }
        /* Names are display-only, bounded printable ASCII in this version. */
        for (unsigned i = 81; i < ECOS_HELLO_SIZE - 1 && hello[i] != 0; i++)
        {
            if (hello[i] < 32 || hello[i] > 126)
            {
                return true;
            }
        }
        memcpy(s->other, hello, ECOS_HELLO_SIZE);
        if (!derive(s))
        {
            return false;
        }
        s->selected = true;
        memcpy(s->view.candidate, hello + 81, ECOS_NAME_SIZE);
        id_text(s->peer, s->view.candidate_id);
        publish(s, "Confronta il codice sui due dispositivi, poi conferma su entrambi");
    }
    else if (s->selected && memcmp(p + 11, s->mac, 6) == 0)
    {
        uint8_t expected[32];
        if (tag(s, p[4], s->peer, expected) && ecos_wire_equal(expected, p + ECOS_HEADER, 32))
        {
            s->remote_ok = true;
            if (p[4] == ECOS_SAVED)
            {
                s->remote_saved = true;
            }
        }
    }
    return true;
}

static void worker(void *arg)
{
    uint32_t target_ip = (uint32_t)(uintptr_t)arg;
    session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM);
    int fd = -1;
    bool wifi_held = false, fast_held = false, espnow_on = false;
    int8_t tx_power = 0;
    const char *result = "Memoria insufficiente";
    if (s == NULL)
    {
        goto done;
    }
    ecos_snapshot(&s->view);
    s->deadline = esp_timer_get_time() + WINDOW_US;
    if (!load_store(s->store))
    {
        result = "Archivio ECOS non leggibile: abbinamento annullato";
        goto done;
    }
    s->espnow = target_ip == 0;
    if (s->espnow)
    {
        publish(s, "Accendo la radio...");
        if (wifi_radio_acquire() != ESP_OK)
        {
            result = "Radio non disponibile (memoria): riprova";
            goto done;
        }
    }
    else
    {
        publish(s, "Connessione al Wi-Fi di casa...");
        if (wifi_acquire(15000) != ESP_OK)
        {
            result = "Wi-Fi non disponibile: collega entrambi alla stessa rete";
            goto done;
        }
    }
    wifi_held = true;
    if (!generate(s))
    {
        result = "Impossibile avviare la crittografia";
        goto done;
    }
    if (s->espnow)
    {
        wifi_set_fast(true);   /* no modem sleep: frames would be missed */
        fast_held = true;
        s_rx = xQueueCreate(8, sizeof(radio_frame_t));
        esp_now_peer_info_t peer = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};
        memcpy(peer.peer_addr, s_broadcast, 6);
        if (s_rx == NULL || esp_now_init() != ESP_OK)
        {
            result = "ESP-NOW non disponibile";
            goto done;
        }
        espnow_on = true;
        if (esp_now_register_recv_cb(on_radio) != ESP_OK || esp_now_add_peer(&peer) != ESP_OK)
        {
            result = "ESP-NOW non disponibile";
            goto done;
        }
        if (!wifi_is_connected())
        {
            wifi_radio_set_channel(1);   /* the meeting point */
        }
        /* The two devices are usually side by side: at full power (20 dBm) the
         * receiver saturates and the frames are lost. 8 dBm still covers a room,
         * and keeps the pairing local. Restored at the end. */
        if (esp_wifi_get_max_tx_power(&tx_power) == ESP_OK)
        {
            esp_wifi_set_max_tx_power(PAIRING_TX_POWER);
        }
        publish(s, "Avvia Abbinamento anche sull'altro dispositivo, vicino: non servono Wi-Fi nÃ© internet");
    }
    else
    {
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int yes = 1;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(ECOS_PORT),
                                .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0)
    {
        result = "Impossibile aprire il collegamento locale";
        goto done;
    }
    s->destination = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(ECOS_PORT),
                                        .sin_addr.s_addr = target_ip != 0 ? target_ip : htonl(INADDR_BROADCAST)};
    publish(s, "Avvia Abbinamento anche sul dispositivo all'indirizzo indicato");
    }
    int64_t next_send = 0;
    int64_t started = esp_timer_get_time(), next_hop = started + FIRST_DWELL_US;
    unsigned hop = 0;
    result = "Tempo scaduto: nessun nuovo abbinamento completato";
    while (esp_timer_get_time() < s->deadline)
    {
        bool cancel, confirm;
        portENTER_CRITICAL(&s_guard);
        cancel = s_cancel;
        confirm = s_confirm;
        s_confirm = false;
        portEXIT_CRITICAL(&s_guard);
        if (cancel)
        {
            result = s->saved ? "Salvato qui; verifica l'esito sull'altro dispositivo" : "Abbinamento annullato";
            break;
        }
        if (!s->espnow && !wifi_is_connected())
        {
            result = "Connessione Wi-Fi interrotta";
            break;
        }
        /* ESP-NOW, nobody heard yet and not tied to a network: visit the
         * channels (the other side may be joined to a network elsewhere). */
        if (s->espnow && !s->committed && !wifi_is_connected() && esp_timer_get_time() >= next_hop)
        {
            hop = (hop + 1) % sizeof(s_hops);
            wifi_radio_set_channel(s_hops[hop]);
            next_hop = esp_timer_get_time() + HOP_DWELL_US;
            next_send = 0;   /* announce at once on the new channel */
        }
        if (confirm && s->selected && !s->local_ok)
        {
            s->local_ok = true;
            publish(s, "Confermato qui; attendo la conferma dell'altro dispositivo");
        }
        if (s->local_ok && s->remote_ok && !s->saved)
        {
            if (!save_peer(s))
            {
                result = "Salvataggio non riuscito o elenco pieno: riprova";
                break;
            }
            s->saved = true;
            publish(s, "Salvato qui; attendo la conferma di salvataggio dell'altro dispositivo");
        }
        if (s->saved && s->remote_saved && s->done_at == 0)
        {
            s->done_at = esp_timer_get_time();
            publish(s, "Abbinamento completato su entrambi i dispositivi");
        }
        if (s->done_at != 0 && esp_timer_get_time() - s->done_at >= 5000000)
        {
            result = "Abbinamento completato su entrambi i dispositivi";
            break;
        }
        if (esp_timer_get_time() >= next_send)
        {
            send_packet(s, fd, ECOS_COMMIT);
            if (s->committed)
            {
                send_packet(s, fd, ECOS_HELLO);
            }
            if (s->local_ok)
            {
                send_packet(s, fd, s->saved ? ECOS_SAVED : ECOS_CONFIRM);
            }
            next_send = esp_timer_get_time() + (s->espnow ? 300000 : 750000);
            publish(s, NULL);
        }
        if (s->espnow)
        {
            radio_frame_t frame;
            bool bad = false;
            /* Wait up to 200 ms for the first frame, then drain without waiting. */
            for (int i = 0; i < 8 && xQueueReceive(s_rx, &frame, i == 0 ? pdMS_TO_TICKS(200) : 0) == pdTRUE; i++)
            {
                if (!receive_packet(s, frame.data, frame.length, NULL, frame.mac))
                {
                    bad = true;
                    break;
                }
            }
            if (bad)
            {
                result = "Scambio crittografico non valido: riavvia l'abbinamento";
                break;
            }
        }
        else
        {
            uint8_t packet[ECOS_FRAME_MAX + 1];
            struct sockaddr_in source;
            socklen_t length = sizeof(source);
            int n = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr *)&source, &length);
            if (n > 0 && !receive_packet(s, packet, (size_t)n, &source, NULL))
            {
                result = "Scambio crittografico non valido: riavvia l'abbinamento";
                break;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                result = "Errore del collegamento locale";
                break;
            }
        }
        /* Bound CPU use even under unauthenticated packet floods. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s->saved && s->done_at == 0 && esp_timer_get_time() >= s->deadline)
    {
        result = "Salvato qui; esito remoto non confermato. Verifica l'altro dispositivo";
    }
done:
    if (fd >= 0)
    {
        close(fd);
    }
    if (tx_power != 0)
    {
        esp_wifi_set_max_tx_power(tx_power);
    }
    if (espnow_on)
    {
        esp_now_unregister_recv_cb();
        esp_now_deinit();
    }
    if (s_rx != NULL)
    {
        QueueHandle_t rx = s_rx;
        s_rx = NULL;
        vQueueDelete(rx);
    }
    if (fast_held)
    {
        wifi_set_fast(false);
    }
    if (s != NULL && s->ephemeral != 0)
    {
        psa_destroy_key(s->ephemeral);
    }
    if (wifi_held)
    {
        wifi_release();
    }
    portENTER_CRITICAL(&s_guard);
    if (s != NULL)
    {
        s_view = s->view;
    }
    s_view.running = false;
    s_view.comparing = false;
    s_view.seconds_left = 0;
    /* s_busy stays true until all private resources have been released. */
    portEXIT_CRITICAL(&s_guard);
    if (s != NULL)
    {
        mbedtls_platform_zeroize(s, sizeof(*s));
        free(s);
    }
    portENTER_CRITICAL(&s_guard);
    snprintf(s_view.message, sizeof(s_view.message), "%s", result);
    s_busy = false;
    portEXIT_CRITICAL(&s_guard);
    ESP_LOGI(TAG, "Pairing ended: %s", result);
    sys_heap_log("ecos stopped");
    vTaskDelete(NULL);
}

esp_err_t ecos_pair_start(const char *peer_ip)
{
    struct in_addr address = {0};
    if (peer_ip != NULL && peer_ip[0] != 0)
    {
        if (inet_pton(AF_INET, peer_ip, &address) != 1)
        {
            return ESP_ERR_INVALID_ARG;
        }
        uint32_t host = ntohl(address.s_addr);
        if ((host >> 24) != 10 && (host >> 20) != 0xac1 && (host >> 16) != 0xc0a8)
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    portENTER_CRITICAL(&s_guard);
    if (s_busy)
    {
        portEXIT_CRITICAL(&s_guard);
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    s_cancel = false;
    s_confirm = false;
    s_view.running = true;
    s_view.comparing = s_view.local_ok = s_view.remote_ok = s_view.saved = false;
    s_view.code = 0;
    s_view.candidate[0] = s_view.candidate_id[0] = 0;
    s_view.seconds_left = 180;
    snprintf(s_view.message, sizeof(s_view.message), "Avvio abbinamento...");
    portEXIT_CRITICAL(&s_guard);
    if (!sys_task_create(worker, "ecos_pair", 6144, (void *)(uintptr_t)address.s_addr, 2, SYS_CORE_ANY))
    {
        portENTER_CRITICAL(&s_guard);
        s_busy = s_view.running = false;
        snprintf(s_view.message, sizeof(s_view.message), "Memoria insufficiente");
        portEXIT_CRITICAL(&s_guard);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ecos_pair_confirm(const char *id, uint32_t code)
{
    portENTER_CRITICAL(&s_guard);
    bool valid = id != NULL && s_busy && !s_cancel && s_view.comparing &&
                 code == s_view.code && strcmp(id, s_view.candidate_id) == 0;
    if (valid)
    {
        s_confirm = true;
    }
    portEXIT_CRITICAL(&s_guard);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void ecos_pair_cancel(void)
{
    portENTER_CRITICAL(&s_guard);
    s_cancel = true;
    portEXIT_CRITICAL(&s_guard);
}

void ecos_snapshot(ecos_snapshot_t *out)
{
    if (out != NULL)
    {
        portENTER_CRITICAL(&s_guard);
        *out = s_view;
        portEXIT_CRITICAL(&s_guard);
    }
}
