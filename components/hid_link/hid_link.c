#include "hid_private.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "hid_link";

#define QUEUE_LEN      256     /* reports: a long word is 2 reports per letter */
#define TX_STACK       4096
#define GAP_MS         8       /* pause between reports: the host never misses one */
#define RETRY_MS       10
#define RETRY_MAX      30      /* give up a report after ~300 ms */

/* ------------------------------------------------------- report map */

const uint8_t hid_report_map[HID_REPORT_MAP_BYTES] = {
    /* Keyboard, report id 1 */
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, HID_REPORT_KEYBOARD,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,  /* modifiers */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                                                              /* reserved */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,                          /* LEDs out */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,                                                              /* LED padding */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,  /* 6 keys */
    0xC0,
    /* Media keys, report id 2 */
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, HID_REPORT_MEDIA,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
    /* Mouse, report id 3 */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, HID_REPORT_MOUSE, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,  /* 5 buttons */
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,                                                              /* padding */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06,                                                                                      /* x, y, wheel */
    0xC0, 0xC0,
};

const uint16_t hid_report_map_len = sizeof(hid_report_map);

/* If this fails, update HID_REPORT_MAP_BYTES (also used by the USB descriptor). */
_Static_assert(sizeof(hid_report_map) == HID_REPORT_MAP_BYTES, "report map size");

/* ------------------------------------------------------------ state */

typedef struct
{
    uint8_t report_id;
    uint8_t length;
    uint8_t data[8];
} report_t;

static QueueHandle_t s_queue = NULL;
static volatile hid_link_transport_t s_transport = HID_LINK_NONE;
static volatile hid_link_state_t s_state = HID_LINK_OFF;
static volatile uint32_t s_version = 0;
static hid_host_layout_t s_layout = HID_HOST_IT;

void hid_link_set_state(hid_link_state_t state)
{
    if (state != s_state)
    {
        s_state = state;
        s_version++;
        ESP_LOGI(TAG, "%s", state == HID_LINK_CONNECTED ? "Host connected" : (state == HID_LINK_WAITING ? "Waiting for the host" : "Off"));

        if (state != HID_LINK_CONNECTED && s_queue != NULL)
        {
            xQueueReset(s_queue);   /* do not type old keys into the next host */
        }
    }
}

static esp_err_t send_now(const report_t *r)
{
    if (s_transport == HID_LINK_BLE)
    {
        return hid_ble_send(r->report_id, r->data, r->length);
    }

    if (s_transport == HID_LINK_USB)
    {
        return hid_usb_send(r->report_id, r->data, r->length);
    }

    return ESP_ERR_INVALID_STATE;
}

static void tx_task(void *arg)
{
    (void)arg;

    report_t r;

    while (true)
    {
        if (xQueueReceive(s_queue, &r, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        /* Retry while the link is busy (BLE notification buffers, USB endpoint). */
        for (int attempt = 0; attempt < RETRY_MAX && s_state == HID_LINK_CONNECTED; attempt++)
        {
            if (send_now(&r) == ESP_OK)
            {
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
        }

        vTaskDelay(pdMS_TO_TICKS(GAP_MS));
    }
}

static bool push(uint8_t report_id, const uint8_t *data, uint8_t length)
{
    if (s_queue == NULL || s_state != HID_LINK_CONNECTED)
    {
        return false;
    }

    report_t r = {.report_id = report_id, .length = length};
    memcpy(r.data, data, length);

    return xQueueSend(s_queue, &r, pdMS_TO_TICKS(20)) == pdTRUE;
}

/* Press + release of one key: two reports, queued together or not at all. */
static bool push_key(uint8_t modifiers, uint8_t keycode)
{
    if (s_queue == NULL || uxQueueSpacesAvailable(s_queue) < 2)
    {
        return false;
    }

    uint8_t down[8] = {modifiers, 0, keycode, 0, 0, 0, 0, 0};
    uint8_t up[8] = {0};

    return push(HID_REPORT_KEYBOARD, down, 8) && push(HID_REPORT_KEYBOARD, up, 8);
}

/* ------------------------------------------------------------ public */

esp_err_t hid_link_start(hid_link_transport_t transport, const char *name)
{
    if (s_queue == NULL)
    {
        s_queue = xQueueCreate(QUEUE_LEN, sizeof(report_t));

        if (s_queue == NULL || xTaskCreatePinnedToCore(tx_task, "hid_tx", TX_STACK, NULL, 6, NULL, 0) != pdPASS)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    if (transport == s_transport)
    {
        return ESP_OK;
    }

    if (s_transport == HID_LINK_USB)
    {
        return ESP_ERR_INVALID_STATE;   /* USB keeps the port until a restart */
    }

    hid_link_stop();

    esp_err_t err = transport == HID_LINK_BLE ? hid_ble_start(name) : hid_usb_start(name);

    if (err == ESP_OK)
    {
        s_transport = transport;
        hid_link_set_state(HID_LINK_WAITING);
    }
    else
    {
        ESP_LOGE(TAG, "Start failed: %s", esp_err_to_name(err));
    }

    s_version++;

    return err;
}

void hid_link_stop(void)
{
    if (s_transport == HID_LINK_BLE)
    {
        hid_ble_stop();
        s_transport = HID_LINK_NONE;
        hid_link_set_state(HID_LINK_OFF);
    }
}

hid_link_transport_t hid_link_transport(void)
{
    return s_transport;
}

hid_link_state_t hid_link_state(void)
{
    return s_state;
}

uint32_t hid_link_version(void)
{
    return s_version;
}

void hid_link_set_host_layout(hid_host_layout_t layout)
{
    s_layout = layout;
}

hid_host_layout_t hid_link_host_layout(void)
{
    return s_layout;
}

/* Next UTF-8 code point (0 at the end). Invalid bytes are skipped. */
static uint32_t next_codepoint(const char **text)
{
    const uint8_t *p = (const uint8_t *)*text;
    uint32_t cp = 0;
    int extra = 0;

    if (*p == 0)
    {
        return 0;
    }

    if (*p < 0x80)
    {
        cp = *p++;
    }
    else if ((*p & 0xE0) == 0xC0)
    {
        cp = *p++ & 0x1F;
        extra = 1;
    }
    else if ((*p & 0xF0) == 0xE0)
    {
        cp = *p++ & 0x0F;
        extra = 2;
    }
    else if ((*p & 0xF8) == 0xF0)
    {
        cp = *p++ & 0x07;
        extra = 3;
    }
    else
    {
        p++;
        cp = 0xFFFD;
    }

    while (extra-- > 0 && (*p & 0xC0) == 0x80)
    {
        cp = (cp << 6) | (*p++ & 0x3F);
    }

    *text = (const char *)p;
    return cp;
}

bool hid_link_type(const char *utf8)
{
    bool ok = true;
    uint32_t cp;

    while ((cp = next_codepoint(&utf8)) != 0)
    {
        uint8_t modifiers = 0;
        uint8_t keycode = 0;

        if (!hid_keymap_lookup(s_layout, cp, &modifiers, &keycode))
        {
            continue;   /* not on the host keyboard: skipped */
        }

        ok = push_key(modifiers, keycode) && ok;
    }

    return ok;
}

bool hid_link_can_type(uint32_t codepoint)
{
    uint8_t modifiers;
    uint8_t keycode;
    return hid_keymap_lookup(s_layout, codepoint, &modifiers, &keycode);
}

bool hid_link_key(uint8_t modifiers, uint8_t keycode)
{
    return push_key(modifiers, keycode);
}

bool hid_link_backspace(int count)
{
    bool ok = true;

    for (int i = 0; i < count; i++)
    {
        ok = push_key(0, HIDL_KEY_BACKSPACE) && ok;
    }

    return ok;
}

bool hid_link_media(uint16_t usage)
{
    uint8_t down[2] = {(uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
    uint8_t up[2] = {0, 0};
    return push(HID_REPORT_MEDIA, down, 2) && push(HID_REPORT_MEDIA, up, 2);
}

bool hid_link_mouse(int8_t dx, int8_t dy, uint8_t buttons, int8_t wheel)
{
    uint8_t report[4] = {buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel};
    return push(HID_REPORT_MOUSE, report, 4);
}

void hid_link_forget_hosts(void)
{
    hid_ble_forget();
}
