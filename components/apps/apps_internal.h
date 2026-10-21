#ifndef APPS_INTERNAL_H
#define APPS_INTERNAL_H

#include "core/app.h"
#include "lvgl.h"

/* ---- apps (each in its own file) */
extern const app_t alarms_app;
extern const app_t alarm_edit_app;      /* hidden: arg = alarm id (0 = new) */
extern const app_t ring_app;            /* hidden: arg = alarm_t * (malloc, freed by it) */
extern const app_t weather_app;
extern const app_t market_app;
extern const app_t settings_app;
extern const app_t settings_display_app;
extern const app_t settings_sound_app;
extern const app_t settings_wifi_app;
extern const app_t settings_bluetooth_app;
extern const app_t settings_time_app;
extern const app_t settings_about_app;
extern const app_t flashlight_app;
extern const app_t web_app;
extern const app_t ecos_app;
extern const app_t ecos_device_app;   /* hidden: arg = malloc'd id (freed by it) */
extern const app_t ecos_pair_app;
extern const app_t ecos_find_app;     /* hidden: arg = malloc'd name of who rings */
void ecos_apps_init(void);
extern const app_t extapps_open_app;
extern const app_t extapps_run_app;     /* hidden: arg = extapp_t * (malloc, freed by it) */
extern const app_t power_app;
extern const app_t voice_memo_app;
extern const app_t sensors_app;
extern const app_t sistema_app;
/* Overlay of Sistema (on after a double reset or from the app); LVGL locked. */
void sistema_boot(void);
extern const app_t timer_app;
/* Orbit Link remote control of the timer (LVGL locked). */
typedef struct
{
    bool countdown_running, countdown_ringing, watch_running;
    int32_t countdown_total_s, countdown_remaining_s;
    int64_t watch_elapsed_ms;
} timer_remote_t;
void timer_remote_state(timer_remote_t *out);
bool timer_remote(const char *action, int seconds);
/* Orbit Link: scenes, events, audio (apps/link). */
void link_init(void);
extern const app_t music_app;
extern const app_t usb_drive_app;
extern const app_t sdcard_app;
extern const app_t battery_app;
extern const app_t launcher_all_app;    /* hidden: every app in a list (round launcher, "Altre") */
extern const app_t launcher_apps_app;   /* hidden: paged cards, arg = (void *)0 built-in, (void *)1 external */
extern const app_t launcher_actions_app;/* hidden: Apri / Dettagli / Preferita..., arg = launcher_item_t * (malloc) */
extern const app_t launcher_details_app;/* hidden: details of one app, arg = launcher_item_t * (malloc) */
extern const app_t call_app;
extern const app_t update_app;          /* hidden: Settings > Aggiornamento */
extern const app_t versions_app;        /* hidden: every release online + on the SD card */
extern const app_t folder_app;          /* hidden: one folder of versions_app, arg = folder id */
extern const app_t news_app;            /* CONFIG_APPKIT_HOSTED: newspaper from RSS feeds (watch-os-common/news) */
void news_service_start(void);
extern const app_t giornata_app;        /* CONFIG_APPKIT_HOSTED: agenda (watch-os-common/giornata) */
void giornata_service_start(void);
extern const app_t pieno_app;
extern const app_t casa_app;
extern const app_t ombra_app;           /* CONFIG_APPKIT_HOSTED: Bluetooth trackers (watch-os-common/ombra) */
extern const app_t copertura_app;
extern const app_t bilancio_app;         /* CONFIG_APPKIT_HOSTED: money (watch-os-common/bilancio) */
extern const app_t sentinel_app;        /* CONFIG_APPKIT_HOSTED: Sentinel Lite, home network (watch-os-common/sentinel/lite) */       /* CONFIG_APPKIT_HOSTED: Wi-Fi coverage (watch-os-common/copertura) */           /* CONFIG_APPKIT_HOSTED: fuel prices (watch-os-common/pieno) */

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
