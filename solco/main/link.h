#ifndef SOLCO_LINK_H
#define SOLCO_LINK_H
/* ESP-NOW link between Solco devices (F5 + F5b MVP).
 *
 *   hub     owns the session: random session key handed to each paired member,
 *           BEACON 10 Hz (I2S frame/time line, D), SYNC responses, and when the
 *           output mode asks for it an ADPCM mono stream of the master
 *           (post-mix, pre-volume) with one block of redundancy.
 *   member  joins with its ECOS pairing (read only), models the hub clock
 *           (offset + skew), plays the stream at hub frame time + D through a
 *           jitter buffer and a continuously re-timed interpolator. In a kit it
 *           sends its own mix (pre-volume, ADPCM, same redundancy) up to the hub,
 *           which mixes it into the master U later, and plays only the master.
 *
 * Every frame carries an 8-byte HMAC-SHA256 tag: pair keys derive from the ECOS
 * credential (HMAC(credential, "SLNK")), the session key is sent encrypted with
 * them. The audio task only calls solco_stream_tap / solco_stream_render: both
 * are lock-free and never wait. */
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    SOLCO_LINK_OFF = 0,
    SOLCO_LINK_HUB = 1,
    SOLCO_LINK_MEMBER = 2,
} solco_link_role_t;

typedef struct
{
    int role, channel;
    bool open;                 /* development mode without ECOS keys */
    bool joined;               /* member: session key received */
    int peers_paired;          /* ECOS records found */
    uint8_t hub_mac[6];
    uint32_t session;
    uint32_t tx, tx_errors, rx, rx_bad_tag, rx_replay, joins;
    uint32_t beacons, beacon_lost;
    uint32_t sync_ok, sync_rejected;
    uint32_t rtt_min_us, rtt_last_us, rtt_max_us;
    int64_t offset_us;         /* hub - local at now */
    int32_t skew_ppb;
    uint32_t residual_us;      /* RMS of the clock fit */
    uint32_t internal_free_at_start, internal_free_now, internal_min;
    /* stream */
    bool streaming;
    uint32_t d_us;
    uint32_t aud_tx, aud_rx, aud_recovered, aud_lost_blocks, aud_late, aud_plc_frames;
    int32_t phase_err_us, phase_err_max_us; /* interpolator vs target, last and worst */
    int32_t buffer_ms;         /* receiver: audio buffered ahead of the playhead */
    uint32_t encode_us_max, decode_us_max;
    uint16_t rx_peak, tx_peak;
    bool converged;            /* member: clock model good enough to play */
    bool audible;              /* this device's speaker is in the current routing */
    int active_count;          /* hub: devices routed by "follow the mixer" (0 = all) */
    uint32_t mem_step[5];      /* internal free after: event loop, wifi init, wifi start, esp-now, task */
    uint32_t mem_min_before, mem_min_after; /* heap minimum around solco_link_start */
    /* uplink (member -> hub: the member's local mix, mixed into the hub master) */
    uint32_t up_tx;            /* member: blocks sent */
    uint32_t up_rx, up_recovered, up_plc_frames; /* hub: all members */
    int32_t up_buffer_ms;      /* hub: least audio buffered ahead among the members */
    int up_live;               /* hub: members currently mixed */
    uint16_t up_peak;
} solco_link_stats_t;

esp_err_t solco_link_start(solco_link_role_t role, int channel, bool open);
void solco_link_stop(void);
void solco_link_stats(solco_link_stats_t *stats);
/* Development console: link-start, link-stop, link, stream, stream-d. */
bool solco_link_console(const char *line);

/* Audio task hooks (lock-free). frame0 = engine (studio) frame of the block. */
/* Hub: mixes the members' uplinks into `stereo` (the master) before tapping it. */
void solco_stream_tap(int16_t *stereo, int frames, uint32_t frame0);
void solco_stream_render(int16_t *stereo, int frames, uint32_t frame0, int64_t anchor_offset_us);
/* Hub: frames by which its own speaker is delayed to stay aligned with the members. */
int solco_stream_local_delay(void);
#endif
