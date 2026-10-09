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

esp_err_t ble_companion_init(void);

/*
 * Switch Bluetooth on/off. Blocking (up to ~0.5 s): the UI does not call it,
 * it just saves settings.ble_enabled and the companion follows by itself.
 */
void ble_companion_enable(bool enable);

/* Restart with the current name/settings, in the background. */
void ble_companion_restart(void);

bool ble_companion_connected(void);

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
