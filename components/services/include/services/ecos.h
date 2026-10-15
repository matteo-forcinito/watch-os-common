#ifndef SERVICES_ECOS_H
#define SERVICES_ECOS_H

/* Explicit, three-minute LAN pairing. No background listener at rest.
 * One transient worker owns crypto, UDP and NVS. UI calls never wait for radio.
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
/* Optional private IPv4 peer address; NULL/empty uses LAN broadcast discovery. */
esp_err_t ecos_pair_start(const char *peer_ip);
/* Confirmation is bound to the snapshot's candidate and code (reject stale UI). */
esp_err_t ecos_pair_confirm(const char *id, uint32_t code);
void ecos_pair_cancel(void);
void ecos_snapshot(ecos_snapshot_t *out);

#endif
