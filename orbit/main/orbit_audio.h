#ifndef ORBIT_AUDIO_H
#define ORBIT_AUDIO_H

/*
 * Orbit sound engine: ambient soundscapes synthesised in real time (no files)
 * and soft guide tones, mixed into one PCM stream in a dedicated task.
 *
 *   generators + tone voice -> gain ramp -> audio_write() -> ES8311
 *
 * The UI only posts commands (never blocks). The engine opens the speaker
 * while something plays and closes it when silent; it steps aside at once
 * when the system alarm needs the speaker.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    ORBIT_AMBIENT_OFF,
    ORBIT_AMBIENT_RAIN,
    ORBIT_AMBIENT_WAVES,
    ORBIT_AMBIENT_WIND,
    ORBIT_AMBIENT_PINK,
    ORBIT_AMBIENT_BROWN,
    ORBIT_AMBIENT_WHITE,
    ORBIT_AMBIENT_COUNT,
} orbit_ambient_t;

typedef enum
{
    ORBIT_TONE_INHALE,  /* rising glide */
    ORBIT_TONE_EXHALE,  /* falling glide */
    ORBIT_TONE_HOLD,    /* short soft tick */
    ORBIT_TONE_BELL,    /* meditation step */
    ORBIT_TONE_END,     /* session done */
} orbit_tone_t;

/* False on boards without a speaker: hide every audio option. */
bool orbit_audio_available(void);
void orbit_audio_init(void);

/* Crossfades to the new soundscape (OFF fades out). */
void orbit_audio_set_ambient(orbit_ambient_t ambient);
orbit_ambient_t orbit_audio_ambient(void);
const char *orbit_audio_name(orbit_ambient_t ambient);

/* 0..100 %, applied at once (codec volume). */
void orbit_audio_set_volume(int percent);
int orbit_audio_volume(void);

/* Ambient stops by itself after `minutes` with a 30 s fade; 0 = never. */
void orbit_audio_set_sleep_timer(int minutes);
/* Seconds left before the sleep timer stops the ambient, -1 without timer. */
int orbit_audio_sleep_left(void);

void orbit_audio_tone(orbit_tone_t tone);

/* Keep the speaker open while a guided session may play tones, so that each
 * tone starts without the codec power-up delay and pop. */
void orbit_audio_hold(bool hold);

/* True while the engine owns the speaker (system chimes would be skipped). */
bool orbit_audio_active(void);

#endif
