#ifndef COPERTURA_ADVICE_H
#define COPERTURA_ADVICE_H

/*
 * Copertura's judgement, pure C (no libc, no ESP-IDF: tested in an emulator).
 *
 *   grades     a measured spot gets A..F from signal, latency and loss: what
 *              it means for video calls and streaming, not just dBm.
 *   channels   how crowded each 2.4 GHz channel is: every neighbour counts by
 *              its strength and by how much its 20 MHz overlaps (channels 5
 *              apart do not overlap). Advice among the non-overlapping sets.
 *   repeater   where a repeater works best: a spot that still hears the
 *              router well (around -60 dBm) but is as far out as possible,
 *              towards the weak rooms.
 */

#include <stdbool.h>
#include <stdint.h>

#define CP_CHANNELS 13

typedef enum { CP_A, CP_B, CP_C, CP_D, CP_E, CP_F } cp_grade_t;

/* rssi dBm (average), ping ms (average, 0 = unknown), loss percent. */
cp_grade_t cp_grade(int rssi, int ping_ms, int loss);
const char *cp_grade_letter(cp_grade_t grade);
/* Short Italian verdict: "Ottimo per videochiamate e 4K"... */
const char *cp_grade_text(cp_grade_t grade);
/* 0..100 from rssi for gauges (-90 -> 0, -40 -> 100). */
int cp_signal_percent(int rssi);

typedef struct
{
    uint8_t channel;
    int8_t rssi;
} cp_neighbour_t;

typedef struct
{
    int score[CP_CHANNELS + 1];      /* congestion per channel 1..13 (0 = free), index 0 unused */
    uint8_t best;                    /* least crowded among the recommended set */
    uint8_t current;                 /* our channel (copy of the input) */
    int current_score;
    int best_score;
    bool worth_changing;             /* the best is clearly better than ours */
} cp_channels_t;

/* neighbours: every AP heard except our own radios. */
void cp_channel_advice(const cp_neighbour_t *neighbours, int count, uint8_t current, cp_channels_t *out);

typedef struct
{
    int8_t rssi;                     /* router signal measured in the spot */
    bool weak;                       /* the spot itself needs coverage */
} cp_spot_t;

/* Index of the best spot for a repeater, -1 if none fits (all strong: no need; all weak: too far). */
int cp_repeater_spot(const cp_spot_t *spots, int count);

/* Hot/cold feedback for the finder: 0 (cold) .. 100 (on target).
 * mode 0: as strong as possible (relative to the best seen); mode 1: repeater target (-62 dBm). */
int cp_finder_heat(int mode, int rssi, int best_seen);

#endif
