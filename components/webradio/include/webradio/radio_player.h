#ifndef SERVICES_RADIO_PLAYER_H
#define SERVICES_RADIO_PLAYER_H

/*
 * Web radio player: plays an internet radio URL on the speaker.
 * (Ported from the e-paper clock project, now stereo.)
 *
 *   network task (core 0)                 decoder task (core 1)
 *   ┌──────────────────────────┐          ┌──────────────────────────┐
 *   │ Wi-Fi (known networks)   │  bytes   │ prebuffer ~2 s           │
 *   │ HTTP(S), redirects       │ ───────► │ MP3 / AAC / HE-AAC / TS  │──► speaker
 *   │ .pls/.m3u, HLS segments  │  384 KB  │ (esp_audio_codec)        │    (stereo)
 *   │ ICY song titles          │  buffer  │                          │
 *   └──────────────────────────┘ in PSRAM └──────────────────────────┘
 *
 * The caller (a screen, the alarm) is never blocked by the network: it polls
 * the state, or observes STATE_RADIO / STATE_RADIO_VERSION in the UI.
 *
 * With play_when_ready = false the stream connects silently and the speaker
 * is used only after radio_player_play(): the alarm uses this to fall back
 * to a beep if the radio does not start.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum
{
    RADIO_STATE_IDLE,
    RADIO_STATE_WIFI,         /* joining a Wi-Fi network */
    RADIO_STATE_CONNECTING,   /* opening the stream */
    RADIO_STATE_BUFFERING,
    RADIO_STATE_READY,        /* buffered, waiting for radio_player_play() */
    RADIO_STATE_PLAYING,
    RADIO_STATE_FAILED,       /* see radio_player_state_text() */
} radio_state_t;

/* Start playing `url` in the background. ESP_ERR_INVALID_STATE while the previous one is still closing. */
esp_err_t radio_player_start(const char *url, int volume, bool play_when_ready);

/* Allow the audio out (if started with play_when_ready = false). */
void radio_player_play(void);

/* Stop and release speaker and Wi-Fi. Blocks until the decoder let go of the speaker (< 2 s). */
void radio_player_stop(void);

/* True while the tasks of a previous station are still closing. */
bool radio_player_busy(void);

radio_state_t radio_player_state(void);

/* "Connessione...", "In riproduzione", or why it failed. */
const char *radio_player_state_text(void);

/* Song title sent by the station (ICY metadata). False if none. */
bool radio_player_title(char *out, size_t size);

/* e.g. "AAC 44 kHz stereo" (empty until playing). */
void radio_player_format_text(char *out, size_t size);

void radio_player_set_volume(int percent);

/* Milliseconds since the last audio reached the speaker (0 before playing). */
uint32_t radio_player_silence_ms(void);

#endif
