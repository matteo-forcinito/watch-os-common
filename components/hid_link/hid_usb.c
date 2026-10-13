#include "hid_private.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_mac.h"
#include <stdio.h>
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"

/*
 * USB keyboard (TinyUSB HID class). The watch must be connected with the
 * cable; the ESP32-S3 USB port is then a keyboard instead of the serial
 * console (it comes back after a restart).
 *
 * The USB controller stops in light sleep, and with the USB console gone
 * nothing else keeps the chip awake: hold a "no light sleep" lock (and the
 * APB clock at 80 MHz) for as long as USB runs. The cable powers the watch.
 */

static const char *TAG = "hid_usb";

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#define HID_EP_IN 0x81

static const uint8_t s_config_descriptor[] = {
    /* config number, interface count, string index, total length, attributes, power (mA) */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    /* interface, string index, boot protocol, report map length, EP in, size, polling (ms) */
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, HID_REPORT_MAP_BYTES, HID_EP_IN, 16, 5),
};

static char s_product[32] = "Watch Remote";
static char s_serial[13];
static const char *s_strings[] = {
    (const char[]){0x09, 0x04}, /* English */
    "Watch OS",                 /* manufacturer */
    s_product,                  /* product */
    s_serial,                   /* per-device serial */
    "Remote HID",               /* HID interface */
};

static bool s_started = false;
static esp_pm_lock_handle_t s_no_sleep = NULL;
static esp_pm_lock_handle_t s_apb_max = NULL;

static void release_power_locks(void)
{
    if (s_apb_max != NULL)
    {
        esp_pm_lock_release(s_apb_max);
        esp_pm_lock_delete(s_apb_max);
        s_apb_max = NULL;
    }
    if (s_no_sleep != NULL)
    {
        esp_pm_lock_release(s_no_sleep);
        esp_pm_lock_delete(s_no_sleep);
        s_no_sleep = NULL;
    }
}

/* Default LED hook: apps that care (Vault, for Caps Lock) define their own. */
__attribute__((weak)) void hid_usb_keyboard_leds(uint8_t leds)
{
    (void)leds;
}

/* --- TinyUSB callbacks (called by the TinyUSB task) */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_map;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance;

    /* Keyboard LED output report (bit 1 = Caps Lock): forwarded to an optional hook. */
    if (report_type == HID_REPORT_TYPE_OUTPUT && buffer != NULL && bufsize >= 1 &&
        (report_id == HID_REPORT_KEYBOARD || report_id == 0))
    {
        hid_usb_keyboard_leds(buffer[0]);
    }
}

static void usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;

    if (event->id == TINYUSB_EVENT_ATTACHED)
    {
        hid_link_set_state(HID_LINK_CONNECTED);
    }
    else if (event->id == TINYUSB_EVENT_DETACHED)
    {
        hid_link_set_state(HID_LINK_WAITING);
    }
}

esp_err_t hid_usb_start(const char *name)
{
    if (s_started)
    {
        return ESP_OK;
    }

    snprintf(s_product, sizeof(s_product), "%.31s", name != NULL ? name : "Watch Remote");
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "hid_usb", &s_no_sleep);
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_pm_lock_acquire(s_no_sleep);
    if (err == ESP_OK)
    {
        err = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "hid_usb_apb", &s_apb_max);
    }
    if (err == ESP_OK)
    {
        err = esp_pm_lock_acquire(s_apb_max);
    }
    if (err != ESP_OK)
    {
        release_power_locks();
        return err;
    }

    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_event);
    config.descriptor.full_speed_config = s_config_descriptor;
    config.descriptor.string = s_strings;
    config.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);

    err = tinyusb_driver_install(&config);

    if (err == ESP_OK)
    {
        s_started = true;
        ESP_LOGI(TAG, "USB keyboard ready (connect the cable)");
    }
    else
    {
        release_power_locks();
    }

    return err;
}

esp_err_t hid_usb_send(uint8_t report_id, const uint8_t *data, uint16_t length)
{
    if (!s_started || !tud_mounted())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (tud_suspended())
    {
        tud_remote_wakeup(); /* wake the sleeping PC with the key press */
        return ESP_ERR_TIMEOUT;
    }

    if (!tud_hid_ready())
    {
        return ESP_ERR_TIMEOUT; /* endpoint busy: hid_tx tries again */
    }

    return tud_hid_report(report_id, data, length) ? ESP_OK : ESP_FAIL;
}
