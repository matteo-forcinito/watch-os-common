#ifndef SERVICES_VOICE_MEMO_H
#define SERVICES_VOICE_MEMO_H

/* Local, manually started voice memos. No network or background recording.
 * Commands are non-blocking; a temporary worker owns the audio and files.
 * Stop/pause/exit/alarm requests release audio before finalising the WAV. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define VOICE_MEMO_LIMIT_SECONDS 60
#define VOICE_MEMO_LIST_SIZE 20
#define VOICE_MEMO_NAME_SIZE 48

typedef enum
{
    VOICE_MEMO_IDLE,
    VOICE_MEMO_STARTING,
    VOICE_MEMO_RECORDING,
    VOICE_MEMO_PLAYING,
    VOICE_MEMO_SAVING,
    VOICE_MEMO_SCANNING,
    VOICE_MEMO_ERROR,
} voice_memo_phase_t;

typedef struct
{
    voice_memo_phase_t phase;
    esp_err_t error;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    uint16_t peak;
    uint16_t rms;
    uint32_t clipped;
    uint8_t levels[24]; /* Measured peak history, -60..0 dBFS mapped to 0..60. */
    int selected;
    int count;
    char names[VOICE_MEMO_LIST_SIZE][VOICE_MEMO_NAME_SIZE];
} voice_memo_snapshot_t;

void voice_memo_format_name(const char *name, char *out, size_t capacity);
void voice_memo_snapshot(voice_memo_snapshot_t *out);
bool voice_memo_record(void);
bool voice_memo_play(int index);
bool voice_memo_delete(int index);
bool voice_memo_scan(void);
void voice_memo_stop(void);
int voice_memo_volume(void);
void voice_memo_set_volume(int percent);

#endif
