#include "hid_private.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdatomic.h>

#include <string.h>

static const char *TAG = "hid_link";

#define QUEUE_LEN 256 /* reports: a long word is 2 reports per letter */
#define TX_STACK 4096
#define GAP_MS 8 /* pause between reports: the host never misses one */
#define RETRY_MS 10
#define RETRY_MAX 30 /* give up a report after ~300 ms */

/* ------------------------------------------------------- report map */

const uint8_t hid_report_map[HID_REPORT_MAP_BYTES] = {
    /* Keyboard, report id 1 */
    0x05,
    0x01,
    0x09,
    0x06,
    0xA1,
    0x01,
    0x85,
    HID_REPORT_KEYBOARD,
    0x05,
    0x07,
    0x19,
    0xE0,
    0x29,
    0xE7,
    0x15,
    0x00,
    0x25,
    0x01,
    0x75,
    0x01,
    0x95,
    0x08,
    0x81,
    0x02, /* modifiers */
    0x95,
    0x01,
    0x75,
    0x08,
    0x81,
    0x01, /* reserved */
    0x95,
    0x05,
    0x75,
    0x01,
    0x05,
    0x08,
    0x19,
    0x01,
    0x29,
    0x05,
    0x91,
    0x02, /* LEDs out */
    0x95,
    0x01,
    0x75,
    0x03,
    0x91,
    0x01, /* LED padding */
    0x95,
    0x06,
    0x75,
    0x08,
    0x15,
    0x00,
    0x25,
    0x65,
    0x05,
    0x07,
    0x19,
    0x00,
    0x29,
    0x65,
    0x81,
    0x00, /* 6 keys */
    0xC0,
    /* Media keys, report id 2 */
    0x05,
    0x0C,
    0x09,
    0x01,
    0xA1,
    0x01,
    0x85,
    HID_REPORT_MEDIA,
    0x15,
    0x00,
    0x26,
    0xFF,
    0x03,
    0x19,
    0x00,
    0x2A,
    0xFF,
    0x03,
    0x75,
    0x10,
    0x95,
    0x01,
    0x81,
    0x00,
    0xC0,
    /* Mouse, report id 3 */
    0x05,
    0x01,
    0x09,
    0x02,
    0xA1,
    0x01,
    0x85,
    HID_REPORT_MOUSE,
    0x09,
    0x01,
    0xA1,
    0x00,
    0x05,
    0x09,
    0x19,
    0x01,
    0x29,
    0x05,
    0x15,
    0x00,
    0x25,
    0x01,
    0x95,
    0x05,
    0x75,
    0x01,
    0x81,
    0x02, /* 5 buttons */
    0x95,
    0x01,
    0x75,
    0x03,
    0x81,
    0x01, /* padding */
    0x05,
    0x01,
    0x09,
    0x30,
    0x09,
    0x31,
    0x09,
    0x38,
    0x15,
    0x81,
    0x25,
    0x7F,
    0x75,
    0x08,
    0x95,
    0x03,
    0x81,
    0x06, /* x, y, wheel */
    /* Horizontal scrolling, Consumer AC Pan */
    0x05,
    0x0C,
    0x0A,
    0x38,
    0x02,
    0x95,
    0x01,
    0x81,
    0x06,
    0xC0,
    0xC0,
    /* Generic gamepad, report id 4: signed X/Y + 16 buttons (no XInput dependency). */
    0x05,
    0x01,
    0x09,
    0x05,
    0xA1,
    0x01,
    0x85,
    HID_REPORT_GAMEPAD,
    0x09,
    0x30,
    0x09,
    0x31,
    0x15,
    0x81,
    0x25,
    0x7F,
    0x75,
    0x08,
    0x95,
    0x02,
    0x81,
    0x02,
    0x05,
    0x09,
    0x19,
    0x01,
    0x29,
    0x10,
    0x15,
    0x00,
    0x25,
    0x01,
    0x75,
    0x01,
    0x95,
    0x10,
    0x81,
    0x02,
    0xC0,
};

const uint16_t hid_report_map_len = sizeof(hid_report_map);

/* If this fails, update HID_REPORT_MAP_BYTES (also used by the USB descriptor). */
_Static_assert(sizeof(hid_report_map) == HID_REPORT_MAP_BYTES, "report map size");

/* ------------------------------------------------------------ state */

/* One queue entry contains a complete press/release transaction. Producers
 * never wait in the LVGL task, and disconnect generations discard stale input. */
typedef struct
{
    uint32_t epoch;
    uint8_t report_id;
    uint8_t length;
    bool release;
    uint8_t data[8];
} report_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_producers;
static _Atomic(hid_link_transport_t) s_transport = HID_LINK_NONE;
static _Atomic(hid_link_state_t) s_state = HID_LINK_OFF;
static atomic_uint s_version;
static hid_host_layout_t s_layout = HID_HOST_IT;
static uint32_t s_reserved_epoch; /* protected by s_producers */

void hid_link_set_state(hid_link_state_t state)
{
    if (atomic_exchange(&s_state, state) != state)
    {
        atomic_fetch_add(&s_version, 1);
        ESP_LOGI(TAG, "%s",
                 state == HID_LINK_CONNECTED ? "Host connected"
                 : state == HID_LINK_WAITING ? "Waiting for the host"
                                             : "Off");
    }
}

static _Atomic(hid_link_router_t) s_router;

void hid_link_set_router(hid_link_router_t router)
{
    atomic_store(&s_router, router);
}

esp_err_t hid_link_transport_send(uint8_t report_id, const uint8_t *data, uint16_t length)
{
    return s_transport == HID_LINK_BLE   ? hid_ble_send(report_id, data, length)
           : s_transport == HID_LINK_USB ? hid_usb_send(report_id, data, length)
                                         : ESP_ERR_INVALID_STATE;
}

void hid_link_satellite_state(hid_link_state_t state)
{
    if (s_transport == HID_LINK_SATELLITE)
    {
        hid_link_set_state(state);
    }
}

static esp_err_t send_now(const report_t *r)
{
    hid_link_router_t router = atomic_load(&s_router);
    return router != NULL ? router(r->report_id, r->data, r->length)
                          : hid_link_transport_send(r->report_id, r->data, r->length);
}

static bool send_retried(const report_t *r)
{
    for (int attempt = 0; attempt < RETRY_MAX; attempt++)
    {
        if (s_state != HID_LINK_CONNECTED || r->epoch != s_version)
        {
            return false;
        }
        if (send_now(r) == ESP_OK)
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
    }
    return false;
}

static bool neutral_reports(uint32_t epoch)
{
    static const uint8_t lengths[] = {0, 8, 2, 5, 4};
    bool ok = true;
    for (uint8_t id = 1; id <= HID_REPORT_GAMEPAD; id++)
    {
        report_t zero = {.epoch = epoch, .report_id = id, .length = lengths[id]};
        ok = send_retried(&zero) && ok;
        vTaskDelay(pdMS_TO_TICKS(GAP_MS));
    }
    return ok;
}

static void tx_task(void *arg)
{
    (void)arg;
    report_t r;
    uint32_t seen_epoch = UINT32_MAX;
    for (;;)
    {
        if (xQueueReceive(s_queue, &r, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        if (s_state != HID_LINK_CONNECTED || r.epoch != s_version)
        {
            continue;
        }
        if (seen_epoch != r.epoch)
        {
            while (s_state == HID_LINK_CONNECTED && r.epoch == s_version && !neutral_reports(r.epoch))
            {
                vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            }
            seen_epoch = r.epoch;
        }
        bool sent = send_retried(&r);
        vTaskDelay(pdMS_TO_TICKS(GAP_MS));
        if (r.release)
        {
            memset(r.data, 0, sizeof(r.data));
            sent = send_retried(&r) && sent;
            vTaskDelay(pdMS_TO_TICKS(GAP_MS));
        }
        if (!sent)
        {
            /* Retry releases even if the queue is empty. A reconnect starts a
             * new generation and discards the old transaction automatically. */
            while (s_state == HID_LINK_CONNECTED && r.epoch == s_version && !neutral_reports(r.epoch))
            {
                vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            }
        }
    }
}

/* Caller owns s_producers, and reserved enough slots for the whole batch. */
static bool enqueue(uint8_t id, const uint8_t *data, uint8_t length, bool release)
{
    report_t r = {.epoch = s_reserved_epoch, .report_id = id, .length = length, .release = release};
    memcpy(r.data, data, length);
    return xQueueSend(s_queue, &r, 0) == pdTRUE;
}

static bool reserve(int count, bool neutral)
{
    if (s_queue == NULL || s_producers == NULL || s_state != HID_LINK_CONNECTED ||
        xSemaphoreTake(s_producers, 0) != pdTRUE)
    {
        return false;
    }
    /* Four emergency entries remain available to release held controls. */
    if (s_state != HID_LINK_CONNECTED || uxQueueSpacesAvailable(s_queue) < (unsigned)(count + (neutral ? 0 : 4)))
    {
        xSemaphoreGive(s_producers);
        return false;
    }
    s_reserved_epoch = s_version;
    return true;
}

static bool push(uint8_t id, const uint8_t *data, uint8_t length, bool release)
{
    if (!reserve(1, false))
    {
        return false;
    }
    bool ok = enqueue(id, data, length, release);
    xSemaphoreGive(s_producers);
    return ok;
}

esp_err_t hid_link_start(hid_link_transport_t transport, const char *name)
{
    if (transport != HID_LINK_BLE && transport != HID_LINK_USB && transport != HID_LINK_SATELLITE)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_queue == NULL)
    {
        s_producers = xSemaphoreCreateMutex();
        s_queue = xQueueCreate(QUEUE_LEN, sizeof(report_t));
        if (s_producers == NULL || s_queue == NULL ||
            xTaskCreatePinnedToCore(tx_task, "hid_tx", TX_STACK, NULL, 6, NULL, 0) != pdPASS)
        {
            if (s_queue != NULL)
                vQueueDelete(s_queue);
            if (s_producers != NULL)
                vSemaphoreDelete(s_producers);
            s_queue = NULL;
            s_producers = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (transport == s_transport)
    {
        return ESP_OK;
    }
    if (s_transport == HID_LINK_USB)
    {
        return ESP_ERR_INVALID_STATE;
    }
    hid_link_stop();
    s_transport = transport;
    hid_link_set_state(HID_LINK_WAITING); /* before asynchronous transport callbacks */
    esp_err_t err = transport == HID_LINK_BLE   ? hid_ble_start(name)
                    : transport == HID_LINK_USB ? hid_usb_start(name)
                                                : ESP_OK; /* satellite: the radio link reports the state */
    if (err != ESP_OK)
    {
        s_transport = HID_LINK_NONE;
        hid_link_set_state(HID_LINK_OFF);
        ESP_LOGE(TAG, "Start failed: %s", esp_err_to_name(err));
    }
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
    s_layout = layout == HID_HOST_US ? HID_HOST_US : HID_HOST_IT;
}
hid_host_layout_t hid_link_host_layout(void)
{
    return s_layout;
}

/* Strict UTF-8: an invalid sequence rejects the entire text transaction. */
static bool next_codepoint(const char **text, uint32_t *out)
{
    const uint8_t *p = (const uint8_t *)*text;
    uint32_t cp;
    int extra;
    uint32_t minimum;
    if (*p < 0x80)
    {
        cp = *p++;
        extra = 0;
        minimum = 0;
    }
    else if ((*p & 0xE0) == 0xC0)
    {
        cp = *p++ & 0x1F;
        extra = 1;
        minimum = 0x80;
    }
    else if ((*p & 0xF0) == 0xE0)
    {
        cp = *p++ & 0x0F;
        extra = 2;
        minimum = 0x800;
    }
    else if ((*p & 0xF8) == 0xF0)
    {
        cp = *p++ & 0x07;
        extra = 3;
        minimum = 0x10000;
    }
    else
    {
        return false;
    }
    for (int i = 0; i < extra; i++)
    {
        if ((*p & 0xC0) != 0x80)
            return false;
        cp = (cp << 6) | (*p++ & 0x3F);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return false;
    *text = (const char *)p;
    *out = cp;
    return true;
}

bool hid_link_replace_text(int backspaces, const char *utf8)
{
    if (utf8 == NULL || backspaces < 0 || backspaces > QUEUE_LEN - 4)
        return false;
    const char *cursor = utf8;
    int count = backspaces;
    uint8_t modifiers, keycode;
    uint32_t cp;
    while (*cursor)
    {
        if (!next_codepoint(&cursor, &cp) || !hid_keymap_lookup(s_layout, cp, &modifiers, &keycode) ||
            ++count > QUEUE_LEN - 4)
            return false;
    }
    if (count == 0)
        return true;
    if (!reserve(count, false))
        return false;
    bool ok = true;
    for (int i = 0; i < backspaces; i++)
    {
        uint8_t data[8] = {0, 0, HIDL_KEY_BACKSPACE};
        ok = enqueue(HID_REPORT_KEYBOARD, data, 8, true) && ok;
    }
    cursor = utf8;
    while (*cursor)
    {
        next_codepoint(&cursor, &cp);
        hid_keymap_lookup(s_layout, cp, &modifiers, &keycode);
        uint8_t data[8] = {modifiers, 0, keycode};
        ok = enqueue(HID_REPORT_KEYBOARD, data, 8, true) && ok;
    }
    xSemaphoreGive(s_producers);
    return ok;
}

bool hid_link_type(const char *utf8)
{
    return hid_link_replace_text(0, utf8);
}
bool hid_link_backspace(int count)
{
    return hid_link_replace_text(count, "");
}
bool hid_link_can_type(uint32_t cp)
{
    uint8_t modifiers, keycode;
    return hid_keymap_lookup(s_layout, cp, &modifiers, &keycode);
}
bool hid_link_key(uint8_t modifiers, uint8_t keycode)
{
    uint8_t data[8] = {modifiers, 0, keycode};
    return push(HID_REPORT_KEYBOARD, data, 8, true);
}
bool hid_link_media(uint16_t usage)
{
    uint8_t data[2] = {(uint8_t)usage, (uint8_t)(usage >> 8)};
    return push(HID_REPORT_MEDIA, data, 2, true);
}
bool hid_link_character(uint32_t codepoint, uint8_t extra_modifiers)
{
    uint8_t modifiers, code;
    return hid_keymap_lookup(s_layout, codepoint, &modifiers, &code) && hid_link_key(modifiers | extra_modifiers, code);
}
bool hid_link_pointer(int8_t dx, int8_t dy, uint8_t buttons, int8_t wheel, int8_t pan)
{
    uint8_t data[5] = {buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel, (uint8_t)pan};
    return push(HID_REPORT_MOUSE, data, 5, false);
}
bool hid_link_mouse(int8_t dx, int8_t dy, uint8_t buttons, int8_t wheel)
{
    return hid_link_pointer(dx, dy, buttons, wheel, 0);
}
bool hid_link_click(uint8_t buttons, int count)
{
    if (count < 1 || count > 2 || !reserve(count, false))
        return false;
    uint8_t data[5] = {buttons & 31, 0, 0, 0, 0};
    bool ok = true;
    for (int i = 0; i < count; i++)
    {
        ok = enqueue(HID_REPORT_MOUSE, data, 5, true) && ok;
    }
    xSemaphoreGive(s_producers);
    return ok;
}
bool hid_link_gamepad(int8_t x, int8_t y, uint16_t buttons)
{
    uint8_t data[4] = {(uint8_t)x, (uint8_t)y, (uint8_t)buttons, (uint8_t)(buttons >> 8)};
    return push(HID_REPORT_GAMEPAD, data, 4, false);
}
bool hid_link_release_all(void)
{
    if (!reserve(4, true))
        return false;
    uint8_t zero[8] = {0};
    bool ok = enqueue(HID_REPORT_KEYBOARD, zero, 8, false);
    ok = enqueue(HID_REPORT_MEDIA, zero, 2, false) && ok;
    ok = enqueue(HID_REPORT_MOUSE, zero, 5, false) && ok;
    ok = enqueue(HID_REPORT_GAMEPAD, zero, 4, false) && ok;
    xSemaphoreGive(s_producers);
    return ok;
}
void hid_link_forget_hosts(void)
{
    hid_ble_forget();
}
