#ifndef APPS_INTERNAL_H
#define APPS_INTERNAL_H

#include "core/app.h"
#include "lvgl.h"

/* ---- apps (each in its own file) */
extern const app_t alarms_app;
extern const app_t alarm_edit_app;      /* hidden: arg = alarm id (0 = new) */
extern const app_t ring_app;            /* hidden: arg = alarm_t * (malloc, freed by it) */
extern const app_t weather_app;
extern const app_t settings_app;
extern const app_t settings_display_app;
extern const app_t settings_sound_app;
extern const app_t settings_wifi_app;
extern const app_t settings_bluetooth_app;
extern const app_t settings_time_app;
extern const app_t settings_about_app;
extern const app_t flashlight_app;
extern const app_t web_app;
extern const app_t extapps_run_app;     /* hidden: arg = extapp_t * (malloc, freed by it) */
extern const app_t power_app;
extern const app_t voice_memo_app;
extern const app_t timer_app;
extern const app_t music_app;
extern const app_t usb_drive_app;
extern const app_t sdcard_app;
extern const app_t battery_app;
extern const app_t launcher_all_app;    /* hidden: every app in a list (round launcher, "Altre") */
extern const app_t call_app;
extern const app_t update_app;          /* hidden: Settings > Aggiornamento */
extern const app_t versions_app;        /* hidden: every release online + on the SD card */
extern const app_t folder_app;          /* hidden: one folder of versions_app, arg = folder id */

/* ---- shell pieces (tiles of the home screen) */
void watchface_create(lv_obj_t *parent);
void watchface_aod_create(lv_obj_t *screen);
void launcher_create(lv_obj_t *parent);
void launcher_refresh(void);
bool launcher_rotate(int delta); /* select a round icon; touch the centre to open */
void control_center_create(lv_obj_t *parent);
void shell_show_control_center(void);   /* slide the home screen to the control center */
void notifications_create(lv_obj_t *parent);
void now_playing_create(lv_obj_t *parent);

/* Helpers shared by apps. */
void apps_open_cb(lv_event_t *e);   /* user_data = app id string */

#endif
