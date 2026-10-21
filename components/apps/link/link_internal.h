#ifndef LINK_INTERNAL_H
#define LINK_INTERNAL_H

/* Orbit Link internals shared by the HTTP side (link.c) and the scenes
 * drawn on the screen (link_scene.c). */

#include <stdbool.h>

#include "cJSON.h"

/* Input for the companion (taps on scenes, dial, Orbi). Any task. */
void link_event(const char *type, int value);
void link_event_text(const char *type, const char *value);

/* Draws the scene described by `json` (takes ownership) in the lvgl task.
 * False when the scene is unknown or the UI queue is full. Any task. */
bool link_scene_post(cJSON *json);

/* Shows or hides the "microphone on" indicator. Any task. */
void link_scene_mic(bool on);

#endif
