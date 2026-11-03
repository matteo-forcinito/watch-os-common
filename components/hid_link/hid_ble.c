#include "hid_private.h"

#include "esp_hidd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
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
 *
 * Several hosts can be paired. With none chosen, the first one that connects
 * wins (the old behaviour). Once one is chosen on the watch, every other host
 * is turned away: at once when its address is known, otherwise as soon as
 * encryption reveals its identity. New hosts may pair only while
 * hid_ble_pair_host() keeps the door open. Choosing another host while one is
 * connected drops it, so the switch happens from the watch alone.
 */

static const char *TAG = "hid_ble";

#define ADV_MIN BLE_GAP_ADV_ITVL_MS(30) /* fast: hosts find and reconnect quickly */
#define ADV_MAX BLE_GAP_ADV_ITVL_MS(50)

void ble_store_config_init(void);

static esp_hidd_dev_t *s_dev = NULL;
static char s_name[32] = "AMOLED Remote";
static uint8_t s_own_addr_type = 0;
static bool s_running = false;
static bool s_connected = false;

#define HOSTS_NVS "hid_hosts" /* chosen host and host names, keyed by address */
#define NO_HOST 0xFF          /* ble_addr_t type: no host chosen, any paired one connects */
#define BONDS_MAX MYNEWT_VAL(BLE_STORE_MAX_BONDS)

static ble_addr_t s_target = {.type = NO_HOST}; /* the host chosen on the watch */
static bool s_pairing;                          /* a new host may pair: it becomes the target */
static ble_addr_t s_known[BONDS_MAX];           /* bonds when pairing opened */
static int s_known_count;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static ble_addr_t s_peer;    /* identity of the connected host, once encrypted */
static bool s_accepted;      /* the connected host passed the check: reports may flow */
static bool s_repaired;      /* the connected host paired again during this connection */
static char s_peer_name[24]; /* from the Deck agent, saved off the host task */
static esp_timer_handle_t s_name_timer;

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

static void addr_key(const ble_addr_t *addr, char key[16])
{
    snprintf(key, 16, "%02x%02x%02x%02x%02x%02x%x", addr->val[5], addr->val[4], addr->val[3], addr->val[2],
             addr->val[1], addr->val[0], addr->type & 0xF);
}

static int bonded_peers(ble_addr_t *out)
{
    int count = 0;
    return ble_store_util_bonded_peers(out, &count, BONDS_MAX) == 0 ? count : 0;
}

static bool in_list(const ble_addr_t *addr, const ble_addr_t *list, int count)
{
    for (int i = 0; i < count; i++)
        if (ble_addr_cmp(addr, &list[i]) == 0)
            return true;
    return false;
}

static bool is_bonded(const ble_addr_t *addr)
{
    ble_addr_t peers[BONDS_MAX];
    return in_list(addr, peers, bonded_peers(peers));
}

static void save_target(void)
{
    nvs_handle_t nvs;
    if (nvs_open(HOSTS_NVS, NVS_READWRITE, &nvs) != ESP_OK)
        return;
    if (s_target.type == NO_HOST)
        nvs_erase_key(nvs, "target");
    else
        nvs_set_blob(nvs, "target", &s_target, sizeof(s_target));
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void load_target(void)
{
    nvs_handle_t nvs;
    size_t size = sizeof(s_target);
    s_target.type = NO_HOST;
    if (nvs_open(HOSTS_NVS, NVS_READONLY, &nvs) != ESP_OK)
        return;
    if (nvs_get_blob(nvs, "target", &s_target, &size) != ESP_OK || size != sizeof(s_target))
        s_target.type = NO_HOST;
    nvs_close(nvs);
}

/* Bonding only while a new host is welcome: a stranger never evicts a paired host. */
static void apply_policy(void)
{
    ble_hs_cfg.sm_bonding = s_target.type == NO_HOST || s_pairing;
}

/* Whether the host with this identity may stay connected. */
static bool host_allowed(const ble_addr_t *peer)
{
    if (s_pairing)
        return s_repaired || !in_list(peer, s_known, s_known_count); /* only the new host */
    return s_target.type == NO_HOST || ble_addr_cmp(peer, &s_target) == 0;
}

static void accept_host(void)
{
    s_accepted = true;
    if (s_pairing)
    {
        s_pairing = false;
        s_target = s_peer;
        save_target();
        apply_policy();
        ESP_LOGI(TAG, "New host paired and chosen");
    }
    if (s_dev != NULL && esp_hidd_dev_connected(s_dev))
        hid_link_set_state(HID_LINK_CONNECTED);
}

static void turn_away(uint16_t conn)
{
    ESP_LOGI(TAG, "Another host is chosen: disconnecting this one");
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

static void save_name(void *arg)
{
    (void)arg;
    char key[16], name[sizeof(s_peer_name)], old[sizeof(s_peer_name)] = "";
    ble_addr_t peer = s_peer;
    snprintf(name, sizeof(name), "%s", s_peer_name);
    if (!s_accepted || name[0] == '\0')
        return;
    addr_key(&peer, key);
    nvs_handle_t nvs;
    if (nvs_open(HOSTS_NVS, NVS_READWRITE, &nvs) != ESP_OK)
        return;
    size_t size = sizeof(old);
    if (nvs_get_str(nvs, key, old, &size) != ESP_OK || strcmp(old, name) != 0)
    {
        nvs_set_str(nvs, key, name);
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

static void advertise(void)
{
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

/*
 * While a host is connected: slow, non-connectable advertising with the same
 * address, so the PC agent can find the watch Windows already holds and open
 * the Deck service over that link. Stops once the agent subscribes.
 */
static void advertise_beacon(void)
{
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(1000),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(1200),
    };

    ble_gap_adv_stop();
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY)
    {
        ESP_LOGW(TAG, "Beacon advertising: %d", rc);
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
        else
        {
            struct ble_gap_conn_desc desc;
            s_connected = true;
            s_conn = event->connect.conn_handle;
            s_accepted = s_repaired = false;
            /* A paired host using no private address shows its identity at once. */
            if (ble_gap_conn_find(s_conn, &desc) == 0 && is_bonded(&desc.peer_id_addr) &&
                !host_allowed(&desc.peer_id_addr))
            {
                turn_away(s_conn);
                break;
            }
            advertise_beacon(); /* lets the PC agent find the connected watch */
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        hid_deck_subscribe(event->subscribe.conn_handle, event->subscribe.attr_handle, event->subscribe.cur_notify);
        if (event->subscribe.cur_notify)
        {
            ble_gap_adv_stop();
        }
        else if (s_connected)
        {
            advertise_beacon();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected (reason %d)", event->disconnect.reason);
        s_connected = false;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_accepted = s_repaired = false;
        hid_deck_disconnected();
        ble_gap_adv_stop();
        if (s_running)
        {
            advertise();
        }
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_running && !s_connected)
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
        s_repaired = true;

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
    {
        ESP_LOGI(TAG, "Encryption: %s", event->enc_change.status == 0 ? "on" : "failed");
        struct ble_gap_conn_desc desc;
        if (event->enc_change.status != 0 || ble_gap_conn_find(event->enc_change.conn_handle, &desc) != 0)
            break;
        s_peer = desc.peer_id_addr; /* the identity, resolved by now */
        if (host_allowed(&s_peer))
            accept_host();
        else
            turn_away(event->enc_change.conn_handle);
        break;
    }

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
        if (s_accepted) /* otherwise the encryption check reports it */
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

    snprintf(s_name, sizeof(s_name), "%.29s", name != NULL ? name : "Watch Remote");
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
    load_target();
    s_pairing = false;
    apply_policy();
    if (s_name_timer == NULL)
    {
        const esp_timer_create_args_t args = {.callback = save_name, .name = "hid_name"};
        esp_timer_create(&args, &s_name_timer);
    }

    err = esp_hidd_dev_init(&s_config, ESP_HID_TRANSPORT_BLE, hidd_event, &s_dev);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HID device: %s", esp_err_to_name(err));
        nimble_port_deinit();
        return err;
    }

    hid_deck_register();
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
        s_target.type = NO_HOST;
        s_pairing = false;
        apply_policy();
        nvs_handle_t nvs;
        if (nvs_open(HOSTS_NVS, NVS_READWRITE, &nvs) == ESP_OK)
        {
            nvs_erase_all(nvs); /* this component's namespace only: names and chosen host */
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        ESP_LOGI(TAG, "All pairings removed");
    }
}

static void host_addr(const hid_link_host_t *host, ble_addr_t *addr)
{
    addr->type = host->id[0];
    memcpy(addr->val, &host->id[1], 6);
}

int hid_ble_hosts(hid_link_host_t *hosts, int max)
{
    if (!s_running)
        return 0;
    ble_addr_t peers[BONDS_MAX];
    int count = bonded_peers(peers);
    if (s_target.type != NO_HOST && !in_list(&s_target, peers, count))
    {
        s_target.type = NO_HOST; /* its bond is gone (forgotten or replaced) */
        save_target();
        apply_policy();
    }
    nvs_handle_t nvs;
    bool names = nvs_open(HOSTS_NVS, NVS_READONLY, &nvs) == ESP_OK;
    int n = 0;
    for (; n < count && n < max; n++)
    {
        hid_link_host_t *host = &hosts[n];
        memset(host, 0, sizeof(*host));
        host->id[0] = peers[n].type;
        memcpy(&host->id[1], peers[n].val, 6);
        host->connected = s_accepted && ble_addr_cmp(&peers[n], &s_peer) == 0;
        host->chosen = s_target.type != NO_HOST && ble_addr_cmp(&peers[n], &s_target) == 0;
        char key[16];
        size_t size = sizeof(host->name);
        addr_key(&peers[n], key);
        if (!names || nvs_get_str(nvs, key, host->name, &size) != ESP_OK)
            snprintf(host->name, sizeof(host->name), "Computer %02X%02X", peers[n].val[1], peers[n].val[0]);
    }
    if (names)
        nvs_close(nvs);
    return n;
}

void hid_ble_choose_host(const hid_link_host_t *host)
{
    if (!s_running)
        return;
    if (host == NULL)
        s_target.type = NO_HOST;
    else
        host_addr(host, &s_target);
    s_pairing = false;
    apply_policy();
    save_target();
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE && s_accepted && !host_allowed(&s_peer))
        turn_away(conn); /* the disconnect restarts advertising for the chosen host */
}

void hid_ble_pair_host(bool open)
{
    if (!s_running)
        return;
    if (open)
        s_known_count = bonded_peers(s_known);
    s_pairing = open;
    apply_policy();
    uint16_t conn = s_conn;
    if (open && conn != BLE_HS_CONN_HANDLE_NONE)
        turn_away(conn); /* free the only connection for the new host */
}

bool hid_ble_pairing(void)
{
    return s_pairing;
}

void hid_ble_forget_host(const hid_link_host_t *host)
{
    if (!s_running || host == NULL)
        return;
    ble_addr_t addr;
    host_addr(host, &addr);
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE && s_accepted && ble_addr_cmp(&addr, &s_peer) == 0)
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    ble_store_util_delete_peer(&addr);
    if (s_target.type != NO_HOST && ble_addr_cmp(&addr, &s_target) == 0)
    {
        s_target.type = NO_HOST;
        save_target();
        apply_policy();
    }
    char key[16];
    addr_key(&addr, key);
    nvs_handle_t nvs;
    if (nvs_open(HOSTS_NVS, NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_erase_key(nvs, key);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* Deck agent hello, on the host task: keep the name, write it from the timer task. */
void hid_ble_name_host(const char *name)
{
    if (!s_accepted || name == NULL || name[0] == '\0' || s_name_timer == NULL)
        return;
    snprintf(s_peer_name, sizeof(s_peer_name), "%s", name);
    esp_timer_stop(s_name_timer);
    esp_timer_start_once(s_name_timer, 1000 * 1000);
}
