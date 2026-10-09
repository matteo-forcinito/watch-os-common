#include "hid_private.h"

#include "esp_hidd.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include <string.h>

/*
 * Bluetooth LE keyboard (HID over GATT) with ESP-IDF's esp_hid on NimBLE.
 *
 * Pairing is "just works" with bonding: the host (Windows, macOS, Android,
 * iOS, Linux) pairs from its Bluetooth settings without a PIN, and
 * reconnects by itself afterwards. If the host forgot the watch, the old
 * bond is dropped and pairing starts again.
 */

static const char *TAG = "hid_ble";

#define ADV_MIN  BLE_GAP_ADV_ITVL_MS(30)   /* fast: hosts find and reconnect quickly */
#define ADV_MAX  BLE_GAP_ADV_ITVL_MS(50)

void ble_store_config_init(void);

static esp_hidd_dev_t *s_dev = NULL;
static char s_name[32] = "AMOLED Remote";
static uint8_t s_own_addr_type = 0;
static bool s_running = false;

static esp_hid_raw_report_map_t s_maps[1];

static esp_hid_device_config_t s_config = {
    .vendor_id = 0x303A,   /* Espressif */
    .product_id = 0x8211,
    .version = 0x0100,
    .device_name = s_name,
    .manufacturer_name = "AMOLED Watch",
    .serial_number = "0001",
    .report_maps = s_maps,
    .report_maps_len = 1,
};

static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    static ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = ESP_HID_APPEARANCE_KEYBOARD;
    fields.appearance_is_present = 1;
    fields.name = (const uint8_t *)s_name;
    fields.name_len = (uint8_t)strlen(s_name);
    fields.name_is_complete = 1;
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);

    if (rc != 0)
    {
        ESP_LOGE(TAG, "Advertising data: %d", rc);
        return;
    }

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = ADV_MIN,
        .itvl_max = ADV_MAX,
    };

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);

    if (rc != 0 && rc != BLE_HS_EALREADY)
    {
        ESP_LOGE(TAG, "Advertising start: %d", rc);
    }
}

/* Our own GAP events (esp_hidd has its own listener for the HID side). */
static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type)
    {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0)
            {
                advertise();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected (reason %d)", event->disconnect.reason);
            if (s_running)
            {
                advertise();
            }
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            if (s_running)
            {
                advertise();
            }
            break;

        case BLE_GAP_EVENT_REPEAT_PAIRING:
        {
            /* The host forgot us and pairs again: drop the old bond, accept the new one. */
            struct ble_gap_conn_desc desc;

            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }

            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        case BLE_GAP_EVENT_PASSKEY_ACTION:
        {
            /* "Just works": accept a numeric comparison if the host asks for one. */
            struct ble_sm_io io = {0};
            io.action = event->passkey.params.action;

            if (io.action == BLE_SM_IOACT_NUMCMP)
            {
                io.numcmp_accept = 1;
                ble_sm_inject_io(event->passkey.conn_handle, &io);
            }
            break;
        }

        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "Encryption: %s", event->enc_change.status == 0 ? "on" : "failed");
            break;

        default:
            break;
    }

    return 0;
}

static void hidd_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    switch ((esp_hidd_event_t)id)
    {
        case ESP_HIDD_START_EVENT:
            ble_hs_util_ensure_addr(0);
            ble_hs_id_infer_auto(0, &s_own_addr_type);
            advertise();
            hid_link_set_state(HID_LINK_WAITING);
            ESP_LOGI(TAG, "Advertising as \"%s\"", s_name);
            break;

        case ESP_HIDD_CONNECT_EVENT:
            hid_link_set_state(HID_LINK_CONNECTED);
            break;

        case ESP_HIDD_DISCONNECT_EVENT:
            hid_link_set_state(s_running ? HID_LINK_WAITING : HID_LINK_OFF);
            break;

        default:
            break;
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t hid_ble_start(const char *name)
{
    if (s_running)
    {
        return ESP_OK;
    }

    snprintf(s_name, sizeof(s_name), "%s", name != NULL ? name : "AMOLED Remote");
    s_maps[0].data = hid_report_map;
    s_maps[0].len = hid_report_map_len;

    esp_err_t err = nimble_port_init();

    if (err != ESP_OK)
    {
        return err;
    }

    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    err = esp_hidd_dev_init(&s_config, ESP_HID_TRANSPORT_BLE, hidd_event, &s_dev);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HID device: %s", esp_err_to_name(err));
        nimble_port_deinit();
        return err;
    }

    ble_svc_gap_device_name_set(s_name);
    ble_svc_gap_device_appearance_set(ESP_HID_APPEARANCE_KEYBOARD);
    esp_hidd_dev_battery_set(s_dev, 100);
    ble_store_config_init();

    s_running = true;
    nimble_port_freertos_init(host_task);

    return ESP_OK;
}

void hid_ble_stop(void)
{
    if (!s_running)
    {
        return;
    }

    s_running = false;
    ble_gap_adv_stop();

    if (s_dev != NULL)
    {
        esp_hidd_dev_deinit(s_dev);
        s_dev = NULL;
    }

    if (nimble_port_stop() == 0)
    {
        nimble_port_deinit();
    }
}

esp_err_t hid_ble_send(uint8_t report_id, const uint8_t *data, uint16_t length)
{
    if (s_dev == NULL || !esp_hidd_dev_connected(s_dev))
    {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_hidd_dev_input_set(s_dev, 0, report_id, (uint8_t *)data, length);
}

void hid_ble_forget(void)
{
    if (s_running)
    {
        ble_store_clear();
        ESP_LOGI(TAG, "All pairings removed");
    }
}
