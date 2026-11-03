#include "hid_private.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"

#include <string.h>

/*
 * Deck data channel: a small GATT service beside HID over GATT, on the same
 * Bluetooth connection and bond. The PC agent (tools/deck_agent) writes JSON
 * messages to RX and subscribes to TX; keys, mouse and media stay HID reports.
 * Both characteristics require an encrypted link, so only a paired host can use them.
 * On USB the same messages travel in vendor HID report 5 (hid_usb.c).
 */

static const char *TAG = "hid_deck";

/* 7a3e0000-5c1d-4e8b-9f2a-d3c0de0decc0 + characteristic in bytes 12..13 */
#define DECK_UUID(c)                                                                                                   \
    BLE_UUID128_INIT(0xc0, 0xec, 0x0d, 0xde, 0xc0, 0xd3, 0x2a, 0x9f, 0x8b, 0x4e, 0x1d, 0x5c, (c), 0x00, 0x3e, 0x7a)

static const ble_uuid128_t s_service_uuid = DECK_UUID(0x00);
static const ble_uuid128_t s_rx_uuid = DECK_UUID(0x01);
static const ble_uuid128_t s_tx_uuid = DECK_UUID(0x02);

static uint16_t s_tx_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed;
static hid_deck_rx_t s_receiver;

static int access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        return BLE_ATT_ERR_UNLIKELY;
    }

    char line[HID_DECK_MAX + 1];
    uint16_t length = 0;

    if (ble_hs_mbuf_to_flat(ctxt->om, line, HID_DECK_MAX, &length) != 0)
    {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    line[length] = '\0';
    s_conn = conn;

    if (s_receiver != NULL)
    {
        s_receiver(line, length);
    }

    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &s_rx_uuid.u,
                    .access_cb = access,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
                },
                {
                    .uuid = &s_tx_uuid.u,
                    .access_cb = access,
                    .val_handle = &s_tx_handle,
                    .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
                },
                {0},
            },
    },
    {0},
};

esp_err_t hid_deck_register(void)
{
    int rc = ble_gatts_count_cfg(s_services);

    if (rc == 0)
    {
        rc = ble_gatts_add_svcs(s_services);
    }

    if (rc != 0)
    {
        ESP_LOGE(TAG, "GATT service: %d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

void hid_deck_subscribe(uint16_t conn, uint16_t attr, bool notify)
{
    if (attr != s_tx_handle)
    {
        return;
    }

    s_conn = conn;
    s_subscribed = notify;
    ESP_LOGI(TAG, "Agent %s", notify ? "subscribed" : "left");
}

void hid_deck_disconnected(void)
{
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
}

void hid_deck_set_receiver(hid_deck_rx_t receiver)
{
    s_receiver = receiver;
}

/* USB: reassemble [length | 0x80 last] + data chunks into one message. */
void hid_deck_usb_frame(const uint8_t *chunk, uint16_t length)
{
    static char message[HID_DECK_MAX + 1];
    static size_t used;
    static TickType_t last;

    /* The chunks of one message arrive back to back: a part older than a second
     * belongs to a message cut by an unplugged cable and must not prefix this one. */
    TickType_t now = xTaskGetTickCount();
    if (now - last > pdMS_TO_TICKS(1000))
        used = 0;
    last = now;
    if (length < 1)
        return;
    size_t n = chunk[0] & 0x3F;
    if (n > (size_t)length - 1 || used + n > HID_DECK_MAX)
    {
        used = 0; /* malformed or too long: start over */
        return;
    }
    memcpy(message + used, chunk + 1, n);
    used += n;
    if (chunk[0] & 0x80)
    {
        message[used] = '\0';
        if (s_receiver != NULL)
            s_receiver(message, used);
        used = 0;
    }
}

bool hid_deck_connected(void)
{
    if (hid_link_transport() == HID_LINK_USB)
        return hid_link_state() == HID_LINK_CONNECTED;
    return s_subscribed;
}

bool hid_deck_send(const char *line)
{
    if (hid_link_transport() == HID_LINK_USB)
        return line != NULL && hid_usb_deck_send(line);
    if (!s_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE || line == NULL)
    {
        return false;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(line, (uint16_t)strnlen(line, HID_DECK_MAX));

    return om != NULL && ble_gatts_notify_custom(s_conn, s_tx_handle, om) == 0;
}
