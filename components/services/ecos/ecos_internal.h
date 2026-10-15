#ifndef ECOS_INTERNAL_H
#define ECOS_INTERNAL_H

/* Shared between the pairing (ecos.c) and the Famiglia link (ecos_link.c). */
#include <stdbool.h>
#include <stdint.h>
#include "services/ecos.h"

/* NVS "ecos"/"peers": version, count, then records of MAC(6) name(24) credential(32). */
#define ECOS_RECORD_SIZE (6 + ECOS_NAME_SIZE + 32)
#define ECOS_STORE_SIZE (2 + ECOS_PEERS_MAX * ECOS_RECORD_SIZE)

bool ecos_store_load(uint8_t store[ECOS_STORE_SIZE]);

/* Pairing needs ESP-NOW for itself: the link lets go of it while suspended. */
void ecos_link_suspend(bool suspend);
/* True while the link still owns ESP-NOW. */
bool ecos_link_running(void);
/* The paired list changed (pairing, forget): reload the keys. */
void ecos_link_peers_changed(void);
/* Boot: timers only, no task, no radio. */
void ecos_link_init(void);

#endif
