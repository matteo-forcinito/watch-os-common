#ifndef SOLCO_STUDIO_H
#define SOLCO_STUDIO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define STUDIO_RATE 44100
#define STUDIO_BANKS 34
#define STUDIO_VOICES 16
#define STUDIO_EVENTS 64
#define STUDIO_HIT 1
#define STUDIO_PLAY 2
#define STUDIO_RECORD 3
#define STUDIO_CLEAR 4
#define STUDIO_TEMPO 5
#define STUDIO_ENABLE 6
#define STUDIO_KIT 7

typedef struct
{
    uint8_t type, value, velocity;
    uint64_t frame;
} studio_event_t;
typedef struct
{
    int16_t *pcm;
    uint32_t frames;
} studio_sample_t;
typedef struct
{
    int bank;
    uint32_t pos, age;
    int velocity, tail, tail_left, last;
    bool on;
} studio_voice_t;
typedef struct
{
    uint16_t pattern[8];
    uint16_t bpm;
    uint16_t version;
} studio_pattern_t;
typedef struct
{
    bool playing, recording, enabled;
    int bpm, step, voices, kit;
    uint32_t hits, dropped, late, revision;
    uint64_t frame;
    studio_pattern_t pattern;
} studio_status_t;
typedef struct
{
    studio_sample_t bank[STUDIO_BANKS];
    studio_voice_t voice[STUDIO_VOICES];
    studio_event_t event[STUDIO_EVENTS];
    size_t events;
    studio_status_t status;
    uint16_t playback[8];
    uint64_t phase;
    int last_step;
    float limiter;
    uint32_t random;
} studio_t;
bool studio_init(studio_t *studio);
void studio_free(studio_t *studio);
bool studio_push(studio_t *studio, studio_event_t event);
void studio_render(studio_t *studio, int16_t *stereo, size_t frames);
void studio_restore(studio_t *studio, const studio_pattern_t *pattern);
#endif
