#include "hid_private.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_mac.h"
#include <stdio.h>
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <string.h>

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

/* The USB report map is Remote's map plus a vendor collection (report 5):
 * the Deck data channel on the cable, opened by the PC agent with hidapi,
 * no driver needed. 63 bytes each way: [length | 0x80 on the last chunk] + 62. */
static const uint8_t s_deck_collection[] = {
    0x06, 0x00, 0xFF,       /* Usage Page (Vendor 0xFF00) */
    0x09, 0x01,             /* Usage (1) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, HID_REPORT_DECK,  /*   Report ID */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,       /*   Logical Maximum (255) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, HID_DECK_CHUNK,   /*   Report Count */
    0x09, 0x02, 0x81, 0x02, /*   Usage (2), Input (Data, Var, Abs) */
    0x09, 0x03, 0x91, 0x02, /*   Usage (3), Output (Data, Var, Abs) */
    0xC0,                   /* End Collection */
};
#define USB_MAP_BYTES (HID_REPORT_MAP_BYTES + 27)
_Static_assert(sizeof(s_deck_collection) == 27, "deck collection size");
static uint8_t s_usb_map[USB_MAP_BYTES];

/* Deck messages to the PC: chunked by a small task (below), never from TinyUSB's own task. */
typedef struct
{
    char text[HID_DECK_MAX + 1];
} deck_message_t;

static QueueHandle_t s_deck_queue;
static void deck_tx_task(void *arg);

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#define HID_EP_IN 0x81

static const uint8_t s_config_descriptor[] = {
    /* config number, interface count, string index, total length, attributes, power (mA) */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    /* interface, string index, boot protocol, report map length, EP in, size, polling (ms) */
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, USB_MAP_BYTES, HID_EP_IN, 64, 5),
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
    return s_usb_map;
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

    if (report_id == HID_REPORT_DECK && report_type == HID_REPORT_TYPE_OUTPUT && buffer != NULL)
    {
        if (bufsize == HID_DECK_CHUNK + 1 && buffer[0] == HID_REPORT_DECK)
        {
            buffer++; /* some paths keep the report id in the buffer */
            bufsize--;
        }
        hid_deck_usb_frame(buffer, bufsize);
        return;
    }

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
    memcpy(s_usb_map, hid_report_map, HID_REPORT_MAP_BYTES);
    memcpy(s_usb_map + HID_REPORT_MAP_BYTES, s_deck_collection, sizeof(s_deck_collection));
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

    if (err == ESP_OK && s_deck_queue == NULL)
    {
        /* Created once here, before any sender can race to create it. */
        s_deck_queue = xQueueCreate(4, sizeof(deck_message_t));
        if (s_deck_queue == NULL || xTaskCreate(deck_tx_task, "deck_usb", 3072, NULL, 4, NULL) != pdPASS)
            ESP_LOGW(TAG, "Deck channel unavailable on USB");
    }
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

/* ---- Deck messages to the PC */

static void deck_tx_task(void *arg)
{
    (void)arg;
    deck_message_t message;
    for (;;)
    {
        if (xQueueReceive(s_deck_queue, &message, portMAX_DELAY) != pdTRUE)
            continue;
        size_t length = strlen(message.text), sent = 0;
        do
        {
            uint8_t chunk[HID_DECK_CHUNK] = {0};
            size_t n = length - sent > HID_DECK_CHUNK - 1 ? HID_DECK_CHUNK - 1 : length - sent;
            chunk[0] = (uint8_t)(n | (sent + n == length ? 0x80 : 0));
            memcpy(chunk + 1, message.text + sent, n);
            bool ok = false;
            for (int tries = 0; tries < 50 && !ok; tries++) /* the keyboard shares the endpoint */
            {
                ok = tud_mounted() && tud_hid_ready() && tud_hid_report(HID_REPORT_DECK, chunk, sizeof(chunk));
                if (!ok)
                    vTaskDelay(pdMS_TO_TICKS(4));
            }
            if (!ok)
                break; /* PC gone: drop the rest of this message */
            sent += n;
        } while (sent < length);
    }
}

bool hid_usb_deck_send(const char *message)
{
    if (!s_started || s_deck_queue == NULL || !tud_mounted())
        return false;
    deck_message_t copy;
    snprintf(copy.text, sizeof(copy.text), "%s", message);
    return xQueueSend(s_deck_queue, &copy, 0) == pdTRUE;
}
