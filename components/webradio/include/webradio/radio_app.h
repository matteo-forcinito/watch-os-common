#ifndef WEBRADIO_RADIO_APP_H
#define WEBRADIO_RADIO_APP_H

/*
 * The web radio app (stations, favourites, genres, now playing).
 *
 * One app, two ways to ship it:
 *   - internal: the firmware registers it before apps_init() and starts the
 *     engine with radio_service_init() (boards with RAM and flash to spare);
 *   - external: the WebRadio app (separate image on the SD card) uses
 *     radio_app.create() as its home screen.
 */

#include "core/app.h"

extern const app_t radio_app;

#endif
