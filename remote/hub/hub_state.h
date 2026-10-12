#ifndef REMOTE_HUB_STATE_H
#define REMOTE_HUB_STATE_H

/* Pure C state engine. One worker owns it; radio callbacks only queue packets.
 * A slot and its session are installed by authenticated pairing, never by an
 * incoming packet. This layer does not authenticate or encrypt radio traffic. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HUB_NODES 5 /* Local controls and four satellites. */
#define HUB_PACKET_SIZE 24
#define HUB_TIMEOUT_MS 500

typedef struct
{
    uint64_t session;
    uint32_t sequence;
    uint32_t last_ms;
    bool authorized;
    bool seen;
    uint8_t keyboard[8];
    uint16_t media;
    uint8_t buttons;
    uint8_t gamepad[4];
    int32_t motion[4];
} hub_node_t;

typedef struct
{
    hub_node_t nodes[HUB_NODES];
    uint32_t generation[4];
    uint32_t motion_overflows;
    uint8_t dirty;
} hub_state_t;

typedef struct
{
    uint32_t generation;
    uint8_t id;
    uint8_t length;
    uint8_t data[8];
} hub_report_t;

typedef enum
{
    HUB_ACCEPTED,
    HUB_DUPLICATE,
    HUB_INVALID,
    HUB_UNAUTHORIZED,
} hub_result_t;

void hub_state_init(hub_state_t *state);
bool hub_state_authorize(hub_state_t *state, unsigned node, uint64_t session);
void hub_state_revoke(hub_state_t *state, unsigned node);
hub_result_t hub_state_receive(hub_state_t *state, unsigned node,
                                      const uint8_t *packet, size_t length, uint32_t now_ms);
void hub_state_expire(hub_state_t *state, uint32_t now_ms);
/* Snapshot is non-destructive. Commit only after USB accepted this report.
 * No receive/expire/revoke may run between snapshot and commit. Failed sends
 * leave all state pending. Each sender also needs bounded retries and ACKs. */
bool hub_state_snapshot(const hub_state_t *state, uint8_t id, hub_report_t *report);
bool hub_state_commit(hub_state_t *state, const hub_report_t *report);
/* USB disconnect: revoke every session and discard motion. Pairing keys can
 * persist elsewhere, but a new session handshake is required before input. */
void hub_state_disconnect(hub_state_t *state);

#endif
