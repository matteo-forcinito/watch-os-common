#include "hid_private.h"

#include "esp_hidd.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include <string.h>
#include <stdatomic.h>
#include "vault.h"
#include "esp_timer.h"
#include "nvs.h"

/* Vault-only authenticated BLE HID. Remote is unchanged. */

static const char *TAG = "hid_ble";

#define ADV_MIN BLE_GAP_ADV_ITVL_MS(30) /* fast: hosts find and reconnect quickly */
#define ADV_MAX BLE_GAP_ADV_ITVL_MS(50)

void ble_store_config_init(void);

static esp_hidd_dev_t *s_dev = NULL;
static char s_name[32] = "AMOLED Remote";
static uint8_t s_own_addr_type = 0;
static bool s_running = false;
static atomic_bool s_locked = true;
static atomic_uint s_passkey;
static atomic_uint s_handle = UINT16_MAX;
static atomic_llong s_pair_until;
static uint8_t s_address[6];

static esp_hid_raw_report_map_t s_maps[1];

static esp_hid_device_config_t s_config = {
    .vendor_id = 0x303A, /* Espressif */
    .product_id = 0x8211,
    .version = 0x0101,
    .device_name = s_name,
    .manufacturer_name = "Watch OS",
    .serial_number = "0001",
    .report_maps = s_maps,
    .report_maps_len = 1,
};

static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    if (atomic_load(&s_locked) || !vault_unlocked())
        return;
    struct ble_hs_adv_fields fields = {0};
    static ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = ESP_HID_APPEARANCE_KEYBOARD;
    fields.appearance_is_present = 1;
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);

    if (rc != 0)
    {
        ESP_LOGE(TAG, "Advertising data: %d", rc);
        return;
    }

    /* The service/flags stay in the advertisement, the name in scan response:
     * longer board names must never overflow BLE's 31-byte advertising limit. */
    struct ble_hs_adv_fields response = {0};
    response.name = (const uint8_t *)s_name;
    response.name_len = (uint8_t)strlen(s_name);
    response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Scan response: %d", rc);
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
            advertise();
        else
        {
            atomic_store(&s_handle, event->connect.conn_handle);
            if (!vault_unlocked())
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            else
                ble_gap_security_initiate(event->connect.conn_handle);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        atomic_store(&s_handle, UINT16_MAX);
        atomic_store(&s_passkey, 0);
        ESP_LOGI(TAG, "Disconnected");
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
        if (!vault_unlocked() || esp_timer_get_time() > atomic_load(&s_pair_until))
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        struct ble_gap_conn_desc descriptor;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &descriptor) != 0)
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        /* Replacement is authorized by the physical Pair action, for this
         * peer only and within the 60-second window. */
        ble_store_util_delete_peer(&descriptor.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
    {
        if (!vault_unlocked() || esp_timer_get_time() > atomic_load(&s_pair_until) ||
            event->passkey.params.action != BLE_SM_IOACT_DISP)
        {
            ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        uint32_t random;
        do
        {
            if (vault_random(&random, sizeof(random)) != ESP_OK)
            {
                ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }
        } while (random >= UINT32_MAX - (UINT32_MAX % 1000000U));
        struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP, .passkey = random % 1000000U};
        atomic_store(&s_passkey, io.passkey + 1);
        ble_sm_inject_io(event->passkey.conn_handle, &io);
        break;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        atomic_store(&s_passkey, 0);
        if (event->enc_change.status != 0 || !vault_hid_authenticated())
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
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
        if (ble_hs_id_set_rnd(s_address) != 0)
            break;
        s_own_addr_type = BLE_OWN_ADDR_RANDOM;
        advertise();
        hid_link_set_state(HID_LINK_WAITING);
        ESP_LOGI(TAG, "Advertising as \"%s\"", s_name);
        break;

    case ESP_HIDD_CONNECT_EVENT:
        hid_link_set_state(HID_LINK_CONNECTED);
        break;
    case ESP_HIDD_OUTPUT_EVENT:
    {
        const esp_hidd_event_data_t *event = data;
        if (event != NULL && event->output.report_id == HID_REPORT_KEYBOARD &&
            event->output.length >= 1 && event->output.data != NULL && vault_hid_authenticated())
            vault_hid_caps((event->output.data[0] & 2) != 0);
        break;
    }

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

    snprintf(s_name, sizeof(s_name), "%.29s", name != NULL ? name : "Watch Remote");
    s_maps[0].data = hid_report_map;
    s_maps[0].len = hid_report_map_len;

    esp_err_t err = nimble_port_init();

    if (err != ESP_OK)
    {
        return err;
    }

    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    nvs_handle_t preferences;
    if (nvs_open("vault_ui", NVS_READWRITE, &preferences) != ESP_OK)
    {
        nimble_port_deinit();
        return ESP_FAIL;
    }
    size_t size = sizeof(s_address);
    if (nvs_get_blob(preferences, "ble_addr", s_address, &size) != ESP_OK || size != 6 ||
        (s_address[5] & 0xc0) != 0xc0)
    {
        if (vault_random(s_address, sizeof(s_address)) != ESP_OK)
        {
            nvs_close(preferences);
            nimble_port_deinit();
            return ESP_FAIL;
        }
        s_address[5] |= 0xc0;
        err = nvs_set_blob(preferences, "ble_addr", s_address, sizeof(s_address));
        if (err == ESP_OK)
            err = nvs_commit(preferences);
        if (err != ESP_OK)
        {
            nvs_close(preferences);
            nimble_port_deinit();
            return err;
        }
    }
    nvs_close(preferences);
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
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
    if (s_dev == NULL || !esp_hidd_dev_connected(s_dev) || !vault_hid_authenticated() ||
        !vault_unlocked())
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

bool vault_hid_authenticated(void)
{
    if (hid_link_transport() == HID_LINK_USB)
        return hid_link_state() == HID_LINK_CONNECTED;
    struct ble_gap_conn_desc descriptor;
    unsigned handle = atomic_load(&s_handle);
    return vault_unlocked() && handle != UINT16_MAX &&
           ble_gap_conn_find(handle, &descriptor) == 0 && descriptor.sec_state.encrypted &&
           descriptor.sec_state.authenticated && descriptor.sec_state.key_size == 16;
}

unsigned vault_hid_passkey(void)
{
    return atomic_load(&s_passkey);
}

void vault_hid_pair(void)
{
    if (vault_unlocked())
        atomic_store(&s_pair_until, esp_timer_get_time() + 60000000);
}

void vault_hid_lock(bool locked)
{
    atomic_store(&s_locked, locked);
    if (hid_link_transport() != HID_LINK_BLE)
        return;
    if (locked)
    {
        atomic_store(&s_passkey, 0);
        atomic_store(&s_pair_until, 0);
        ble_gap_adv_stop();
        unsigned handle = atomic_load(&s_handle);
        if (handle != UINT16_MAX)
            ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    else if (atomic_load(&s_handle) == UINT16_MAX)
        advertise();
}
