#ifndef SERVICES_BLE_COMPANION_H
#define SERVICES_BLE_COMPANION_H

/*
 * Phone companion over Bluetooth LE.
 *
 * Transport: Nordic UART Service (NUS), lines of text in both directions.
 * Protocol: the one Gadgetbridge (Android, free, open source) speaks to
 * Bangle.js watches, so the existing app works with no phone code to write:
 *
 *   phone -> watch                          watch -> phone
 *   setTime(1760000000);                    {"t":"status","bat":80,"chg":0}
 *   GB({"t":"notify","id":1,"src":"WhatsApp",   {"t":"music","n":"next"}
 *       "title":"Anna","body":"Ciao!"})     {"t":"findPhone","n":true}
 *   GB({"t":"notify-","id":1})              {"t":"ver","fw":"1.0","hw":"..."}
 *   GB({"t":"call","cmd":"incoming",...})
 *   GB({"t":"musicinfo","artist":..,"track":..})
 *   GB({"t":"musicstate","state":"play"})
 *   GB({"t":"weather","temp":291,"code":800,...})
 *   GB({"t":"find","n":true})
 *
 * Gadgetbridge recognizes Bangle.js watches by name: keep the device name
 * starting with "Bangle.js" (Settings > Bluetooth) to pair it there. Any
 * other app can use the same NUS service and messages.
 *
 * Power: slow advertising (~1 s) while waiting; once connected, a relaxed
 * connection interval with slave latency, so the radio wakes rarely.
 */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct
{
    bool valid;
    bool playing;
    char artist[48];
    char track[64];
} ble_music_t;

/* Sets the controller's memory aside while the heap is still whole (right
 * after the UI is built); ble_companion_init() hands it to the controller. */
void ble_companion_reserve(void);
esp_err_t ble_companion_init(void);

/*
 * Switch Bluetooth on/off. Blocking (up to ~0.5 s): the UI does not call it,
 * it just saves settings.ble_enabled and the companion follows by itself.
 */
void ble_companion_enable(bool enable);

/* Restart with the current name/settings, in the background. */
void ble_companion_restart(void);

bool ble_companion_connected(void);

/* BLE scan with the companion's host, for internal apps (Ombra): `cb` gets the
 * BLE_GAP_EVENT_DISC events in the NimBLE host task. Needs Bluetooth on (false
 * if off); resumes by itself when Bluetooth comes back. */
struct ble_gap_event;
bool ble_companion_scan_start(int (*cb)(struct ble_gap_event *event, void *arg));
void ble_companion_scan_stop(void);
bool ble_companion_scanning(void);

/* Bluetooth on for an app's session even if it is off in the settings
 * (Ombra): hold(true) at open, hold(false) at close switches it off again
 * unless the user wants it on. Non-blocking (runs on the sys worker). */
void ble_companion_hold(bool on);

/* Music shown by the phone, and remote control buttons. */
bool ble_companion_music(ble_music_t *out);
void ble_companion_music_command(const char *command);   /* "play" "pause" "next" "previous" "volumeup" "volumedown" */

/* Ring the phone. */
void ble_companion_find_phone(bool on);

/* Send the battery state now (also sent automatically when it changes). */
void ble_companion_send_status(void);

/* Answer an incoming call: "accept" or "reject"... */
void ble_companion_call_command(const char *command);

#endif
