#ifndef HID_HUB_H
#define HID_HUB_H

/*
 * Remote Hub: several watches act as ONE set of HID devices on a computer.
 *
 *   hub        connected to the PC by USB or Bluetooth (the normal Remote link);
 *              merges its own controls with up to four satellites
 *   satellite  any other watch with Remote: its keyboard / mouse / air mouse /
 *              gamepad reports travel by radio (ESP-NOW) to the hub
 *
 * Pairing is done on the watches: the hub opens a window, the satellite picks
 * it, both show the same 6-digit code (P-256 ECDH, compared by the user).
 * Every radio frame is AES-128-GCM encrypted and authenticated with the pair
 * key, bound to both MAC addresses; sessions and sequence numbers stop replays.
 * Role changes take effect after restarting the app.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define HID_HUB_PEERS 4
#define HID_HUB_NAME 24

typedef enum
{
    HID_HUB_OFF,
    HID_HUB_HUB,
    HID_HUB_SATELLITE,
} hid_hub_role_t;

typedef enum
{
    HID_HUB_PAIR_IDLE,
    HID_HUB_PAIR_OPEN,      /* hub: waiting for a satellite; satellite: searching */
    HID_HUB_PAIR_CODE,      /* both: compare the code and confirm */
    HID_HUB_PAIR_CONFIRMED, /* this side confirmed, waiting for the other */
    HID_HUB_PAIR_DONE,
    HID_HUB_PAIR_FAILED,
} hid_hub_pair_phase_t;

typedef struct
{
    hid_hub_pair_phase_t phase;
    char peer[HID_HUB_NAME];
    uint32_t code;
    int seconds_left;
    char message[64];
} hid_hub_pairing_t;

typedef struct
{
    char name[HID_HUB_NAME];
    uint8_t mac[6];
    int rssi;
    bool connected;
} hid_hub_peer_info_t;

typedef struct
{
    uint32_t received, retries, dropped, rejected;
} hid_hub_stats_t;

/* Saved role (NVS "remote_hub"). Start the radio once, after hid_link_start(). */
hid_hub_role_t hid_hub_role(void);
esp_err_t hid_hub_set_role(hid_hub_role_t role);
esp_err_t hid_hub_start(const char *name);

/* Paired watches (hub: satellites; satellite: its hub), with link state. */
int hid_hub_peers(hid_hub_peer_info_t *out, int max);
esp_err_t hid_hub_forget(int index);

/* Pairing. Satellite: hubs found while searching, then pick one. */
esp_err_t hid_hub_pair_open(void);
int hid_hub_pair_found(hid_hub_peer_info_t *out, int max);
esp_err_t hid_hub_pair_select(int index);
void hid_hub_pair_confirm(bool accept);
void hid_hub_pair_cancel(void);
void hid_hub_pairing(hid_hub_pairing_t *out);

void hid_hub_stats(hid_hub_stats_t *stats);

#endif
