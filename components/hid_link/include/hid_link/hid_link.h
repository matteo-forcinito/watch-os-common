#ifndef HID_LINK_H
#define HID_LINK_H

/*
 * The watch as a keyboard / mouse / media remote for a PC, tablet or phone.
 *
 *   transport   Bluetooth LE (HID over GATT, pairs like any BT keyboard)
 *               or USB (the watch is a USB keyboard on the cable)
 *   reports     keyboard (id 1), media keys (id 2), mouse (id 3) - one
 *               device for every remote control mode
 *
 * Everything is queued and sent by the "hid_tx" task at a safe pace, so a
 * screen never waits and no key gets lost when typing fast.
 *
 * Text is typed according to the keyboard layout configured ON THE HOST
 * (HID sends key positions, not characters): with an Italian PC choose
 * HID_HOST_IT, so "è" or "@" come out right.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum
{
    HID_LINK_NONE,
    HID_LINK_BLE,
    HID_LINK_USB,
} hid_link_transport_t;

typedef enum
{
    HID_LINK_OFF,
    HID_LINK_WAITING,      /* advertising / waiting for the USB host */
    HID_LINK_CONNECTED,
} hid_link_state_t;

typedef enum
{
    HID_HOST_IT,
    HID_HOST_US,
} hid_host_layout_t;

/* Modifier bits (first byte of the keyboard report). */
#define HIDL_MOD_CTRL    0x01
#define HIDL_MOD_SHIFT   0x02
#define HIDL_MOD_ALT     0x04
#define HIDL_MOD_GUI     0x08   /* Windows / Cmd */
#define HIDL_MOD_ALTGR   0x40   /* right Alt */

/* A few key codes (USB HID usage table, keyboard page). */
#define HIDL_KEY_ENTER      0x28
#define HIDL_KEY_ESC        0x29
#define HIDL_KEY_BACKSPACE  0x2A
#define HIDL_KEY_TAB        0x2B
#define HIDL_KEY_SPACE      0x2C
#define HIDL_KEY_DELETE     0x4C
#define HIDL_KEY_RIGHT      0x4F
#define HIDL_KEY_LEFT       0x50
#define HIDL_KEY_DOWN       0x51
#define HIDL_KEY_UP         0x52
#define HIDL_KEY_HOME       0x4A
#define HIDL_KEY_END        0x4D
#define HIDL_KEY_PAGE_UP    0x4B
#define HIDL_KEY_PAGE_DOWN  0x4E

/* Media keys (consumer page). */
#define HIDL_MEDIA_PLAY_PAUSE  0x00CD
#define HIDL_MEDIA_NEXT        0x00B5
#define HIDL_MEDIA_PREVIOUS    0x00B6
#define HIDL_MEDIA_VOLUME_UP   0x00E9
#define HIDL_MEDIA_VOLUME_DOWN 0x00EA
#define HIDL_MEDIA_MUTE        0x00E2

/*
 * Start a transport. BLE: advertised as `name` (pair it from the host's
 * Bluetooth settings). USB: takes the USB port (no serial console after).
 */
esp_err_t hid_link_start(hid_link_transport_t transport, const char *name);

/* Stop Bluetooth (USB cannot be given back without a restart). */
void hid_link_stop(void);

hid_link_transport_t hid_link_transport(void);
hid_link_state_t hid_link_state(void);

/* Changes on every connect / disconnect (poll it from the UI). */
uint32_t hid_link_version(void);

void hid_link_set_host_layout(hid_host_layout_t layout);
hid_host_layout_t hid_link_host_layout(void);

/* Type UTF-8 text with the host layout. False if not connected or queue full. */
bool hid_link_type(const char *utf8);

/* True if the character can be typed with the current host layout. */
bool hid_link_can_type(uint32_t codepoint);

/* Press and release a key (with modifiers). */
bool hid_link_key(uint8_t modifiers, uint8_t keycode);

/* `count` backspaces. */
bool hid_link_backspace(int count);

bool hid_link_media(uint16_t usage);
bool hid_link_mouse(int8_t dx, int8_t dy, uint8_t buttons, int8_t wheel);

/* Forget every paired host (Bluetooth). */
void hid_link_forget_hosts(void);

#endif
