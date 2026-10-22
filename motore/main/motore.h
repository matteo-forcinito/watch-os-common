#ifndef MOTORE_H
#define MOTORE_H

/*
 * Motore: the car's OBD-II port through an ELM327-compatible adapter.
 *
 *   link.c   the adapter: Bluetooth LE (any brand: the serial service is
 *            found by its characteristics) or Wi-Fi (TCP, e.g. 192.168.0.10:35000).
 *            Lines out, text in until the ELM ">" prompt.
 *   obd.c    the protocol: adapter setup, supported PIDs, live polling,
 *            trouble codes, VIN, readiness, freeze frame, trip computer and
 *            the safety lock for anything that writes to the car.
 *   dtc.c    Italian descriptions of the generic trouble codes.
 *   ui.c     the watch: drive view, live data, diagnosis, trip, connection.
 *   web.c    the dashboard (same data, bigger: charts, logs, export).
 *
 * Thread model: one task owns the adapter (obd.c); everyone else reads a
 * snapshot under mt_lock()/mt_unlock() and asks for actions with mt_request().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

/* ------------------------------------------------------------- live data */

typedef enum
{
    MT_RPM, MT_SPEED, MT_COOLANT, MT_INTAKE_T, MT_LOAD, MT_THROTTLE, MT_MAP, MT_MAF,
    MT_STFT1, MT_LTFT1, MT_TIMING, MT_O2_V, MT_FUEL_LEVEL, MT_BARO, MT_AMBIENT,
    MT_RUNTIME, MT_DIST_MIL, MT_DIST_CLEAR, MT_WARMUPS, MT_MODULE_V, MT_REL_THROTTLE,
    MT_ODOMETER, MT_OIL_T, MT_FUEL_RATE, MT_BATTERY,   /* MT_BATTERY: adapter's AT RV */
    MT_VALUES
} mt_value_t;

typedef struct
{
    uint8_t pid;            /* mode 01 PID (0 for MT_BATTERY) */
    const char *key;        /* "rpm": API and CSV name */
    const char *name;       /* "Giri motore" */
    const char *unit;       /* "rpm" */
    float min, max;         /* scale for gauges and charts */
    uint8_t decimals;
    bool fast;              /* polled every cycle (else round robin) */
} mt_value_info_t;

const mt_value_info_t *mt_value_info(mt_value_t v);

/* ------------------------------------------------------------- state */

typedef enum
{
    MT_PHASE_IDLE,          /* not configured: choose an adapter */
    MT_PHASE_SEARCHING,     /* Bluetooth scan running */
    MT_PHASE_CONNECTING,    /* opening the link */
    MT_PHASE_SETUP,         /* adapter found, talking to the car */
    MT_PHASE_LIVE,          /* reading */
    MT_PHASE_NO_CAR,        /* adapter OK, the car does not answer (ignition off?) */
    MT_PHASE_LOST,          /* link dropped: retrying */
} mt_phase_t;

#define MT_DTC_MAX 32

typedef enum { MT_DTC_STORED, MT_DTC_PENDING, MT_DTC_PERMANENT } mt_dtc_kind_t;

typedef struct
{
    char code[6];           /* "P0301" */
    uint8_t kind;           /* mt_dtc_kind_t */
} mt_dtc_t;

typedef struct
{
    const char *name;       /* "Catalizzatore" */
    bool supported;
    bool complete;
} mt_monitor_t;

#define MT_MONITORS 11

typedef struct
{
    float km;               /* integrated from speed */
    float litres;           /* from fuel rate or MAF (petrol equivalent) */
    uint32_t seconds;       /* engine running */
    uint32_t idle_seconds;
    float max_speed;
    float max_rpm;
} mt_trip_t;

typedef struct
{
    mt_phase_t phase;
    char message[64];       /* what is happening, in Italian */
    char adapter[40];       /* "OBDII · ELM327 v1.5" */
    char protocol[40];      /* "ISO 15765-4 CAN (11 bit, 500 kbaud)" */
    char vin[18];
    uint8_t supported[32];  /* mode 01 PIDs 0x01..0xFF, bit set = supported */
    float value[MT_VALUES];
    int64_t updated[MT_VALUES];   /* esp_timer us of the last reading, 0 = never */
    bool mil;               /* check engine light */
    uint8_t dtc_count;      /* as reported by the car (PID 01) */
    mt_dtc_t dtc[MT_DTC_MAX];
    uint8_t dtcs;
    int64_t dtc_read;       /* when the codes were last read, 0 = never */
    mt_monitor_t monitor[MT_MONITORS];
    bool compression;       /* diesel monitors */
    float freeze[MT_VALUES];   /* freeze frame of the first stored code, NAN if none */
    char freeze_code[6];
    mt_trip_t trip;
    uint32_t revision;      /* changes with every update */
    uint32_t polls_per_s;
    bool unlocked;          /* critical actions allowed until mt_lock_actions() or 10 min */
} mt_state_t;

void mt_lock(void);
void mt_unlock(void);
/* Pointer valid between mt_lock() and mt_unlock() only. */
const mt_state_t *mt_state(void);
bool mt_value_fresh(const mt_state_t *s, mt_value_t v);   /* read in the last 5 s */

/* ------------------------------------------------------------- settings */

typedef enum { MT_LINK_BLE, MT_LINK_WIFI } mt_link_kind_t;

/* What the engine burns: sets the air/fuel ratio and density of the
 * consumption estimate (LPG: 15.5 and 0.54 kg/l). */
typedef enum { MT_FUEL_PETROL, MT_FUEL_LPG, MT_FUEL_DIESEL, MT_FUELS } mt_fuel_t;

typedef struct
{
    uint8_t link;           /* mt_link_kind_t */
    uint8_t ble_addr[7];    /* type + address of the chosen adapter; all zero = none */
    char ble_name[32];
    char host[40];          /* Wi-Fi adapter: "192.168.0.10" */
    uint16_t port;          /* 35000 */
    char pin[9];            /* code for critical actions, "" = not set yet */
    uint8_t fuel_type;      /* mt_fuel_t */
    uint16_t displacement_cc;   /* for cars without MAF (Clio 1.2: 1149) */
    float volumetric;       /* volumetric efficiency of the speed-density estimate */
    uint16_t shift_rpm;     /* shift lights flash from here */
    float gear_kmh[6];      /* km/h per 1000 rpm in each gear, 0 = no such gear */
} mt_settings_t;

/* Estimated gear from speed and rpm: 1..6, 0 = clutch/neutral/unknown. */
int mt_gear(const mt_settings_t *s, float speed, float rpm);

void mt_settings(mt_settings_t *out);
esp_err_t mt_set_settings(const mt_settings_t *in);

/* ------------------------------------------------------------- actions */

esp_err_t mt_start(void);   /* loads settings, starts the adapter task */

/* Bluetooth scan results (adapters first), for the connection page. */
#define MT_SCAN_MAX 12
typedef struct
{
    uint8_t addr[7];
    char name[32];
    int8_t rssi;
    bool likely;            /* name or service looks like an OBD adapter */
} mt_found_t;
int mt_scan_results(mt_found_t *out, int max);
void mt_scan(void);                         /* starts a 8 s scan */
void mt_choose_ble(const mt_found_t *f);    /* saves it and connects */
void mt_reconnect(void);

void mt_read_codes(void);                   /* DTCs, readiness, freeze frame */
void mt_trip_reset(void);

/* Safety lock: writes to the car (clearing codes) need the code and a
 * stationary car. */
bool mt_unlock_actions(const char *pin);
void mt_lock_actions(void);
/* Clears the trouble codes and the check engine light. Fails (and says why)
 * unless unlocked, speed 0 and engine off. */
esp_err_t mt_clear_codes(char *why, size_t size);

/* Raw ELM command for the expert console (read-only commands only). */
esp_err_t mt_console(const char *command, char *out, size_t size);

/* ------------------------------------------------------------- codes */

/* Italian description of a code ("P0301" -> "Mancata accensione cilindro 1"),
 * or a generic text by family. */
const char *mt_dtc_text(const char *code, char *buf, size_t size);
/* "Motore", "Telaio", "Carrozzeria", "Rete" */
const char *mt_dtc_system(const char *code);

/* ------------------------------------------------------------- UI and web */

void motore_ui_create(lv_obj_t *screen);
bool motore_ui_back(void);
esp_err_t motore_web_start(void);

/* link.c, used by obd.c only */
esp_err_t mt_link_open(const mt_settings_t *s, char *why, size_t size);
void mt_link_close(void);
bool mt_link_up(void);
/* Sends "command\r" and collects the answer until the ">" prompt (or timeout). */
esp_err_t mt_link_command(const char *command, char *out, size_t size, int timeout_ms);
void mt_link_scan(void);
int mt_link_scan_results(mt_found_t *out, int max);
bool mt_link_scanning(void);

#endif
