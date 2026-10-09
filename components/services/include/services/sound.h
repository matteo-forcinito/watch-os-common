#ifndef SERVICES_SOUND_H
#define SERVICES_SOUND_H

/*
 * Simple generated sounds (no files needed): key clicks, notification
 * chimes and the alarm melody. They run in their own task, so callers never
 * block, and they never interrupt the radio (the radio owns the speaker
 * while it plays; the alarm stops the radio first if it needs the speaker).
 */

#include <stdbool.h>

void sound_service_init(void);

/* Short tick for key presses (only if enabled in the settings). */
void sound_click(void);

/* Two-tone chime for a new notification. */
void sound_notification(void);

/* Looping alarm melody until sound_alarm_stop(). Gets louder over ~20 s. */
void sound_alarm_start(void);
void sound_alarm_stop(void);
bool sound_alarm_playing(void);

#endif
