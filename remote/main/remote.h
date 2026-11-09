#ifndef REMOTE_H
#define REMOTE_H

/*
 * Remote Control: shared pieces of the app.
 *
 *   remote_home.c     home screen: connection, control modes, options
 *   keyboard_app.c    the swipe keyboard
 *   swipe.c           swipe decoder (pure C, host-tested)
 *   remote_deck.c     PC page: contextual touchbar and state from the PC agent
 *   remote_air.c      air mouse (gyroscope), boards with an IMU
 *   remote_hub_ui.c   Remote Hub role and pairing (radio in hid_link/hid_hub.c)
 *   main.c            start-up
 */

#include "core/app.h"
#include "hid_link/hid_link.h"
#include "swipe.h"
#include "ui/keyboard_layout.h"

#include <stdbool.h>

typedef enum
{
    DICT_IT,
    DICT_EN,
} remote_dict_t;

typedef struct
{
    uint8_t transport;     /* hid_link_transport_t */
    uint8_t host_layout;   /* hid_host_layout_t */
    uint8_t dictionary;    /* remote_dict_t */
    bool swipe;            /* swipe typing on */
    bool auto_caps;        /* capital letter at the start of a sentence */
    uint8_t host_os;       /* 0 Windows/Linux, 1 macOS */
    uint8_t meeting;       /* 0 Teams, 1 Zoom */
    uint8_t pointer_speed; /* 1..3 */
    uint8_t air_flags;     /* air mouse: 1 invert X, 2 invert Y, 4 always on */
    uint8_t scroll_flags;  /* 1 invert vertical, 2 invert horizontal scroll (edges, knob) */
    uint8_t air_grip;      /* air mouse hold: 0 auto (screen orientation), 1 horizontal, 2 vertical,
                              3 horizontal upside down, 4 vertical upside down */
} remote_settings_t;

/* Settings kept in NVS ("remote" namespace). */
const remote_settings_t *remote_settings(void);
void remote_settings_save(const remote_settings_t *settings);
void remote_settings_load(void);
/* Scroll steps as the user wants them (Opzioni > Mouse: inverti). */
int8_t remote_wheel(int8_t steps);
int8_t remote_pan(int8_t steps);

/* Dictionary for the current language (loaded once, NULL if out of memory). */
const swipe_dict_t *remote_dictionary(void);
void remote_dictionary_select(remote_dict_t dict);

/* Start the saved transport (at boot and after a change). */
void remote_connect(void);
/* Battery pill over every screen, level also sent to the computers. LVGL lock held. */
void remote_battery_start(void);
/* Ask, then return to the launcher. */
void remote_confirm_exit(void);
#define REMOTE_TOP 22 /* first row of the header on square panels, below the battery pill */

/* "Bluetooth: in attesa" ... for headers. */
const char *remote_status_text(void);

extern const app_t keyboard_app;
extern const app_t remote_pc_keyboard_app;
extern const app_t remote_options_app;
extern const app_t remote_hosts_app; /* paired computers: choose, add, forget */
const char *remote_host_text(void);
extern const app_t remote_mouse_app;
extern const app_t remote_media_app;
extern const app_t remote_presenter_app;
extern const app_t remote_meeting_app;
extern const app_t remote_gamepad_app;
extern const app_t remote_shortcuts_app;
extern const app_t remote_air_app; /* only on boards with an IMU */
void remote_modes_register(void);
void remote_air_register(void);

/* Deck (remote_deck.c): PC page fed by the PC agent over hid_link's data channel. */
extern const app_t remote_deck_app;
void remote_deck_init(void);
/* Ask to restart Remote on the other transport (USB <-> Bluetooth). */
void remote_change_transport(lv_event_t *e);

/* Remote Hub (remote_hub_ui.c): several watches as one HID set. */
extern const app_t remote_hub_app;
void remote_hub_register(void);
const char *remote_hub_role_text(void);
const char *remote_device_name(void);
bool remote_send_ok(bool accepted);
void remote_alarm_prepare(void);
void remote_alarm_start(void);

/* The screen as LVGL draws it, rotation included: layouts must never use the
 * native panel size (an AMOLED 1.8 turned to landscape is 448x368). */
static inline int32_t remote_screen_w(void)
{
    return lv_display_get_horizontal_resolution(lv_display_get_default());
}
static inline int32_t remote_screen_h(void)
{
    return lv_display_get_vertical_resolution(lv_display_get_default());
}
static inline bool remote_landscape(void)
{
    return remote_screen_w() > remote_screen_h() + 40;
}

/* Edge gestures of the touch surfaces (remote_gestures.c): scroll, volume, brightness. */
typedef enum
{
    REMOTE_EDGE_NONE = -1,
    REMOTE_EDGE_RIGHT,
    REMOTE_EDGE_BOTTOM,
    REMOTE_EDGE_LEFT,
    REMOTE_EDGE_TOP,
} remote_edge_t;
remote_edge_t remote_edge_at(lv_point_t p);
bool remote_edge_vertical(remote_edge_t edge);
/* The first move (tx, ty from the touch-down) goes along the edge the touch started on. */
bool remote_edge_claims(remote_edge_t edge, int32_t tx, int32_t ty);
const char *remote_edge_name(remote_edge_t edge);
/* Travel along the edge in px; *carry keeps what has not become a step yet. */
void remote_edge_move(remote_edge_t edge, int32_t delta, float *carry);
lv_obj_t *remote_edge_mark(lv_obj_t *parent, remote_edge_t edge);
void remote_edge_light(lv_obj_t *const marks[4], remote_edge_t lit);

/* Mode switcher: one button, same place and look on every mode screen. */
extern const app_t remote_switcher_app;
void remote_switcher_open(lv_event_t *e);
lv_obj_t *remote_switcher_button(lv_obj_t *screen);
/* The header band of every mode screen: switcher on the left, title centred. */
ui_key_rect_t remote_header_band(void);
/* ui_page() plus the switcher, for the list screens of a mode. */
lv_obj_t *remote_page(lv_obj_t *screen, const char *title);
void remote_home_create(lv_obj_t *screen);

/* USB drive to install Watch Deck from the microSD (remote_drive.c). */
bool remote_drive_supported(void);  /* native USB port on this board */
int remote_drive_installers(void);  /* bit 0 Windows, 1 macOS, 2 Linux: installers in /sdcard/deck */
void remote_drive_request(void);    /* restart as a USB drive; does not return */
bool remote_drive_boot(void);       /* at boot: true if this start is the drive */

#endif
