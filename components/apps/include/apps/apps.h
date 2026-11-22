#ifndef APPS_APPS_H
#define APPS_APPS_H

#include "lvgl.h"

/*
 * The built-in apps and the watch shell (home screen).
 *
 *                     ┌──────────────┐
 *                     │ control      │   swipe down from the watch face
 *                     │ center       │
 *     ┌────────────┐  ├──────────────┤  ┌────────────┐
 *     │ now playing│◄─│  WATCH FACE  │─►│  app list  │
 *     └────────────┘  ├──────────────┤  └────────────┘
 *                     │ notifications│   swipe up
 *                     └──────────────┘
 *
 * To add an app: write my_app.c with a `const app_t my_app`, declare it in
 * apps_internal.h and add it to the list in apps.c. That is all.
 */

/* Register every built-in app and build the home screen. LVGL lock held. */
void apps_init(void);

/*
 * A board can replace the classic watch face in the middle of the home tiles
 * with its own (e.g. a phone-like home on a large panel). The control center,
 * notifications, launcher and now-playing tiles stay. Call before apps_init();
 * the always-on face is unchanged.
 */
void apps_set_home_face(void (*create)(lv_obj_t *parent));

/*
 * Show one page of the home screen (lvgl task): col/row in the 3 x 3 grid,
 * 1/1 = watch face, 1/0 = control center, 2/1 = apps, 0/1 = now playing,
 * 1/2 = notifications. Closes the open apps first.
 */
void apps_show_home_tile(int col, int row);
/* Open notifications from controls, including when Casa occupies the lower tile. */
void apps_show_notifications(void);

#endif
