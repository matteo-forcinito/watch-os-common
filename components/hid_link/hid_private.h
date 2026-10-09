#ifndef HID_PRIVATE_H
#define HID_PRIVATE_H

/* Shared between hid_link.c and the two transports. */

#include "hid_link/hid_link.h"

#include <stdint.h>

#define HID_REPORT_KEYBOARD  1
#define HID_REPORT_MEDIA     2
#define HID_REPORT_MOUSE     3

/*
 * One report map for both transports:
 *   id 1 keyboard: [modifiers, reserved, key1..key6] in, LEDs out
 *   id 2 media:    16-bit consumer usage
 *   id 3 mouse:    [buttons, x, y, wheel]
 */
#define HID_REPORT_MAP_BYTES 144

extern const uint8_t hid_report_map[HID_REPORT_MAP_BYTES];
extern const uint16_t hid_report_map_len;

/* Transport -> hid_link: connection changed. */
void hid_link_set_state(hid_link_state_t state);

/* Send one report now (from the hid_tx task). */
esp_err_t hid_ble_start(const char *name);
void hid_ble_stop(void);
esp_err_t hid_ble_send(uint8_t report_id, const uint8_t *data, uint16_t length);
void hid_ble_forget(void);

esp_err_t hid_usb_start(const char *name);
esp_err_t hid_usb_send(uint8_t report_id, const uint8_t *data, uint16_t length);

/* Character -> (modifiers, key code) for the host layout. False if not typeable. */
bool hid_keymap_lookup(hid_host_layout_t layout, uint32_t codepoint, uint8_t *modifiers, uint8_t *keycode);

#endif
