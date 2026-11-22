#ifndef SOLCO_STUDIO_PRO_H
#define SOLCO_STUDIO_PRO_H

/*
 * Studio Pro (boards with a microphone and plenty of PSRAM, SOLCO_STUDIO_PRO):
 *
 *   sampler    8 slots recorded from the microphone (or loaded from the card),
 *              trimmed and processed, played by pads and by the sequencer;
 *   synth      6-voice subtractive synth (2 oscillators, resonant filter with
 *              its own envelope, ADSR, drive, tempo echo), presets;
 *   sequencer  16 steps synced to the Studio transport: sample tracks and a
 *              melody track for the synth (the drum track is the Studio beat).
 *
 * Audio runs in the engine's audio task (studio_pro_render adds to the Studio
 * bus). The UI calls the other functions from the LVGL task: they only touch
 * shared state under a short spinlock or queue an event. Recording needs the
 * codec, which plays the output too: the engine pauses its output for the take
 * (studio_pro_capture_*), then fades back in.
 */
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRO_SLOTS        8
#define PRO_STEPS        16
#define PRO_REC_RATE     16000
#define PRO_REC_SECONDS  10
#define PRO_NOTE_ROWS    8      /* melody grid rows: scale degrees */

/* ---- sampler */

typedef struct
{
    bool used;
    uint32_t frames;            /* after trim */
    uint32_t rate;              /* source rate (16000 recorded, 44100 loaded) */
    int pitch;                  /* semitones -12..+12 (playback) */
    int gain;                   /* percent 0..200 */
    char name[24];
} pro_slot_info_t;

void studio_pro_slot_info(int slot, pro_slot_info_t *out);
/* Peaks of the slot for a waveform of `count` columns (0..32767). Returns false if empty. */
bool studio_pro_slot_peaks(int slot, uint16_t *peaks, int count);

/* Play a slot now (pads, preview). velocity 1..127. */
void studio_pro_hit(int slot, int velocity);

typedef enum
{
    PRO_EDIT_TRIM,        /* keep [a, b) per mille of the length */
    PRO_EDIT_NORMALIZE,
    PRO_EDIT_REVERSE,
    PRO_EDIT_FADE,        /* short fade in, longer fade out */
    PRO_EDIT_LOWPASS,     /* warmer, one pass */
    PRO_EDIT_HIGHPASS,    /* thinner, one pass */
    PRO_EDIT_CHOP,        /* cut the slot in 8 equal parts into slots 0..7 */
    PRO_EDIT_CLEAR,
} pro_edit_t;

esp_err_t studio_pro_edit(int slot, pro_edit_t edit, int a, int b);
void studio_pro_set_pitch(int slot, int semitones);
void studio_pro_set_gain(int slot, int percent);

/* /sdcard/solco/samples/slotN.wav (16-bit mono). */
esp_err_t studio_pro_save(int slot);
esp_err_t studio_pro_load(int slot);

/* ---- capture (microphone) */

typedef enum
{
    PRO_CAPTURE_IDLE,
    PRO_CAPTURE_WAITING,   /* asked, the engine is releasing the speaker */
    PRO_CAPTURE_RECORDING,
    PRO_CAPTURE_DONE,
    PRO_CAPTURE_FAILED,
} pro_capture_state_t;

esp_err_t studio_pro_record(int slot);   /* starts; studio_pro_record_stop() or 10 s end it */
void studio_pro_record_stop(void);
pro_capture_state_t studio_pro_capture_state(int *elapsed_ms, int *level);

/* Engine side (audio task only). */
bool studio_pro_capture_pending(void);
void studio_pro_capture_run(void);

/* ---- synth */

typedef enum
{
    PRO_WAVE_SAW,
    PRO_WAVE_SQUARE,
    PRO_WAVE_TRIANGLE,
    PRO_WAVE_SINE,
    PRO_WAVE_NOISE,
    PRO_WAVE_COUNT,
} pro_wave_t;

typedef struct
{
    uint8_t wave;        /* pro_wave_t */
    uint8_t cutoff;      /* 0..100 */
    uint8_t resonance;   /* 0..100 */
    uint8_t env_amount;  /* filter envelope 0..100 */
    uint8_t attack;      /* 0..100 (0 = 1 ms .. 100 = 2 s) */
    uint8_t decay;
    uint8_t sustain;     /* level 0..100 */
    uint8_t release;
    uint8_t detune;      /* second oscillator, 0..100 cents/2 */
    uint8_t drive;       /* 0..100 */
    uint8_t echo;        /* tempo echo mix 0..100 */
    uint8_t volume;      /* 0..100 */
} pro_synth_t;

#define PRO_PRESETS 8
const char *studio_pro_preset_name(int preset);
void studio_pro_preset(int preset);
void studio_pro_synth_get(pro_synth_t *out);
void studio_pro_synth_set(const pro_synth_t *params);

void studio_pro_note_on(int note, int velocity);   /* MIDI note */
void studio_pro_note_off(int note);

/* ---- sequencer (steps follow the Studio transport and tempo) */

/* Sample tracks: bit i of mask = step i. */
uint16_t studio_pro_track(int slot);
void studio_pro_toggle(int slot, int step);
/* Melody: row 0..7 (scale degree) or -1 per step. */
int studio_pro_melody(int step);
void studio_pro_set_melody(int step, int row);
int studio_pro_row_note(int row);           /* MIDI note of a melody row */
void studio_pro_set_root(int midi_note);    /* lowest row (default C3 = 48) */
int studio_pro_root(void);
void studio_pro_set_swing(int percent);     /* 0..60 */
int studio_pro_swing(void);
void studio_pro_clear_tracks(void);
int studio_pro_step(void);                  /* last step played, -1 stopped */

/* Pattern, synth and slot settings on the card (/sdcard/solco/studio/pro.bin). */
void studio_pro_save_project(void);

/* ---- engine hooks */

bool studio_pro_init(void);                 /* allocations + load the saved project */
bool studio_pro_active(void);               /* something is sounding */
/* Add into the stereo Studio bus. playing/step/bpm come from the Studio transport. */
void studio_pro_render(int16_t *stereo, size_t frames, bool playing, int step, int bpm);

#endif
