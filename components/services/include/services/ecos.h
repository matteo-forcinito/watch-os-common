#ifndef SERVICES_ECOS_H
#define SERVICES_ECOS_H

/* Explicit, three-minute ESP-NOW pairing (optional LAN by peer IPv4).
 * One transient worker owns crypto, radio and NVS. UI calls never wait for radio.
 * Pairwise trust only: no group key or HID permissions are granted here. */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define ECOS_PEERS_MAX 6
#define ECOS_NAME_SIZE 24

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
    char ssid[33];
    char ip[16];
    uint32_t uptime_min;
    unsigned ram_kb;
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
/* The dashboard looks: keep the radio on for the next 90 seconds. */
void ecos_family_touch(void);
/* Search the paired devices now (all channels). Result in ecos_family(). */
esp_err_t ecos_family_refresh(void);
esp_err_t ecos_family_ring(const char *id, bool on);
esp_err_t ecos_family_message(const char *id, const char *text);
/* This device stopped ringing (the user found it). */
void ecos_family_ring_done(void);
/* The apps layer shows what others ask (ring). Called in the link task: post only. */
void ecos_family_set_handler(void (*handler)(const ecos_event_t *event));

#endif
