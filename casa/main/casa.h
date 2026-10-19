/*
 * Casa - Home Assistant on the wrist and on the desk.
 *
 *   ha.c     WebSocket client: auth, subscribe_entities (live states, no
 *            polling), call_service, ping; reconnects with backoff
 *   model.c  favourite entities and their live state (name, area, domain,
 *            state, brightness, colour, position, temperatures, media...)
 *   web.c    setup page on the PC: the browser lists areas and entities from
 *            Home Assistant itself, the watch stores URL, token and favourites
 *   ui.c     rooms of tiles (lights glow in their real colour), detail pages
 *            with a dial (brightness, position, volume, setpoint): knob or touch
 */
#ifndef CASA_H
#define CASA_H

#include "esp_err.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CASA_ENTITIES 256
#define CASA_ID 256 /* Home Assistant entity IDs: up to 255 characters plus NUL. */
#define CASA_NAME 40

typedef enum
{
    CASA_LIGHT,
    CASA_SWITCH,   /* switch, input_boolean, automation */
    CASA_FAN,
    CASA_COVER,
    CASA_CLIMATE,
    CASA_MEDIA,
    CASA_SCENE,    /* scene, script, button, input_button */
    CASA_LOCK,
    CASA_SENSOR,   /* sensor, binary_sensor, weather... read only */
    CASA_PERSON,   /* person, device_tracker */
    CASA_OTHER,
} casa_kind_t;

typedef struct
{
    char id[CASA_ID];       /* light.kitchen */
    char name[CASA_NAME];   /* from setup, else friendly_name */
    char area[CASA_NAME];
    char device_id[64];
    char device_name[CASA_NAME];
    char device_class[24];
    char dashboards[4][CASA_NAME];
    uint8_t kind;           /* casa_kind_t */
    bool known;             /* a state arrived */
    bool available;
    bool on;                /* on / open / playing / home / unlocked... per kind */
    char state[24];         /* raw state */
    char unit[12];
    int brightness;         /* 0..100, -1 n/a */
    uint32_t rgb;           /* light colour (0 = n/a) */
    int color_temp_kelvin;  /* 0 = n/a */
    bool supports_color, supports_temp, supports_brightness;
    int position;           /* cover 0..100, fan percentage, media volume 0..100; -1 n/a */
    float current_temp, target_temp, min_temp, max_temp, temp_step; /* climate (NaN = n/a) */
    char hvac_modes[6][12];
    int hvac_count;
    char media_title[48], media_artist[40];
    uint32_t changed_ms;    /* local time of the last change */
    int64_t last_changed;   /* unix time from Home Assistant ("lc"), 0 = unknown */
} casa_entity_t;

typedef struct
{
    bool configured;
    bool online;            /* Wi-Fi */
    bool connected;         /* authenticated with Home Assistant */
    char message[80];
    char ha_version[16];
    int count;
    uint32_t revision;      /* changes on every state change */
    uint32_t event_count;
    uint32_t last_event_ms;
} casa_status_t;

esp_err_t casa_start(void);
void casa_stop(void);
void casa_ui_create(lv_obj_t *screen);
void casa_ui_destroy(void);
bool casa_ui_back(void);
void casa_status(casa_status_t *out);
int casa_entities(casa_entity_t *out, int max);
bool casa_entity(const char *id, casa_entity_t *out);

/* Actions (queued, never blocking the UI). data_json: service data object or NULL. */
bool casa_call(const char *domain, const char *service, const char *entity_id, const char *data_json);
/* The natural tap: toggle a light, run a scene, open/close a cover... */
bool casa_tap(const casa_entity_t *e);

/* Configuration (web.c stores it). */
esp_err_t casa_config_store(const char *json, char *error, size_t error_size);
esp_err_t casa_config_clear(void);
void casa_reconnect(void);
const char *casa_web_address(void);
const char *casa_web_code(void);
esp_err_t casa_web_start(void);

/* Helpers shared by the UI. */
casa_kind_t casa_kind_of(const char *entity_id);
const char *casa_state_text(const casa_entity_t *e, char *out, size_t size);
const char *casa_type_name(const casa_entity_t *e);

#endif
