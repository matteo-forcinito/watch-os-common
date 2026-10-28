#ifndef SOLCO_ENGINE_H
#define SOLCO_ENGINE_H

#include "esp_err.h"
#include "studio.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef SOLCO_OUTPUT_CHANNELS
#define SOLCO_OUTPUT_CHANNELS 2
#endif
#if SOLCO_OUTPUT_CHANNELS != 1 && SOLCO_OUTPUT_CHANNELS != 2
#error "Solco requires a mono or stereo output profile"
#endif

typedef struct
{
    bool playing[2], keylock[2];
    bool looping[2], loop_marked[2];
    uint32_t loop_in[2], loop_out[2];
    bool scratching[2], ready[2], preparing[2];
    int prepare_percent[2];
    int32_t speed[2], rate[2]; /* Q16 */
    int32_t rate_ppm[2];       /* fine pitch relative to 1x, ppm */
    uint32_t frames[2], platter_phase[2];
    uint32_t buffered[2];
    uint32_t miss_frame[2], miss_tag[2];
    char title[2][64];
    char error[64];
    int cross;
    int volume;
    uint16_t peak;
    uint16_t wave[2][64]; /* Peak envelope of the buffered audio, starting at the playhead. */
    uint8_t wave_bands[2][64][3]; /* low / mid / high peaks of the same pages, 0..255 */
    int track[2];                 /* library index of the loaded track (0, 1 = demos) */
    uint32_t length[2];           /* frames */
    bool shift_pending[2];        /* a loop shift waits for the next loop edge */
    uint8_t hotcue_set[2];        /* bit mask of hot cues 1-4 */
    uint32_t hotcue_frame[2][4];
    bool rec_active;              /* recorder (F3): always false until it exists */
    uint32_t rec_frames, rec_gaps;
    uint32_t underruns;
    uint32_t audio_errors;
    uint32_t mix_us;
    uint32_t cycle_us, late_cycles;
    uint32_t blocks;
    uint32_t clips;
    int profile, block, queue_frames; /* latency profile, engine block, DMA queue */
} solco_status_t;

bool solco_studio_submit_at(studio_event_t event, int64_t time_us);
void solco_studio_command(int type, int value, int velocity);
void solco_studio_local_status(studio_status_t *status);
void solco_studio_status(studio_status_t *status);
bool solco_studio_saved(void);
uint32_t solco_studio_init_us(void);
void solco_motion_calibrate(void);
void solco_motion_state(int *learned, int *pad);
void solco_motion_mode(int mode);
int solco_motion_get(void);
void solco_motion_stats(uint32_t *reads, uint32_t *errors, uint32_t *hits);
void solco_engine_sync_init(void);
esp_err_t solco_engine_start(void);
bool solco_toggle(int deck);
bool solco_cue(int deck);
bool solco_next(int deck);
int solco_library_count(void);
void solco_library_title(int track, char *title, size_t length);
bool solco_select(int deck, int track);
/* phase: 0 grab, 1 relative move in source frames, 2 release. */
void solco_scratch(int deck, int phase, int32_t frames);
void solco_jog(int deck, int steps);
bool solco_loop(int deck, int action);
bool solco_rate(int deck, int32_t rate_q16);
/* Continuous rate, Q32 (1x = 1 << 32): pitch faders and clock trims; |rate| 1/16x..4x. */
bool solco_rate_fine(int deck, int64_t rate_q32);
/* Mix time histogram in eighths of the block budget, output restarts, queue drops. */
void solco_mix_histogram(uint32_t histogram[8], uint32_t *restarts, uint32_t *dropped);
/* Development: stop and reopen the audio output as after a driver error. */
void solco_test_audio_restart(void);
/* Latency profile: 0 safe (256-frame block, 4 x 256 queue), 1 live (128, 4 x 128). */
#define SOLCO_PROFILES 2
#define SOLCO_HOTCUES 4

typedef struct
{
    int track;               /* library index (0, 1 = demos) */
    uint32_t length_frames;  /* 44.1 kHz frames; 0 while an MP3 is being prepared */
    int32_t bpm_q16;         /* 0 = unknown (analysis comes with F4) */
    uint32_t grid_offset_frames;
    int key;                 /* -1 = unknown */
} solco_track_info_t;
bool solco_track_info(int deck, solco_track_info_t *info);
/* Loop shift (M10b): move the active loop by its length (half = by half), applied
 * at the next loop edge, or at once with now = true. */
bool solco_loop_shift(int deck, int direction, bool half, bool now);
/* Beat loop from the playhead; false while the tempo is unknown. */
bool solco_loop_beats(int deck, int beats);
/* Hot cue slot 0-3: action 0 set at the playhead, 1 jump, 2 clear. */
bool solco_hotcue(int deck, int slot, int action);
/* Milliseconds since the last deck/mixer/pad action (UINT32_MAX: never). */
uint32_t solco_engine_idle_ms(void);
bool solco_audio_profile(int profile);
/* I2S frame/time line: frame(t) = (t - anchor_offset_us) * 44100 / 1e6 for the
 * studio timeline; false until the first regular write. */
bool solco_engine_timeline(int64_t *anchor_offset_us, int *block, int *queue_frames);
const char *solco_audio_profile_name(int profile);
bool solco_keylock(int deck, bool enabled);
void solco_cross(int value);
void solco_volume(int value);
void solco_status(solco_status_t *status);
bool solco_test_fixture(void);
bool solco_test_load(void);
bool solco_test_mp3_fixture(void);
bool solco_test_load_mp3(void);
void solco_test_mp3_verify(void);
bool solco_test_mp3_reset(void);
bool solco_test_load_stress(void);
#endif
