#ifndef SERVICES_ECOS_H
#define SERVICES_ECOS_H

/* Explicit, three-minute ESP-NOW pairing (optional LAN by peer IPv4).
 * One transient worker owns crypto, radio and NVS. UI calls never wait for radio.
 * Pairwise trust only: no group key or HID permissions are granted here. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ECOS_PEERS_MAX 6
#define ECOS_NAME_SIZE 24
#define ECOS_PERMISSION_WEB_CODE 1U
#define ECOS_PERMISSION_RESOURCES 2U

typedef struct
{
    char name[ECOS_NAME_SIZE];
    char id[13];
} ecos_peer_info_t;

typedef struct
{
    bool running;
    bool comparing;
    bool local_ok;
    bool remote_ok;
    bool saved;
    unsigned seconds_left;
    uint32_t code;
    char candidate[ECOS_NAME_SIZE];
    char candidate_id[13];
    char message[112];
    int count;
    ecos_peer_info_t peers[ECOS_PEERS_MAX];
} ecos_snapshot_t;

/* Once at system startup, before apps are registered; only reads small NVS data. */
void ecos_init(void);
/* Optional private IPv4 peer address; NULL/empty uses direct ESP-NOW discovery. */
esp_err_t ecos_pair_start(const char *peer_ip);
/* Bonded/encrypted BLE transport for the exact same EC01 handshake.
 * Callback runs on the pairing task; NULL/0 is its final lifecycle notification.
 * On successful start the context must live until that notification. A failed
 * start never invokes it. Neither callback nor receive may block on the UI. */
typedef bool (*ecos_pair_ble_send_t)(const uint8_t *frame, size_t length, void *context);
esp_err_t ecos_pair_ble_start(ecos_pair_ble_send_t send, void *context);
bool ecos_pair_ble_receive(const uint8_t *frame, size_t length, void *context);
void ecos_pair_ble_disconnect(void *context);
/* Confirmation is bound to the snapshot's candidate and code (reject stale UI). */
esp_err_t ecos_pair_confirm(const char *id, uint32_t code);
void ecos_pair_cancel(void);
void ecos_snapshot(ecos_snapshot_t *out);
/* Remove a paired device here (the other side keeps its record until it forgets too). */
esp_err_t ecos_forget(const char *id);

/* ---- Famiglia: the paired devices, their info, and things to do with them.
 *
 * Device to device over ESP-NOW, every frame sealed with the pair's key
 * (AES-128-GCM, one key per direction, replay checked). The radio is on only:
 *   - while someone looks (the Ecosistema app, the dashboard page), or
 *   - on external power with the screen off (Watcher, Knob, a watch charging):
 *     then the device answers the others ("reachable").
 * A battery device with the screen on is not reachable: no Wi-Fi kept on. */

typedef struct
{
    char id[13];
    char name[ECOS_NAME_SIZE];
    bool known;            /* info received at least once since boot */
    bool reachable;        /* answered the last search */
    uint32_t seen_ago;     /* seconds since the last frame; UINT32_MAX = never */
    int rssi;
    uint8_t channel;
    char product[32];
    char version[16];
    int battery;           /* -1 unknown */
    bool charging, usb, listening, wifi, ringing;
    bool recovery;
    bool companion;        /* Authenticated phone Companion reachable through this gateway. */
    bool powered;         /* External supply, including boards without a PMU. */
    char ssid[33];
    char ip[16];
    uint32_t uptime_min;
    unsigned ram_kb;
    uint32_t permissions; /* What this local device grants to that member. */
    bool access_pending, access_known;
    uint8_t access_operation, access_result, resource_flags;
    uint32_t access_age_s, sample_age_ms;
    char web_code[7];
    uint16_t battery_mv, resource_ram_kb;
    int resource_battery;
    int16_t acceleration_mg[3], angular_dps[3];
} ecos_member_t;

typedef struct
{
    bool radio;            /* the link is running here */
    bool busy;             /* a search or an action is in progress */
    bool reachable_here;   /* this device answers the others right now */
    int count;
    ecos_member_t members[ECOS_PEERS_MAX];
    char message[96];      /* the last search / action result */
} ecos_family_t;

typedef enum
{
    ECOS_EVENT_RING,
    ECOS_EVENT_RING_STOP,
} ecos_event_kind_t;

typedef struct
{
    ecos_event_kind_t kind;
    char from[ECOS_NAME_SIZE];
} ecos_event_t;

void ecos_family(ecos_family_t *out);
/* The UI looks at the family: keep the radio on (reference counted, never blocks). */
void ecos_family_hold(bool on);
bool ecos_family_hold_active(void); /* Conditional hold; never starts the radio. */
/* The dashboard looks: keep the radio on for the next 90 seconds. */
void ecos_family_touch(void);
/* Search the paired devices now (all channels). Result in ecos_family(). */
esp_err_t ecos_family_refresh(void);
/* Explicit per-member grants, credential-bound in NVS; default deny. */
esp_err_t ecos_permissions_set(const char *id, uint32_t permissions);
/* Request another member's grant: 1 = web code, 2 = current resources. */
esp_err_t ecos_family_request(const char *id, uint8_t operation);
esp_err_t ecos_family_ring(const char *id, bool on);
esp_err_t ecos_family_message(const char *id, const char *text);
/* This device stopped ringing (the user found it). */
void ecos_family_ring_done(void);
/* The apps layer shows what others ask (ring). Called in the link task: post only. */
void ecos_family_set_handler(void (*handler)(const ecos_event_t *event));

/* Pets travel between paired devices (docs/ORBI-PETS-ROADMAP.md, phase 1).
 * Steps are opaque bytes up to ECOS_PET_MAX, sealed like every frame. */
#define ECOS_PET_MAX 120
/* Runs in the link task for each step received: answer into reply, return its
 * length (0 = no answer). Keep it short: one store command, no UI. */
typedef size_t (*ecos_pet_handler_t)(const char *from_id, const char *from_name, const uint8_t *step, size_t length,
                                     uint8_t *reply, size_t max);
void ecos_family_set_pet_handler(ecos_pet_handler_t handler);
/* Sends a step and waits for the answer (blocking: call from a task, never the
 * LVGL one). Returns the answer length, or -1 if the device did not answer. */
int ecos_family_pet(const char *id, const uint8_t *step, size_t length, uint8_t *reply, size_t max, int timeout_ms);

/* Orbit RPC over the paired, authenticated EL02 transport. The handler runs
 * on the family worker; never call the blocking client from that handler or UI.
 * Zero means no reply. Oversized requests/replies are rejected, never truncated.
 * A timeout is an unknown outcome: callers must not blindly retry side effects. */
#define ECOS_ORBIT_MAX 144
typedef size_t (*ecos_orbit_handler_t)(const char *from_id, const uint8_t *request, size_t length,
                                      uint8_t *reply, size_t max);
void ecos_family_set_orbit_handler(ecos_orbit_handler_t handler);
int ecos_family_orbit(const char *id, const uint8_t *request, size_t length,
                      uint8_t *reply, size_t max, int timeout_ms);

/* Authenticated EL02 transport over a bonded BLE session for one already paired
 * peer. The application supplies a bounded enqueue callback and owns its context
 * until close returns. The callback runs on the family worker; true confirms
 * local enqueue, not remote delivery. A callback with (frame == NULL, length == 0)
 * means the family lease ended (for example after startup failure or suspension);
 * the caller still owns the context and must close it explicitly. */
typedef bool (*ecos_family_ble_send_t)(const uint8_t *frame, size_t length, void *context);
bool ecos_family_ble_open(ecos_family_ble_send_t send, void *context, const uint8_t mac[6]);
bool ecos_family_ble_receive(const uint8_t *frame, size_t length, void *context);
void ecos_family_ble_close(void *context);

/* True only while the paired BLE Companion phone has a fresh authenticated
 * EL02 status through this gateway. `id` may be NULL; otherwise receives its
 * 12-character paired MAC ID. */
bool ecos_family_companion(char id[13]);

/* One authenticated, lossy stream target. The application owns its family
 * hold and session token. Enqueue replaces an unsent packet; true confirms
 * local acceptance only. Handler runs on the family task, never the Wi-Fi one. */
#define ECOS_STREAM_MAX 142
typedef void (*ecos_stream_handler_t)(const char *from_id, const uint8_t *data, size_t length);
void ecos_family_set_stream_handler(ecos_stream_handler_t handler);
bool ecos_family_stream_open(const char *id);
void ecos_family_stream_close(const char *id);
bool ecos_family_stream_send(const char *id, const uint8_t *data, size_t length);
bool ecos_family_stream_active(void);

#endif
