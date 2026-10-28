/* ESP-NOW session link and audio stream (F5 + F5b MVP). See link.h.
 *
 * Tasks: the Wi-Fi receive callback only timestamps and copies into a bounded
 * queue; the link task (core 0) authenticates, answers, encodes and decodes; the
 * audio task (core 1) touches only the tap ring and the jitter buffer, through
 * per-slot tags (invalidate, copy, publish) and a seqlock for the clock model. */
#include "link.h"
#include "adpcm.h"
#include "engine.h"
#include "esp_event.h"
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
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int solco_output_mode(void); /* MASTER sheet: 0 all, 1 hub only, 2 follow the mixer */

#define VERSION 2
#define T_BEACON 1
#define T_SYNC_REQ 2
#define T_SYNC_RESP 3
#define T_JOIN 4
#define T_KEY 5
#define T_AUDIO 6
#define T_STATUS 7
#define T_UPLINK 8               /* member -> hub: local mix, ADPCM, frames on the hub line */
#define FOLLOW_MS 30000         /* "follow the mixer": stay audible 30 s after the last action */
#define MEMBERS 6
#define CONVERGED_SAMPLES 8
#define CONVERGED_RESIDUAL_US 1000
#define HDR 20
#define TAG 8
#define FRAME_MAX 600
#define RATE 44100
#define SB 256                 /* stream block, frames */
#define SLOTS 32               /* jitter buffer: 32 x 256 frames = 186 ms */
#define TAP_RING 4096          /* audio -> link mono ring, frames */
#define SAMPLES 48             /* clock model window */
#define RTT_LIMIT_US 20000
#define RTT_SLACK_US 300
#define ECOS_RECORD (6 + 24 + 32)
#define ECOS_PEERS 6

static const char *TAG_LOG = "solco_link";

typedef struct
{
    uint8_t mac[6];
    int64_t t_rx;
    uint16_t len;
    uint8_t data[FRAME_MAX];
} rx_t;

typedef struct
{
    uint8_t mac[6];
    psa_key_id_t key;  /* pair key */
    uint32_t last_seq; /* hub side: replay guard for this member */
} peer_t;

typedef struct
{
    double t, off;
    uint32_t rtt;
} sample_t;

/* Clock model published to the audio task: hub = local + a + b * (local - origin). */
typedef struct
{
    double a_us, b;
    int64_t origin_us;
    int64_t hub_anchor_us; /* hub I2S line, hub time */
    uint32_t d_us;
    bool valid, stream, converged;
    bool all;                  /* routing: every device plays */
    uint8_t active[MEMBERS][6];
    uint8_t active_count;
} model_t;

static QueueHandle_t s_rx;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static solco_link_stats_t s_st;
static volatile bool s_stop;
static bool s_wifi_up;
static uint32_t s_seq;
static peer_t s_peers[ECOS_PEERS];
static int s_peer_count;
static psa_key_id_t s_session_key;
static uint8_t s_session_raw[32];
static uint8_t s_join_nonce[16];
static uint8_t s_cand_mac[6]; /* hub we sent a JOIN to, adopted only after its KEY verifies */
static uint32_t s_cand_session;
static uint32_t s_hub_last_seq;
static int64_t s_last_beacon_us;
static sample_t s_samples[SAMPLES];
static int s_sample_count, s_sample_next;
static int64_t s_origin_us;
static model_t s_model[2];
static uint32_t s_model_seq;
/* Session delay budget (see the uplink notes at solco_stream_tap):
 *   uplink U: member block (5.8 ms) + redundancy (5.8) + link poll/encode/air (~3) +
 *             radio jitter p99 (~5) + render-line slack (~5) = ~25 ms -> 35 ms.
 *   downlink D: same chain hub -> members plus the members' resampler span and the
 *             extra airtime of N uplinks (contention) -> 60 ms (was 50 with no uplink).
 * A member's contribution is heard everywhere U + D = ~95 ms after it was rendered. */
static uint32_t s_d_us = 60000;
static uint32_t s_u_us = 35000;
static bool s_stream_wanted;
static uint8_t s_my_mac[6];
typedef struct
{
    uint8_t mac[6];
    uint32_t idle_ms;
    int64_t at_us;
    uint32_t last_seq; /* uplink replay guard, reset on JOIN */
} member_t;
static member_t s_members[MEMBERS];
static int s_member_count;
static uint8_t s_active[MEMBERS][6];
static int s_active_count;
static float s_route_gain; /* audio task: speaker gain of the routed stream / hub output */

/* Tap ring (hub): written by the audio task, read by the link task. */
static int16_t *s_tap;
static uint32_t s_tap_end; /* frame after the last written sample */
static uint32_t s_tap_next;
static solco_adpcm_t s_enc;
static uint8_t s_prev_block[4 + SB / 2];
static bool s_prev_valid;

/* Jitter buffer: member <- hub (downlink) and hub <- each member (uplink). */
typedef struct
{
    uint32_t frame0; /* tag: UINT32_MAX while invalid or being written */
    int16_t pcm[SB];
} slot_t;
typedef struct
{
    slot_t *slots;     /* SLOTS, written by the link task */
    uint32_t newest;   /* frame after the newest stored block */
    /* audio task only */
    double p;          /* playhead in sender frames */
    bool p_valid;
    float plc_last, fade;
} rxs_t;
/* Hub: where a member's frames sit on the hub line (seqlock, link -> audio task). */
typedef struct
{
    uint32_t seq;      /* odd while being written */
    double mf, hf;     /* member frame mf is hub frame hf */
    float rate;        /* hub frames per member frame */
    int64_t at_us;     /* last packet, 0 = none */
} upmap_t;
static rxs_t s_down;
static slot_t *s_slots; /* = s_down.slots */
static rxs_t s_up[MEMBERS];
static upmap_t s_upmap[MEMBERS];
static uint32_t s_up_reset[MEMBERS]; /* link task bumps, audio task drops the playhead */
static int16_t *s_delay;     /* hub local delay line, stereo */
static uint32_t s_delay_pos;
static float s_local_gain = 1.f; /* member: own mix on its speaker (1 out of kit, 0 in kit) */

#define LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define RELAXED(p) __atomic_load_n((p), __ATOMIC_RELAXED)

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, int64_t v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = (uint8_t)((uint64_t)v >> (8 * i));
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = v << 8 | p[i];
    return (int64_t)v;
}

/* ------------------------------------------------------------- crypto */

static psa_key_id_t import_key(const uint8_t raw[32])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id = 0;
    return psa_import_key(&attr, raw, 32, &id) == PSA_SUCCESS ? id : 0;
}

static bool hmac(psa_key_id_t key, const uint8_t *msg, size_t len, uint8_t out[32])
{
    size_t written = 0;
    return key && psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, len, out, 32, &written) == PSA_SUCCESS &&
           written == 32;
}

static bool hmac_raw(const uint8_t raw[32], const uint8_t *msg, size_t len, uint8_t out[32])
{
    psa_key_id_t key = import_key(raw);
    bool ok = hmac(key, msg, len, out);
    if (key)
        psa_destroy_key(key);
    return ok;
}

static void sign(psa_key_id_t key, uint8_t *frame, int len)
{
    uint8_t mac[32] = {0};
    hmac(key, frame, (size_t)len, mac);
    memcpy(frame + len, mac, TAG);
}

static bool verify(psa_key_id_t key, const uint8_t *frame, int len)
{
    if (len < HDR + TAG || !key)
        return false;
    uint8_t mac[32];
    if (!hmac(key, frame, (size_t)(len - TAG), mac))
        return false;
    uint8_t diff = 0;
    for (int i = 0; i < TAG; ++i)
        diff |= mac[i] ^ frame[len - TAG + i];
    return diff == 0;
}

/* Pair keys from the ECOS store, read only: HMAC(credential, "SLNK"). */
static int load_peers(bool open)
{
    for (int i = 0; i < s_peer_count; ++i)
        psa_destroy_key(s_peers[i].key);
    s_peer_count = 0;
    if (open)
        return 0;
    uint8_t store[2 + ECOS_PEERS * ECOS_RECORD];
    size_t size = sizeof(store);
    nvs_handle_t h;
    if (nvs_open("ecos", NVS_READONLY, &h) != ESP_OK)
        return 0;
    esp_err_t err = nvs_get_blob(h, "peers", store, &size);
    nvs_close(h);
    if (err != ESP_OK || size != sizeof(store) || store[0] != 1)
        return 0;
    for (int i = 0; i < store[1] && i < ECOS_PEERS; ++i)
    {
        const uint8_t *record = store + 2 + i * ECOS_RECORD;
        uint8_t raw[32];
        if (!hmac_raw(record + 30, (const uint8_t *)"SLNK", 4, raw))
            continue;
        memcpy(s_peers[s_peer_count].mac, record, 6);
        s_peers[s_peer_count].key = import_key(raw);
        s_peers[s_peer_count].last_seq = 0;
        if (s_peers[s_peer_count].key)
            ++s_peer_count;
    }
    return s_peer_count;
}

static peer_t *find_peer(const uint8_t mac[6])
{
    for (int i = 0; i < s_peer_count; ++i)
        if (!memcmp(s_peers[i].mac, mac, 6))
            return &s_peers[i];
    return NULL;
}

/* Development mode (both devices started with "open"): a fixed public key. */
static psa_key_id_t open_key(void)
{
    static psa_key_id_t key;
    if (!key)
    {
        uint8_t raw[32];
        hmac_raw((const uint8_t *)"solco-open-development-key-0000", (const uint8_t *)"SLNK", 4, raw);
        key = import_key(raw);
    }
    return key;
}

static psa_key_id_t pair_key(const uint8_t mac[6])
{
    if (s_st.open)
        return open_key();
    peer_t *peer = find_peer(mac);
    return peer ? peer->key : 0;
}

/* ------------------------------------------------------------- frames */

static int header(uint8_t *f, uint8_t type, int64_t now)
{
    f[0] = 'S';
    f[1] = 'L';
    f[2] = VERSION;
    f[3] = type;
    put32(f + 4, s_st.session);
    put32(f + 8, ++s_seq);
    put64(f + 12, now);
    return HDR;
}

static void send_to(const uint8_t *mac, uint8_t *f, int len, psa_key_id_t key)
{
    sign(key, f, len);
    esp_err_t err = esp_now_send(mac, f, (size_t)(len + TAG));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (err == ESP_OK)
        ++s_st.tx;
    else
        ++s_st.tx_errors;
    xSemaphoreGive(s_lock);
}

static void ensure_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac))
        return;
    esp_now_peer_info_t peer = {.channel = (uint8_t)s_st.channel, .ifidx = WIFI_IF_STA};
    memcpy(peer.peer_addr, mac, 6);
    if (esp_now_add_peer(&peer) == ESP_OK)
    {
        /* 24 Mbit/s OFDM: the stream takes ~5 % of the channel instead of ~50 %. */
        esp_now_rate_config_t rate = {.phymode = WIFI_PHY_MODE_11G, .rate = WIFI_PHY_RATE_24M};
        esp_now_set_peer_rate_config(mac, &rate);
    }
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    static rx_t item; /* only the Wi-Fi task calls this: keep 600 B off its stack */
    item.t_rx = esp_timer_get_time();
    if (len < HDR + TAG || len > FRAME_MAX || data[0] != 'S' || data[1] != 'L')
        return;
    memcpy(item.mac, info->src_addr, 6);
    item.len = (uint16_t)len;
    memcpy(item.data, data, (size_t)len);
    xQueueSend(s_rx, &item, 0);
}

/* ------------------------------------------------------------- clock model */

static void publish_model(double a, double b, bool valid)
{
    uint32_t seq = RELAXED(&s_model_seq) + 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    model_t m = s_model[(seq + 1) & 1];
    model_t cur = s_model[RELAXED(&s_model_seq) & 1];
    m = cur;
    m.a_us = a;
    m.b = b;
    m.origin_us = s_origin_us;
    m.d_us = s_d_us;
    m.valid = valid;
    m.converged = valid && s_sample_count >= CONVERGED_SAMPLES && s_st.residual_us < CONVERGED_RESIDUAL_US;
    s_model[seq & 1] = m;
    STORE(&s_model_seq, seq);
}

static void publish_beacon_fields(int64_t anchor, uint32_t d_us, bool stream, bool all,
                                  const uint8_t active[][6], int count)
{
    model_t cur = s_model[RELAXED(&s_model_seq) & 1];
    cur.all = all;
    cur.active_count = (uint8_t)(count > MEMBERS ? MEMBERS : count);
    if (count > 0)
        memcpy(cur.active, active, (size_t)cur.active_count * 6);
    uint32_t seq = RELAXED(&s_model_seq) + 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    cur.hub_anchor_us = anchor;
    cur.d_us = d_us;
    cur.stream = stream;
    s_model[seq & 1] = cur;
    STORE(&s_model_seq, seq);
}

static void read_model(model_t *out)
{
    for (;;)
    {
        uint32_t seq = LOAD(&s_model_seq);
        *out = s_model[seq & 1];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (RELAXED(&s_model_seq) == seq)
            return;
    }
}

static void fit(void)
{
    int n = s_sample_count;
    uint32_t best = UINT32_MAX;
    for (int i = 0; i < n; ++i)
        if (s_samples[i].rtt < best)
            best = s_samples[i].rtt;
    double st = 0, so = 0, stt = 0, sto = 0;
    int m = 0;
    for (int i = 0; i < n; ++i)
    {
        if (s_samples[i].rtt > best + RTT_SLACK_US)
            continue;
        st += s_samples[i].t;
        so += s_samples[i].off;
        stt += s_samples[i].t * s_samples[i].t;
        sto += s_samples[i].t * s_samples[i].off;
        ++m;
    }
    if (!m)
        return;
    double den = m * stt - st * st;
    double b = m >= 6 && fabs(den) > 1e-6 ? (m * sto - st * so) / den : 0;
    double a = (so - b * st) / m;
    double residual = 0;
    for (int i = 0; i < n; ++i)
    {
        if (s_samples[i].rtt > best + RTT_SLACK_US)
            continue;
        double e = s_samples[i].off - (a + b * s_samples[i].t);
        residual += e * e;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.skew_ppb = (int32_t)lrint(b * 1000.0);
    s_st.residual_us = (uint32_t)lrint(sqrt(residual / m));
    xSemaphoreGive(s_lock);
    publish_model(a, b, true);
}

static bool hub_time(const model_t *m, int64_t local, double *hub)
{
    if (s_st.role == SOLCO_LINK_HUB)
    {
        *hub = (double)local;
        return true;
    }
    if (!m->valid)
        return false;
    double t = (local - m->origin_us) / 1e6;
    *hub = (double)local + m->a_us + m->b * t;
    return true;
}

/* ------------------------------------------------------------- receive */

/* Hub: the member table slot of `mac`, created on first contact. A new slot gets a
 * clean uplink buffer before the audio task can see it. */
static int member_slot(const uint8_t mac[6])
{
    for (int i = 0; i < s_member_count; ++i)
        if (!memcmp(s_members[i].mac, mac, 6))
            return i;
    if (s_member_count >= MEMBERS)
        return -1;
    int slot = s_member_count;
    memcpy(s_members[slot].mac, mac, 6);
    s_members[slot].idle_ms = UINT32_MAX;
    s_members[slot].at_us = 0;
    s_members[slot].last_seq = 0;
    for (int i = 0; i < SLOTS; ++i)
        STORE(&s_up[slot].slots[i].frame0, UINT32_MAX);
    STORE(&s_up[slot].newest, 0);
    STORE(&s_upmap[slot].at_us, 0);
    STORE(&s_up_reset[slot], RELAXED(&s_up_reset[slot]) + 1);
    STORE(&s_member_count, slot + 1);
    return slot;
}

/* Decodes 1-2 ADPCM blocks (block 0 new, block 1 the previous one: redundancy)
 * into a jitter buffer. Returns the blocks recovered from redundancy. */
static int store_blocks(rxs_t *r, const uint8_t *blocks_at, uint32_t frame0, int blocks, int16_t *peak)
{
    int recovered = 0;
    for (int b = 0; b < blocks; ++b)
    {
        uint32_t at = frame0 - (uint32_t)(b * SB);
        slot_t *slot = &r->slots[(at / SB) % SLOTS];
        if (RELAXED(&slot->frame0) == at)
            continue; /* already have it */
        const uint8_t *blk = blocks_at + b * (4 + SB / 2);
        solco_adpcm_t st = {.predictor = (int16_t)get16(blk), .index = blk[2] > 88 ? 88 : blk[2]};
        __atomic_store_n(&slot->frame0, UINT32_MAX, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        solco_adpcm_decode(&st, blk + 4, SB, slot->pcm);
        STORE(&slot->frame0, at);
        for (int i = 0; i < SB; ++i)
        {
            int16_t v = slot->pcm[i] < 0 ? (int16_t)-slot->pcm[i] : slot->pcm[i];
            *peak = v > *peak ? v : *peak;
        }
        if (b == 1)
            ++recovered;
    }
    if ((int32_t)(frame0 + SB - RELAXED(&r->newest)) > 0)
        STORE(&r->newest, frame0 + SB);
    return recovered;
}

static void handle(const rx_t *rx)
{
    const uint8_t *f = rx->data;
    int len = rx->len;
    uint8_t type = f[3];
    uint32_t session = get32(f + 4), seq = get32(f + 8);
    int64_t t_send = get64(f + 12);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ++s_st.rx;
    xSemaphoreGive(s_lock);
    if (f[2] != VERSION)
        return;
    bool hub = s_st.role == SOLCO_LINK_HUB;
    if (hub && type == T_JOIN && len >= HDR + 16 + TAG)
    {
        peer_t *peer = s_st.open ? NULL : find_peer(rx->mac);
        psa_key_id_t key = pair_key(rx->mac);
        if (!verify(key, f, len))
            goto bad;
        (void)peer;
        /* No sequence check: the JOIN is signed and bound to its fresh nonce, and the KEY
         * is useless without the pair key, so a replay gains nothing, while a rebooted
         * member (sequence back to a random start) must be able to rejoin at once. */
        /* Session key, encrypted for this member: XOR with HMAC(pair, "SLKE" | session | nonce). */
        uint8_t in[4 + 4 + 16], pad[32];
        static uint8_t out[FRAME_MAX]; /* link task only: off its stack */
        memcpy(in, "SLKE", 4);
        put32(in + 4, s_st.session);
        memcpy(in + 8, f + HDR, 16);
        if (!hmac(key, in, sizeof(in), pad))
            return;
        int n = header(out, T_KEY, esp_timer_get_time());
        memcpy(out + n, f + HDR, 16);
        for (int i = 0; i < 32; ++i)
            out[n + 16 + i] = s_session_raw[i] ^ pad[i];
        ensure_peer(rx->mac);
        send_to(rx->mac, out, n + 48, key);
        int joined = member_slot(rx->mac);
        if (joined >= 0)
            s_members[joined].last_seq = 0; /* a rebooted member restarts its sequence */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ++s_st.joins;
        xSemaphoreGive(s_lock);
        return;
    }
    if (hub && type == T_STATUS && len >= HDR + 4 + TAG)
    {
        if (!verify(s_session_key, f, len))
            goto bad;
        int slot = member_slot(rx->mac);
        if (slot >= 0)
        {
            s_members[slot].idle_ms = get32(f + HDR);
            s_members[slot].at_us = rx->t_rx;
        }
        return;
    }
    if (hub && type == T_SYNC_REQ)
    {
        if (!verify(s_session_key, f, len))
            goto bad;
        static uint8_t out[FRAME_MAX];
        int n = header(out, T_SYNC_RESP, 0);
        put64(out + n, t_send);
        put64(out + n + 8, rx->t_rx);
        put64(out + 12, esp_timer_get_time()); /* t3 */
        ensure_peer(rx->mac);
        send_to(rx->mac, out, n + 16, s_session_key);
        return;
    }
    if (hub && type == T_UPLINK && len >= HDR + 20 + TAG)
    {
        if (session != s_st.session || !verify(s_session_key, f, len))
            goto bad;
        int slot = member_slot(rx->mac);
        if (slot < 0)
            return;
        if (seq <= s_members[slot].last_seq)
            goto replay;
        s_members[slot].last_seq = seq;
        uint32_t frame0 = get32(f + HDR);
        int frames = get16(f + HDR + 4), blocks = f[HDR + 6];
        if (frames != SB || blocks < 1 || blocks > 2 || len < HDR + 20 + blocks * (4 + SB / 2) + TAG)
            goto bad;
        double hf = (double)get64(f + HDR + 8) / 65536.0;
        float rate;
        uint32_t bits = get32(f + HDR + 16);
        memcpy(&rate, &bits, 4);
        if (!(rate > 0.99f && rate < 1.01f))
            goto bad;
        int16_t peak = 0;
        int recovered = store_blocks(&s_up[slot], f + HDR + 20, frame0, blocks, &peak);
        upmap_t *map = &s_upmap[slot];
        STORE(&map->seq, map->seq + 1);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        map->mf = frame0;
        map->hf = hf;
        map->rate = rate;
        map->at_us = rx->t_rx;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        STORE(&map->seq, map->seq + 1);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ++s_st.up_rx;
        s_st.up_recovered += (uint32_t)recovered;
        uint16_t held = (uint16_t)(s_st.up_peak - s_st.up_peak / 64);
        s_st.up_peak = (uint16_t)peak > held ? (uint16_t)peak : held;
        xSemaphoreGive(s_lock);
        return;
    }
    if (hub)
        return;
    /* ---- member ---- */
    if (type == T_BEACON || type == T_AUDIO)
    {
        if (!s_st.joined || session != s_st.session || memcmp(rx->mac, s_st.hub_mac, 6))
        {
            if (type != T_BEACON)
                return;
            /* While our hub is alive an unverified beacon (other session or MAC) never
             * displaces it; we look elsewhere only after 3 s without valid beacons. */
            if (s_st.joined && rx->t_rx - s_last_beacon_us < 3000000)
                return;
            /* A hub we have not joined: ask for its session key (pair key, signed). */
            static int64_t last_join;
            if (rx->t_rx - last_join < 500000 || !pair_key(rx->mac))
                return;
            last_join = rx->t_rx;
            memcpy(s_cand_mac, rx->mac, 6);
            s_cand_session = session;
            esp_fill_random(s_join_nonce, sizeof(s_join_nonce));
            static uint8_t out[FRAME_MAX];
            int n = header(out, T_JOIN, esp_timer_get_time());
            memcpy(out + n, s_join_nonce, 16);
            ensure_peer(rx->mac);
            send_to(rx->mac, out, n + 16, pair_key(rx->mac));
            return;
        }
        if (!verify(s_session_key, f, len))
            goto bad;
        if (seq <= s_hub_last_seq)
            goto replay;
        s_hub_last_seq = seq;
    }
    if (type == T_KEY && len >= HDR + 48 + TAG && !memcmp(rx->mac, s_cand_mac, 6) && session == s_cand_session)
    {
        psa_key_id_t key = pair_key(rx->mac);
        if (!verify(key, f, len) || memcmp(f + HDR, s_join_nonce, 16))
            goto bad;
        uint8_t in[24], pad[32];
        memcpy(in, "SLKE", 4);
        put32(in + 4, session);
        memcpy(in + 8, s_join_nonce, 16);
        if (!hmac(key, in, sizeof(in), pad))
            return;
        for (int i = 0; i < 32; ++i)
            s_session_raw[i] = f[HDR + 16 + i] ^ pad[i];
        if (s_session_key)
            psa_destroy_key(s_session_key);
        s_session_key = import_key(s_session_raw);
        s_hub_last_seq = seq;
        s_sample_count = s_sample_next = 0;
        publish_model(0, 0, false);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.joined = s_session_key != 0;
        s_st.session = session;
        memcpy(s_st.hub_mac, rx->mac, 6);
        s_last_beacon_us = rx->t_rx;
        s_st.beacons = s_st.beacon_lost = 0;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG_LOG, "joined session %08lx", (unsigned long)session);
        return;
    }
    if (type == T_BEACON && len >= HDR + 16 + TAG)
    {
        int64_t gap = s_last_beacon_us ? rx->t_rx - s_last_beacon_us : 0;
        s_last_beacon_us = rx->t_rx;
        uint8_t active[MEMBERS][6];
        int count = len >= HDR + 17 + TAG ? f[HDR + 16] : 255;
        bool all = count == 255;
        if (!all)
        {
            if (count > MEMBERS || len < HDR + 17 + count * 6 + TAG)
                count = 0;
            memcpy(active, f + HDR + 17, (size_t)count * 6);
        }
        publish_beacon_fields(get64(f + HDR), get32(f + HDR + 8), f[HDR + 12] != 0, all, active, all ? 0 : count);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ++s_st.beacons;
        if (gap > 150000)
            s_st.beacon_lost += (uint32_t)((gap + 50000) / 100000) - 1;
        s_st.d_us = get32(f + HDR + 8);
        s_st.streaming = f[HDR + 12] != 0;
        xSemaphoreGive(s_lock);
        return;
    }
    if (type == T_SYNC_RESP && len >= HDR + 16 + TAG)
    {
        if (!verify(s_session_key, f, len))
            goto bad;
        int64_t t1 = get64(f + HDR), t2 = get64(f + HDR + 8), t3 = t_send, t4 = rx->t_rx;
        int64_t rtt = (t4 - t1) - (t3 - t2);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (rtt < 0 || rtt > RTT_LIMIT_US)
        {
            ++s_st.sync_rejected;
            xSemaphoreGive(s_lock);
            return;
        }
        ++s_st.sync_ok;
        s_st.rtt_last_us = (uint32_t)rtt;
        if (!s_st.rtt_min_us || rtt < s_st.rtt_min_us)
            s_st.rtt_min_us = (uint32_t)rtt;
        if (rtt > s_st.rtt_max_us)
            s_st.rtt_max_us = (uint32_t)rtt;
        xSemaphoreGive(s_lock);
        if (!s_sample_count)
            s_origin_us = t4;
        s_samples[s_sample_next] = (sample_t){(t4 - s_origin_us) / 1e6, (double)(((t2 - t1) + (t3 - t4)) / 2),
                                              (uint32_t)rtt};
        s_sample_next = (s_sample_next + 1) % SAMPLES;
        if (s_sample_count < SAMPLES)
            ++s_sample_count;
        fit();
        return;
    }
    if (type == T_AUDIO && len >= HDR + 8 + TAG)
    {
        uint32_t frame0 = get32(f + HDR);
        int frames = get16(f + HDR + 4), blocks = f[HDR + 6];
        if (frames != SB || blocks < 1 || blocks > 2 || len < HDR + 8 + blocks * (4 + SB / 2) + TAG)
            goto bad;
        int64_t start = esp_timer_get_time();
        int16_t peak = 0;
        int recovered = store_blocks(&s_down, f + HDR + 8, frame0, blocks, &peak);
        uint32_t took = (uint32_t)(esp_timer_get_time() - start);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.aud_recovered += (uint32_t)recovered;
        ++s_st.aud_rx;
        /* Peak hold with slow release: the demo is mostly silence between pulses. */
        uint16_t held = (uint16_t)(s_st.rx_peak - s_st.rx_peak / 64);
        s_st.rx_peak = (uint16_t)peak > held ? (uint16_t)peak : held;
        if (took > s_st.decode_us_max)
            s_st.decode_us_max = took;
        xSemaphoreGive(s_lock);
        return;
    }
    return;
bad:
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ++s_st.rx_bad_tag;
    xSemaphoreGive(s_lock);
    return;
replay:
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ++s_st.rx_replay;
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------- hub stream */

/* Hub: T_AUDIO broadcast of the master. Member: T_UPLINK of its local mix to the
 * hub, each block stamped with its place on the hub frame line. */
static void send_audio(const uint8_t *dest, uint8_t type)
{
    bool up = type == T_UPLINK;
    model_t m = {0};
    int64_t anchor = 0;
    if (up)
    {
        int block = 0, queue = 0;
        read_model(&m);
        if (!m.converged || !m.hub_anchor_us || !solco_engine_timeline(&anchor, &block, &queue))
        {
            s_tap_next = 0;
            s_prev_valid = false;
            return;
        }
    }
    uint32_t end = LOAD(&s_tap_end);
    if (!s_tap_next || (int32_t)(end - s_tap_next) > TAP_RING - SB)
        s_tap_next = (end - SB) & ~(uint32_t)(SB - 1); /* start, or fell behind: resync */
    while ((int32_t)(end - (s_tap_next + SB)) >= 0)
    {
        int64_t start = esp_timer_get_time();
        static int16_t pcm[SB];
        int16_t peak = 0;
        for (int i = 0; i < SB; ++i)
        {
            pcm[i] = s_tap[(s_tap_next + i) & (TAP_RING - 1)];
            int16_t v = pcm[i] < 0 ? (int16_t)-pcm[i] : pcm[i];
            peak = v > peak ? v : peak;
        }
        static uint8_t f[FRAME_MAX];
        int n = header(f, type, start);
        put32(f + n, s_tap_next);
        put16(f + n + 4, SB);
        f[n + 6] = s_prev_valid ? 2 : 1;
        f[n + 7] = 1; /* codec: IMA ADPCM mono */
        uint8_t *blk = f + n + 8;
        if (up)
        {
            /* Member frame -> local time on its render line -> hub time -> hub frame. */
            double h0 = 0, h1 = 0;
            hub_time(&m, anchor + (int64_t)((double)s_tap_next * 1e6 / RATE), &h0);
            hub_time(&m, anchor + (int64_t)(((double)s_tap_next + RATE) * 1e6 / RATE), &h1);
            double hf = (h0 - (double)m.hub_anchor_us) * RATE / 1e6;
            float rate = (float)((h1 - h0) / 1e6);
            uint32_t bits;
            memcpy(&bits, &rate, 4);
            put64(f + n + 8, (int64_t)llround(hf * 65536.0));
            put32(f + n + 16, bits);
            blk = f + n + 20;
        }
        put16(blk, (uint16_t)s_enc.predictor);
        blk[2] = s_enc.index;
        blk[3] = 0;
        solco_adpcm_encode(&s_enc, pcm, SB, blk + 4);
        int len = (int)(blk - f) + 4 + SB / 2;
        if (s_prev_valid)
        {
            memcpy(f + len, s_prev_block, sizeof(s_prev_block));
            len += sizeof(s_prev_block);
        }
        memcpy(s_prev_block, blk, sizeof(s_prev_block));
        s_prev_valid = true;
        send_to(dest, f, len, s_session_key);
        uint32_t took = (uint32_t)(esp_timer_get_time() - start);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (up)
            ++s_st.up_tx;
        else
            ++s_st.aud_tx;
        uint16_t held = (uint16_t)(s_st.tx_peak - s_st.tx_peak / 64);
        s_st.tx_peak = (uint16_t)peak > held ? (uint16_t)peak : held;
        if (took > s_st.encode_us_max)
            s_st.encode_us_max = took;
        xSemaphoreGive(s_lock);
        s_tap_next += SB;
    }
}

/* ------------------------------------------------------------- routing */

/* "Follow the mixer": the devices touched in the last 30 s play; if nobody is
 * active the last set keeps playing (never zero outputs). The hub counts as a
 * device like the others. */
static void update_route(int64_t now)
{
    uint8_t next[MEMBERS][6];
    int count = 0;
    if (solco_engine_idle_ms() < FOLLOW_MS)
        memcpy(next[count++], s_my_mac, 6);
    for (int i = 0; i < s_member_count && count < MEMBERS; ++i)
    {
        uint64_t idle = (uint64_t)s_members[i].idle_ms + (uint64_t)((now - s_members[i].at_us) / 1000);
        if (s_members[i].idle_ms != UINT32_MAX && idle < FOLLOW_MS && now - s_members[i].at_us < 3000000)
            memcpy(next[count++], s_members[i].mac, 6);
    }
    if (count)
    {
        memcpy(s_active, next, (size_t)count * 6);
        s_active_count = count;
    }
    else if (!s_active_count)
    {
        memcpy(s_active[0], s_my_mac, 6); /* nobody yet: the hub plays */
        s_active_count = 1;
    }
}

static bool routed_here(const model_t *m)
{
    if (m->all || solco_output_mode() == 1)
        return true;
    for (int i = 0; i < m->active_count; ++i)
        if (!memcmp(m->active[i], s_my_mac, 6))
            return true;
    return false;
}

/* ------------------------------------------------------------- task */

static void link_task(void *arg)
{
    (void)arg;
    static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    int64_t next_beacon = 0, next_sync = 0, next_status = 0;
    while (!s_stop)
    {
        static rx_t rx;
        bool uplink = s_st.role == SOLCO_LINK_MEMBER && s_st.joined;
        TickType_t wait = (s_st.role == SOLCO_LINK_HUB && s_stream_wanted) || uplink ? 1 : pdMS_TO_TICKS(5);
        while (xQueueReceive(s_rx, &rx, wait) == pdTRUE)
        {
            handle(&rx);
            wait = 0;
        }
        int64_t now = esp_timer_get_time();
        static uint8_t f[FRAME_MAX];
        if (s_st.role == SOLCO_LINK_HUB)
        {
            bool stream = s_stream_wanted && solco_output_mode() != 1;
            if (now >= next_beacon)
            {
                next_beacon = now + 100000;
                bool all = solco_output_mode() == 0;
                update_route(now);
                int64_t anchor = 0;
                int block = 0, queue = 0;
                solco_engine_timeline(&anchor, &block, &queue);
                int n = header(f, T_BEACON, now);
                put64(f + n, anchor);
                put32(f + n + 8, s_d_us);
                f[n + 12] = stream;
                f[n + 13] = f[n + 14] = f[n + 15] = 0;
                f[n + 16] = all ? 255 : (uint8_t)s_active_count;
                int len = n + 17;
                if (!all)
                {
                    memcpy(f + len, s_active, (size_t)s_active_count * 6);
                    len += s_active_count * 6;
                }
                send_to(broadcast, f, len, s_session_key);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_st.streaming = stream;
                s_st.d_us = s_d_us;
                xSemaphoreGive(s_lock);
                publish_beacon_fields(anchor, s_d_us, stream, all, s_active, s_active_count);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_st.active_count = all ? 0 : s_active_count;
                xSemaphoreGive(s_lock);
            }
            if (stream)
                send_audio(broadcast, T_AUDIO);
            else
                s_prev_valid = false;
        }
        if (uplink)
            send_audio(s_st.hub_mac, T_UPLINK); /* also in "hub only": the hub still plays it */
        if (s_st.role == SOLCO_LINK_MEMBER && s_st.joined && now >= next_status)
        {
            next_status = now + 500000;
            int n = header(f, T_STATUS, now);
            put32(f + n, solco_engine_idle_ms());
            send_to(s_st.hub_mac, f, n + 4, s_session_key);
        }
        if (s_st.role == SOLCO_LINK_MEMBER && s_st.joined && now >= next_sync)
        {
            next_sync = now + 250000;
            int n = header(f, T_SYNC_REQ, esp_timer_get_time());
            send_to(s_st.hub_mac, f, n, s_session_key);
        }
        size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (free_now < s_st.internal_min)
            s_st.internal_min = (uint32_t)free_now;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t solco_link_start(solco_link_role_t role, int channel, bool open)
{
    if ((role != SOLCO_LINK_HUB && role != SOLCO_LINK_MEMBER) || channel < 1 || channel > 13)
        return ESP_ERR_INVALID_ARG;
    if (s_task)
        return ESP_ERR_INVALID_STATE;
    if (!s_lock)
        s_lock = xSemaphoreCreateMutex();
    if (!s_rx)
        s_rx = xQueueCreateWithCaps(24, sizeof(rx_t), MALLOC_CAP_SPIRAM); /* 15 KB off internal RAM */
    if (!s_tap)
        s_tap = heap_caps_calloc(TAP_RING, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_slots)
        s_slots = heap_caps_calloc(SLOTS, sizeof(slot_t), MALLOC_CAP_SPIRAM);
    s_down.slots = s_slots;
    for (int i = 0; i < MEMBERS; ++i)
        if (!s_up[i].slots)
            s_up[i].slots = heap_caps_calloc(SLOTS, sizeof(slot_t), MALLOC_CAP_SPIRAM); /* 16 KB each */
    for (int i = 0; i < MEMBERS; ++i)
        if (!s_up[i].slots)
            return ESP_ERR_NO_MEM;
    if (!s_delay)
        s_delay = heap_caps_calloc(8192 * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_rx || !s_tap || !s_slots || !s_delay)
        return ESP_ERR_NO_MEM;
    STORE(&s_member_count, 0);
    for (int i = 0; i < SLOTS; ++i)
        s_slots[i].frame0 = UINT32_MAX;
    s_down.newest = 0;
    if (psa_crypto_init() != PSA_SUCCESS)
        return ESP_FAIL;
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t min_before = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t step[5] = {0};
#define MEM_STEP(i) (step[i] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT))
    esp_err_t err = ESP_OK;
    if (!s_wifi_up)
    {
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            return err;
        MEM_STEP(0);
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        config.nvs_enable = 0; /* nothing written to the shared NVS */
        err = esp_wifi_init(&config);
        MEM_STEP(1);
        if (err == ESP_OK)
            err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err == ESP_OK)
            err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK)
            err = esp_wifi_start();
        MEM_STEP(2);
        if (err == ESP_OK)
            err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err == ESP_OK)
            err = esp_now_init();
        MEM_STEP(3);
        if (err == ESP_OK)
            err = esp_now_register_recv_cb(on_recv);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_LOG, "radio start failed: %s", esp_err_to_name(err));
            esp_now_deinit();
            esp_wifi_stop();
            esp_wifi_deinit();
            return err;
        }
        s_wifi_up = true;
    }
    err = esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK)
        return err;
    int paired = load_peers(open);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_st, 0, sizeof(s_st));
    s_st.role = role;
    s_st.channel = channel;
    s_st.open = open;
    s_st.peers_paired = paired;
    s_st.internal_free_at_start = (uint32_t)before;
    s_st.internal_min = UINT32_MAX;
    s_st.d_us = s_d_us;
    s_st.mem_min_before = min_before;
    memcpy(s_st.mem_step, step, sizeof(step));
    xSemaphoreGive(s_lock);
    esp_wifi_get_mac(WIFI_IF_STA, s_my_mac);
    s_active_count = 0;
    s_seq = esp_random() >> 2; /* random start below 2^30: never wraps in practice */
    s_hub_last_seq = 0;
    s_sample_count = s_sample_next = 0;
    s_last_beacon_us = 0;
    s_tap_next = 0;
    s_prev_valid = false;
    s_down.p_valid = false;
    memset(&s_enc, 0, sizeof(s_enc));
    publish_model(0, 0, false);
    if (s_session_key)
        psa_destroy_key(s_session_key);
    s_session_key = 0;
    if (role == SOLCO_LINK_HUB)
    {
        esp_fill_random(s_session_raw, sizeof(s_session_raw));
        s_session_key = import_key(s_session_raw);
        s_st.session = esp_random() | 1;
        s_st.joined = true;
    }
    static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    ensure_peer(broadcast);
    s_stop = false;
    if (xTaskCreatePinnedToCore(link_task, "solco_link", 6144, NULL, 15, &s_task, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    MEM_STEP(4);
    memcpy(s_st.mem_step, step, sizeof(step));
    s_st.mem_min_after = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    xSemaphoreGive(s_lock);
#undef MEM_STEP
    ESP_LOGI(TAG_LOG, "%s on channel %d, %d paired, %s", role == SOLCO_LINK_HUB ? "hub" : "member", channel,
             paired, open ? "OPEN (development key)" : "ECOS keys");
    return ESP_OK;
}

void solco_link_stop(void)
{
    s_stop = true;
    for (int i = 0; i < 50 && s_task; ++i)
        vTaskDelay(pdMS_TO_TICKS(10));
    s_stream_wanted = false;
    publish_beacon_fields(0, s_d_us, false, true, NULL, 0);
    if (s_wifi_up)
    {
        esp_now_deinit();
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_up = false;
    }
    if (s_lock)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.role = SOLCO_LINK_OFF;
        s_st.joined = false;
        xSemaphoreGive(s_lock);
    }
}

void solco_link_stats(solco_link_stats_t *out)
{
    if (!s_lock)
    {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
    out->internal_free_now = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    model_t m;
    read_model(&m);
    double hub;
    int64_t now = esp_timer_get_time();
    if (out->role == SOLCO_LINK_MEMBER && hub_time(&m, now, &hub))
        out->offset_us = (int64_t)llrint(hub - (double)now);
}

/* ------------------------------------------------------------- audio task side */

/* Copies sender frames [first, first + count) from a jitter buffer: one tag check
 * per slot, not per sample. Missing frames are flagged in `ok`. */
#define SPAN_MAX (2 * SB + 8)
static void stream_fetch(const slot_t *slots, int64_t first, int count, int16_t *out, uint8_t *ok)
{
    int i = 0;
    while (i < count)
    {
        uint32_t at = (uint32_t)(first + i);
        uint32_t base = at & ~(uint32_t)(SB - 1);
        int n = (int)(base + SB - at);
        if (n > count - i)
            n = count - i;
        const slot_t *slot = &slots[(at / SB) % SLOTS];
        bool valid = LOAD(&slot->frame0) == base;
        if (valid)
        {
            memcpy(out + i, slot->pcm + (at - base), (size_t)n * sizeof(int16_t));
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            valid = RELAXED(&slot->frame0) == base; /* not rewritten while copying */
        }
        memset(ok + i, valid, (size_t)n);
        i += n;
    }
}

/* Plays `frames` output frames whose sender positions run from f0 to f1 out of a
 * jitter buffer, into `out` (mono). Returns false (out untouched) while the buffer
 * does not hold the target yet. The playhead follows the target at most 300 ppm
 * apart: never a jump; gaps decay to silence and fade back in. */
static bool stream_play(rxs_t *r, double f0, double f1, int frames, float *out, uint32_t *missing,
                        int32_t *err_us, int32_t *buffer_ms)
{
    double ratio = (f1 - f0) / frames;
    if (!r->p_valid || fabs(r->p - f0) > 2 * SB || ratio < 0.99 || ratio > 1.01)
    {
        /* (Re)start only when the jitter buffer holds the target: then fade in. */
        int16_t probe[4];
        uint8_t have_probe[4];
        stream_fetch(r->slots, (int64_t)floor(f0) - 1, 4, probe, have_probe);
        if (!(have_probe[0] && have_probe[1] && have_probe[2] && have_probe[3]))
        {
            r->p_valid = false;
            return false;
        }
        r->p = f0;
        r->p_valid = true;
        r->fade = 0;
    }
    double error = r->p - f0;
    *err_us = (int32_t)lrint(error * 1e6 / RATE);
    double correction = -error / (frames * 8.0);
    correction = correction > 3e-4 ? 3e-4 : correction < -3e-4 ? -3e-4 : correction;
    double step = ratio + correction;
    *buffer_ms = (int32_t)(((int64_t)RELAXED(&r->newest) - (int64_t)r->p) * 1000 / RATE);
    int64_t first = (int64_t)floor(r->p) - 1;
    int span = (int)ceil(step * frames) + 4;
    if (span > SPAN_MAX)
        span = SPAN_MAX;
    static int16_t src[SPAN_MAX]; /* audio task only */
    static uint8_t have[SPAN_MAX];
    stream_fetch(r->slots, first, span, src, have);
    uint32_t lost = 0;
    for (int i = 0; i < frames; ++i)
    {
        int64_t k = (int64_t)floor(r->p);
        int j = (int)(k - first);
        float fr = (float)(r->p - (double)k);
        float y;
        if (j >= 1 && j + 2 < span && have[j - 1] && have[j] && have[j + 1] && have[j + 2])
        {
            /* Cubic Hermite (Catmull-Rom). */
            float a = src[j - 1], b = src[j], c = src[j + 1], d = src[j + 2];
            float c1 = 0.5f * (c - a), c2 = a - 2.5f * b + 2.f * c - 0.5f * d, c3 = 0.5f * (d - a) + 1.5f * (b - c);
            y = ((c3 * fr + c2) * fr + c1) * fr + b;
            r->plc_last = y;
            r->fade = r->fade < 1.f ? r->fade + 1.f / 441.f : 1.f; /* 10 ms fade-in after a gap */
        }
        else
        {
            /* Concealment: decay the last sample to silence in ~10 ms. */
            r->plc_last *= 0.99f;
            y = r->plc_last;
            r->fade = 0;
            ++lost;
        }
        out[i] = y * (r->fade > 0 ? r->fade : 1.f);
        r->p += step;
    }
    *missing = lost;
    return true;
}

static inline int16_t soft_clip(float x)
{
    /* Linear up to -1.6 dBFS, then a tanh knee into full scale: summing several
     * devices never wraps and rarely hard-clips. */
    const float knee = 27000.f, room = 32767.f - knee;
    float a = x < 0 ? -x : x;
    if (a > knee)
        a = knee + room * tanhf((a - knee) / room);
    return (int16_t)(x < 0 ? -a : a);
}

/* Hub: adds every member's uplink to the master, U behind the hub line. Runs before
 * the tap and the local delay line, so the speaker of the hub (aligned to D) and
 * the downlink both carry the complete master. */
static void uplink_mix(int16_t *stereo, int frames, uint32_t frame0)
{
    int count = LOAD(&s_member_count);
    if (!count)
        return;
    static float acc[SB], one[SB]; /* audio task only */
    static uint32_t seen_reset[MEMBERS];
    int64_t now = esp_timer_get_time();
    double u_frames = (double)s_u_us * RATE / 1e6;
    int32_t worst_buffer = INT32_MAX;
    int live = 0;
    for (int at = 0; at < frames; at += SB)
    {
        int n = frames - at < SB ? frames - at : SB;
        bool any = false;
        for (int m = 0; m < count; ++m)
        {
            rxs_t *r = &s_up[m];
            uint32_t reset = LOAD(&s_up_reset[m]);
            if (reset != seen_reset[m])
            {
                seen_reset[m] = reset;
                r->p_valid = false;
            }
            upmap_t map;
            uint32_t seq;
            do
            {
                seq = LOAD(&s_upmap[m].seq);
                map = s_upmap[m];
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
            } while ((seq & 1) || RELAXED(&s_upmap[m].seq) != seq);
            if (!map.at_us || now - map.at_us > 300000)
            {
                r->p_valid = false; /* member silent or gone: no endless concealment */
                continue;
            }
            double h0 = (double)(uint32_t)(frame0 + (uint32_t)at) - u_frames;
            double f0 = map.mf + (h0 - map.hf) / map.rate;
            double f1 = f0 + n / map.rate;
            uint32_t missing = 0;
            int32_t err = 0, buffered = 0;
            if (!stream_play(r, f0, f1, n, one, &missing, &err, &buffered))
                continue;
            if (!any)
                memset(acc, 0, (size_t)n * sizeof(float));
            any = true;
            for (int i = 0; i < n; ++i)
                acc[i] += one[i];
            s_st.up_plc_frames += missing;
            if (buffered < worst_buffer)
                worst_buffer = buffered;
            if (!at)
                ++live;
        }
        if (!any)
            continue;
        for (int i = 0; i < n; ++i)
        {
            int16_t *o = stereo + (at + i) * 2;
            o[0] = soft_clip(o[0] + acc[i]);
            o[1] = soft_clip(o[1] + acc[i]);
        }
    }
    s_st.up_live = live;
    if (worst_buffer != INT32_MAX)
        s_st.up_buffer_ms = worst_buffer;
}

/* Uplink timing. Member frame m is stamped with the hub frame written at the same
 * instant (both render lines, through the clock model); the hub mixes it at that
 * hub frame + U. While in a kit the member's mix is no longer played locally: it
 * comes back, with everybody else's, in the master at D. */
void solco_stream_tap(int16_t *stereo, int frames, uint32_t frame0)
{
    if (!s_tap)
        return;
    bool hub = s_st.role == SOLCO_LINK_HUB;
    if (hub)
        uplink_mix(stereo, frames, frame0);
    if (hub ? !s_stream_wanted : !(s_st.role == SOLCO_LINK_MEMBER && s_st.joined))
        return;
    for (int i = 0; i < frames; ++i)
        s_tap[(frame0 + (uint32_t)i) & (TAP_RING - 1)] = (int16_t)(((int32_t)stereo[i * 2] + stereo[i * 2 + 1]) / 2);
    STORE(&s_tap_end, frame0 + (uint32_t)frames);
}

int solco_stream_local_delay(void)
{
    /* The hub's own speaker plays D after render, like every member (D-R3). */
    if (s_st.role != SOLCO_LINK_HUB || !s_stream_wanted || solco_output_mode() == 1)
        return 0;
    int frames = (int)((int64_t)s_d_us * RATE / 1000000);
    return frames < 8191 ? frames : 8191;
}

/* Hub only: delay its local output by solco_stream_local_delay() frames. */
void solco_stream_delay(int16_t *stereo, int frames);
void solco_stream_delay(int16_t *stereo, int frames)
{
    int delay = solco_stream_local_delay();
    if (!delay || !s_delay)
        return;
    model_t m;
    read_model(&m);
    float target = routed_here(&m) ? 1.f : 0.f; /* "follow the mixer" on the hub's speaker */
    s_st.audible = target > 0;
    for (int i = 0; i < frames; ++i)
    {
        s_route_gain += (target - s_route_gain) * (1.f / 2205.f); /* ~50 ms */
        uint32_t w = s_delay_pos & 8191, r = (s_delay_pos - (uint32_t)delay) & 8191;
        int16_t l = stereo[i * 2], rr = stereo[i * 2 + 1];
        stereo[i * 2] = (int16_t)(s_delay[r * 2] * s_route_gain);
        stereo[i * 2 + 1] = (int16_t)(s_delay[r * 2 + 1] * s_route_gain);
        s_delay[w * 2] = l;
        s_delay[w * 2 + 1] = rr;
        ++s_delay_pos;
    }
}

void solco_stream_render(int16_t *stereo, int frames, uint32_t frame0, int64_t anchor_offset_us)
{
    if (!s_slots || s_st.role != SOLCO_LINK_MEMBER)
        return;
    model_t m;
    read_model(&m);
    s_st.converged = m.converged;
    /* In a kit (joined, clock converged) the own mix goes up to the hub and this
     * speaker plays only the master: no double, no echo. Out of a kit, local. */
    bool in_kit = s_st.joined && m.converged;
    if (!in_kit && s_local_gain >= 1.f)
    {
        s_down.p_valid = false;
        s_route_gain = 0;
        s_st.audible = true;
        return;
    }
    static float mono[2 * SB]; /* audio task only */
    bool have = false;
    float route = 0;
    if (in_kit && m.stream && frames <= 2 * SB)
    {
        route = routed_here(&m) ? 1.f : 0.f;
        /* Target hub frames for this block: our I2S line -> hub time -> minus D -> hub I2S line. */
        double t0 = anchor_offset_us + (double)frame0 * 1e6 / RATE;
        double t1 = t0 + (double)frames * 1e6 / RATE;
        double h0 = 0, h1 = 0;
        hub_time(&m, (int64_t)t0, &h0);
        hub_time(&m, (int64_t)t1, &h1);
        double f0 = (h0 - m.d_us - (double)m.hub_anchor_us) * RATE / 1e6;
        double f1 = (h1 - m.d_us - (double)m.hub_anchor_us) * RATE / 1e6;
        uint32_t missing = 0;
        int32_t err_us = 0, buffered = 0;
        have = stream_play(&s_down, f0, f1, frames, mono, &missing, &err_us, &buffered);
        if (have)
        {
            s_st.phase_err_us = err_us;
            if (abs(err_us) > abs(s_st.phase_err_max_us) && s_st.aud_rx > 50)
                s_st.phase_err_max_us = err_us;
            s_st.buffer_ms = buffered;
            s_st.aud_plc_frames += missing;
        }
    }
    else
        s_down.p_valid = false; /* "hub only" or not in sync yet: muted, the uplink goes on */
    /* Mute the own mix only while the hub master really plays here: if the
     * downlink is missing or routed away, the local mix stays audible. */
    float local = (in_kit && have && route > 0) ? 0.f : 1.f;
    s_st.audible = true;
    for (int i = 0; i < frames; ++i)
    {
        s_local_gain += (local - s_local_gain) * (1.f / 2205.f); /* ~50 ms hand-over */
        if (local > 0.5f && s_local_gain > 0.999f)
            s_local_gain = 1.f;
        s_route_gain += ((have ? route : 0.f) - s_route_gain) * (1.f / 2205.f); /* routing changes: ~50 ms */
        float v = have ? mono[i] * s_route_gain : 0.f;
        for (int ch = 0; ch < 2; ++ch)
        {
            float o = stereo[i * 2 + ch] * s_local_gain + v;
            stereo[i * 2 + ch] = (int16_t)(o > 32767 ? 32767 : o < -32768 ? -32768 : o);
        }
    }
}

/* ------------------------------------------------------------- console */

bool solco_link_console(const char *line)
{
    int role, channel, value;
    char extra[8] = {0};
    int n = sscanf(line, "link-start %d %d %7s", &role, &channel, extra);
    if (n >= 2)
    {
        esp_err_t err = solco_link_start((solco_link_role_t)role, channel, n == 3 && !strcmp(extra, "open"));
        printf("@LINKSTART %s\n", esp_err_to_name(err));
        printf("@OK link-start\n");
        return true;
    }
    if (!strcmp(line, "link-stop"))
    {
        solco_link_stop();
        printf("@OK link-stop\n");
        return true;
    }
    if (sscanf(line, "stream-d %d", &value) == 1 && value >= 10 && value <= 180)
    {
        s_d_us = (uint32_t)value * 1000;
        printf("@OK stream-d\n");
        return true;
    }
    if (sscanf(line, "stream-u %d", &value) == 1 && value >= 10 && value <= 120)
    {
        s_u_us = (uint32_t)value * 1000;
        printf("@OK stream-u\n");
        return true;
    }
    if (sscanf(line, "stream %d", &value) == 1)
    {
        s_stream_wanted = value != 0;
        printf("@OK stream\n");
        return true;
    }
    if (!strcmp(line, "link"))
    {
        solco_link_stats_t st;
        solco_link_stats(&st);
        printf("@LINK role=%d ch=%d open=%d joined=%d paired=%d session=%08lx tx=%lu txerr=%lu rx=%lu "
               "badtag=%lu replay=%lu joins=%lu beacons=%lu beacon_lost=%lu\n",
               st.role, st.channel, st.open, st.joined, st.peers_paired, (unsigned long)st.session,
               (unsigned long)st.tx, (unsigned long)st.tx_errors, (unsigned long)st.rx,
               (unsigned long)st.rx_bad_tag, (unsigned long)st.rx_replay, (unsigned long)st.joins,
               (unsigned long)st.beacons, (unsigned long)st.beacon_lost);
        printf("@CLOCK sync_ok=%lu rejected=%lu rtt_min=%lu rtt_last=%lu rtt_max=%lu offset_us=%lld "
               "skew_ppb=%ld residual_us=%lu\n",
               (unsigned long)st.sync_ok, (unsigned long)st.sync_rejected, (unsigned long)st.rtt_min_us,
               (unsigned long)st.rtt_last_us, (unsigned long)st.rtt_max_us, (long long)st.offset_us,
               (long)st.skew_ppb, (unsigned long)st.residual_us);
        printf("@STREAM on=%d d_ms=%lu tx=%lu rx=%lu recovered=%lu plc_frames=%lu buffer_ms=%ld phase_us=%ld "
               "phase_max_us=%ld enc_us=%lu dec_us=%lu peak=%u tx_peak=%u\n",
               st.streaming, (unsigned long)(st.d_us / 1000), (unsigned long)st.aud_tx, (unsigned long)st.aud_rx,
               (unsigned long)st.aud_recovered, (unsigned long)st.aud_plc_frames, (long)st.buffer_ms,
               (long)st.phase_err_us, (long)st.phase_err_max_us, (unsigned long)st.encode_us_max,
               (unsigned long)st.decode_us_max, st.rx_peak, st.tx_peak);
        printf("@UPLINK u_ms=%lu tx=%lu rx=%lu recovered=%lu plc_frames=%lu buffer_ms=%ld live=%d peak=%u\n",
               (unsigned long)(s_u_us / 1000), (unsigned long)st.up_tx, (unsigned long)st.up_rx,
               (unsigned long)st.up_recovered, (unsigned long)st.up_plc_frames, (long)st.up_buffer_ms, st.up_live,
               st.up_peak);
        printf("@LINKMEM start=%lu now=%lu min=%lu steps=%lu/%lu/%lu/%lu/%lu heap_min=%lu->%lu\n",
               (unsigned long)st.internal_free_at_start, (unsigned long)st.internal_free_now,
               (unsigned long)st.internal_min, (unsigned long)st.mem_step[0], (unsigned long)st.mem_step[1],
               (unsigned long)st.mem_step[2], (unsigned long)st.mem_step[3], (unsigned long)st.mem_step[4],
               (unsigned long)st.mem_min_before, (unsigned long)st.mem_min_after);
        printf("@ROUTE mode=%d converged=%d audible=%d active=%d idle_ms=%lu\n", solco_output_mode(), st.converged,
               st.audible, st.active_count, (unsigned long)solco_engine_idle_ms());
        printf("@OK link\n");
        return true;
    }
    return false;
}
