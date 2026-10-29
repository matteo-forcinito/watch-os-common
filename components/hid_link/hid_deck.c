#include "hid_private.h"

#include "esp_log.h"
#include "host/ble_hs.h"

#include <string.h>

/*
 * Deck data channel: a small GATT service beside HID over GATT, on the same
 * Bluetooth connection and bond. The PC agent (tools/deck_agent) writes JSON
 * messages to RX and subscribes to TX; keys, mouse and media stay HID reports.
 * Both characteristics require an encrypted link, so only a paired host can use them.
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

bool hid_deck_connected(void)
{
    return s_subscribed;
}

bool hid_deck_send(const char *line)
{
    if (!s_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE || line == NULL)
    {
        return false;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(line, (uint16_t)strnlen(line, HID_DECK_MAX));

    return om != NULL && ble_gatts_notify_custom(s_conn, s_tx_handle, om) == 0;
}
