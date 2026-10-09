#include "hid_private.h"

#include "esp_log.h"
#include "esp_pm.h"
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
#define HID_EP_IN        0x81

static const uint8_t s_config_descriptor[] = {
    /* config number, interface count, string index, total length, attributes, power (mA) */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    /* interface, string index, boot protocol, report map length, EP in, size, polling (ms) */
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, HID_REPORT_MAP_BYTES, HID_EP_IN, 16, 5),
};

static const char *s_strings[] = {
    (const char[]){0x09, 0x04},   /* English */
    "AMOLED Watch",               /* manufacturer */
    "AMOLED Remote",              /* product */
    "0001",                       /* serial */
    "AMOLED Remote HID",          /* HID interface */
};

static bool s_started = false;
static esp_pm_lock_handle_t s_no_sleep = NULL;
static esp_pm_lock_handle_t s_apb_max = NULL;

/* --- TinyUSB callbacks (called by the TinyUSB task) */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_map;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;   /* keyboard LEDs: not used */
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
    (void)name;

    if (s_started)
    {
        return ESP_OK;
    }

    if (s_no_sleep == NULL && esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "hid_usb", &s_no_sleep) == ESP_OK)
    {
        esp_pm_lock_acquire(s_no_sleep);
    }

    if (s_apb_max == NULL && esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "hid_usb_apb", &s_apb_max) == ESP_OK)
    {
        esp_pm_lock_acquire(s_apb_max);
    }

    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_event);
    config.descriptor.full_speed_config = s_config_descriptor;
    config.descriptor.string = s_strings;
    config.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);

    esp_err_t err = tinyusb_driver_install(&config);

    if (err == ESP_OK)
    {
        s_started = true;
        ESP_LOGI(TAG, "USB keyboard ready (connect the cable)");
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
        tud_remote_wakeup();   /* wake the sleeping PC with the key press */
        return ESP_ERR_TIMEOUT;
    }

    if (!tud_hid_ready())
    {
        return ESP_ERR_TIMEOUT;   /* endpoint busy: hid_tx tries again */
    }

    return tud_hid_report(report_id, data, length) ? ESP_OK : ESP_FAIL;
}
