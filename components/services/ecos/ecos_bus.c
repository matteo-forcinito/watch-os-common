/* ECOS bus: the one owner of ESP-NOW in the OS (docs/ORBIT-ESPNOW.md, phase 2).
 *
 *   service (pairing EC01, Famiglia EL02, Orbit later)
 *        │ ecos_bus_open("EL02", recv, sent, tx_power)
 *        ▼
 *   bus: radio lease + esp_now_init on the first open, deinit on the last close;
 *        incoming frames go to the service whose 4-byte tag starts the frame;
 *        send results go to every open service that asked for them;
 *        TX power = the lowest any open service asked for, restored at the end.
 *
 * At rest: no task, no queue, no radio. Each service keeps its own queue,
 * keys and frame format; the bus only shares the radio between them. */
#include "ecos_bus.h"
#include "services/wifi.h"

#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

#define BUS_SERVICES 4

static const char *TAG = "ecos_bus";

typedef struct
{
    char tag[4];
    uint8_t pattern[4];
    uint8_t mask[4];
    ecos_bus_recv_t recv;
    ecos_bus_sent_t sent;
    int8_t tx_power;   /* 0: no limit */
    bool used;
    unsigned active;   /* callbacks already dispatched; close drains them */
} bus_service_t;

static bus_service_t s_services[BUS_SERVICES];
static portMUX_TYPE s_table = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;   /* open/close: radio up and down */
static int s_open;
static int8_t s_saved_power;
static bool s_power_lowered;
/* ESP-NOW send callbacks carry only the destination MAC, so permit one
 * outstanding send globally and retain its owner until callback/deinit. */
static bool s_tx_pending;
static bool s_tx_callback_enabled;
static int s_tx_owner = -1;
static uint8_t s_tx_mac[6];
static uint32_t s_tx_generation;
static uint32_t s_service_generation[BUS_SERVICES];

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int length)
{
    if (info == NULL || data == NULL || length < 4)
    {
        return;
    }
    ecos_bus_recv_t recv = NULL;
    int slot = -1;
    portENTER_CRITICAL(&s_table);
    for (int i = 0; i < BUS_SERVICES; i++)
    {
        bool match = s_services[i].used;
        for (int b = 0; match && b < 4; b++)
        {
            match = ((data[b] ^ s_services[i].pattern[b]) & s_services[i].mask[b]) == 0;
        }
        if (match)
        {
            recv = s_services[i].recv;
            slot = i;
            s_services[i].active++;
            break;
        }
    }
    portEXIT_CRITICAL(&s_table);
    if (recv != NULL)
    {
        recv(info, data, length);
        portENTER_CRITICAL(&s_table);
        s_services[slot].active--;
        portEXIT_CRITICAL(&s_table);
    }
}

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    ecos_bus_sent_t sent = NULL;
    int slot = -1;
    portENTER_CRITICAL(&s_table);
    if (s_tx_pending && info != NULL && info->des_addr != NULL &&
        memcmp(info->des_addr, s_tx_mac, 6) == 0)
    {
        if (s_tx_callback_enabled && s_tx_owner >= 0 && s_tx_owner < BUS_SERVICES &&
            s_services[s_tx_owner].used && s_service_generation[s_tx_owner] == s_tx_generation)
        {
            slot = s_tx_owner;
            sent = s_services[slot].sent;
            if (sent != NULL)
            {
                s_services[slot].active++;
            }
        }
        s_tx_pending = false;
        s_tx_owner = -1;
    }
    portEXIT_CRITICAL(&s_table);
    if (sent != NULL)
    {
        sent(info, status);
        portENTER_CRITICAL(&s_table);
        s_services[slot].active--;
        portEXIT_CRITICAL(&s_table);
    }
}

/* Called with s_lock held and the radio up. */
static esp_err_t apply_power(void)
{
    int8_t lowest = 0;
    portENTER_CRITICAL(&s_table);
    for (int i = 0; i < BUS_SERVICES; i++)
    {
        if (s_services[i].used && s_services[i].tx_power > 0 && (lowest == 0 || s_services[i].tx_power < lowest))
        {
            lowest = s_services[i].tx_power;
        }
    }
    portEXIT_CRITICAL(&s_table);
    if (!s_power_lowered && lowest > 0)
    {
        esp_err_t err = esp_wifi_get_max_tx_power(&s_saved_power);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    int8_t target = lowest > 0 && lowest < s_saved_power ? lowest : s_saved_power;
    if (lowest > 0 || s_power_lowered)
    {
        esp_err_t err = esp_wifi_set_max_tx_power(target);
        if (err != ESP_OK)
        {
            return err;
        }
        s_power_lowered = target < s_saved_power;
    }
    return ESP_OK;
}

static bool take_lock(void)
{
    portENTER_CRITICAL(&s_table);
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    portEXIT_CRITICAL(&s_table);
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

esp_err_t ecos_bus_open(const char tag[4], ecos_bus_recv_t recv, ecos_bus_sent_t sent, int8_t tx_power)
{
    static const uint8_t full_mask[4] = {0xff, 0xff, 0xff, 0xff};
    if (tag == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return ecos_bus_open_mask(tag, (const uint8_t *)tag, full_mask, recv, sent, tx_power);
}

esp_err_t ecos_bus_open_mask(const char tag[4], const uint8_t pattern[4], const uint8_t mask[4],
                             ecos_bus_recv_t recv, ecos_bus_sent_t sent, int8_t tx_power)
{
    if (tag == NULL || pattern == NULL || mask == NULL || recv == NULL || !take_lock())
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    int slot = -1;
    portENTER_CRITICAL(&s_table);
    for (int i = 0; i < BUS_SERVICES; i++)
    {
        bool same_tag = s_services[i].used && memcmp(s_services[i].tag, tag, 4) == 0;
        bool overlap = s_services[i].used;
        for (int b = 0; overlap && b < 4; b++)
        {
            overlap = (((pattern[b] ^ s_services[i].pattern[b]) & mask[b] & s_services[i].mask[b]) == 0);
        }
        if (same_tag || overlap)
        {
            err = ESP_ERR_INVALID_STATE;   /* one owner per tag */
        }
        else if (!s_services[i].used && slot < 0)
        {
            slot = i;
        }
    }
    portEXIT_CRITICAL(&s_table);
    if (err == ESP_OK && slot < 0)
    {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK && s_open == 0)
    {
        if ((err = wifi_radio_acquire()) == ESP_OK)
        {
            if ((err = esp_now_init()) == ESP_OK)
            {
                err = esp_now_register_recv_cb(on_recv);
                if (err == ESP_OK)
                {
                    err = esp_now_register_send_cb(on_sent);
                }
                if (err != ESP_OK)
                {
                    esp_now_unregister_recv_cb();
                    esp_now_unregister_send_cb();
                    esp_now_deinit();
                    wifi_radio_release();
                }
            }
            else
            {
                wifi_radio_release();
            }
        }
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Radio on: %s", esp_err_to_name(err));
        }
    }
    if (err == ESP_OK)
    {
        portENTER_CRITICAL(&s_table);
        s_services[slot] = (bus_service_t){.recv = recv, .sent = sent, .tx_power = tx_power, .used = true};
        memcpy(s_services[slot].tag, tag, 4);
        memcpy(s_services[slot].pattern, pattern, 4);
        memcpy(s_services[slot].mask, mask, 4);
        s_service_generation[slot]++;
        portEXIT_CRITICAL(&s_table);
        if (s_open++ == 0)
        {
            ESP_LOGI(TAG, "Radio on for %.4s", tag);
        }
        apply_power();
    }
    xSemaphoreGive(s_lock);
    return err;
}

void ecos_bus_close(const char tag[4])
{
    if (tag == NULL || !take_lock())
    {
        return;
    }
    bool found = false;
    int slot = -1;
    portENTER_CRITICAL(&s_table);
    for (int i = 0; i < BUS_SERVICES; i++)
    {
        if (s_services[i].used && memcmp(s_services[i].tag, tag, 4) == 0)
        {
            s_services[i].used = false;
            found = true;
            slot = i;
        }
    }
    portEXIT_CRITICAL(&s_table);
    /* Removing the dispatch entry prevents new callbacks. Wait for any callback
     * that already copied it before the caller is allowed to free its queue. */
    if (found)
    {
        unsigned active;
        do
        {
            portENTER_CRITICAL(&s_table);
            active = s_services[slot].active;
            portEXIT_CRITICAL(&s_table);
            if (active != 0)
            {
                vTaskDelay(1);
            }
        } while (active != 0);
    }
    if (found && s_tx_pending && s_tx_owner == slot)
    {
        portENTER_CRITICAL(&s_table);
        s_tx_callback_enabled = false;
        portEXIT_CRITICAL(&s_table);
    }
    if (found && --s_open == 0)
    {
        if (s_power_lowered)
        {
            esp_wifi_set_max_tx_power(s_saved_power);
            s_power_lowered = false;
        }
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        esp_now_deinit();   /* drops every peer */
        portENTER_CRITICAL(&s_table);
        s_tx_pending = false;
        s_tx_callback_enabled = false;
        s_tx_owner = -1;
        portEXIT_CRITICAL(&s_table);
        wifi_radio_release();
        ESP_LOGI(TAG, "Radio off");
    }
    else if (found)
    {
        apply_power();
    }
    xSemaphoreGive(s_lock);
}

esp_err_t ecos_bus_power(const char tag[4], int8_t tx_power)
{
    if (tag == NULL || !take_lock())
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&s_table);
    for (int i = 0; i < BUS_SERVICES; i++)
    {
        if (s_services[i].used && memcmp(s_services[i].tag, tag, 4) == 0)
        {
            s_services[i].tx_power = tx_power;
            err = ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_table);
    if (err == ESP_OK)
    {
        err = apply_power();
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ecos_bus_peer(const uint8_t mac[6])
{
    if (mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_now_is_peer_exist(mac))
    {
        return ESP_OK;
    }
    esp_now_peer_info_t peer = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};
    memcpy(peer.peer_addr, mac, 6);
    esp_err_t err = esp_now_add_peer(&peer);
    return err == ESP_ERR_ESPNOW_EXIST ? ESP_OK : err;
}

esp_err_t ecos_bus_send(const uint8_t mac[6], const uint8_t *data, size_t length)
{
    if (mac == NULL || data == NULL || length < 4 || length > ESP_NOW_MAX_DATA_LEN_V2)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char tag[4];
    memcpy(tag, data, 4);
    return ecos_bus_send_tag(tag, mac, data, length);
}

esp_err_t ecos_bus_send_tag(const char tag[4], const uint8_t mac[6], const uint8_t *data, size_t length)
{
    if (tag == NULL || mac == NULL || data == NULL || length == 0 || length > ESP_NOW_MAX_DATA_LEN_V2)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock())
    {
        return ESP_ERR_INVALID_STATE;
    }
    bool broadcast = true;
    for (int i = 0; i < 6; i++)
    {
        if (mac[i] != 0xff)
        {
            broadcast = false;
            break;
        }
    }
    int slot = -1;
    uint32_t generation = 0;
    portENTER_CRITICAL(&s_table);
    for (int i = 0; i < BUS_SERVICES; i++)
    {
        if (s_services[i].used && memcmp(s_services[i].tag, tag, 4) == 0)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        portEXIT_CRITICAL(&s_table);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_tx_pending)
    {
        portEXIT_CRITICAL(&s_table);
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_tx_pending = true;
    s_tx_callback_enabled = !broadcast && s_services[slot].sent != NULL;
    s_tx_owner = slot;
    s_tx_generation = s_service_generation[slot];
    generation = s_tx_generation;
    memcpy(s_tx_mac, mac, 6);
    portEXIT_CRITICAL(&s_table);

    esp_err_t err = esp_now_send(mac, data, length);
    if (err != ESP_OK)
    {
        portENTER_CRITICAL(&s_table);
        if (s_tx_pending && s_tx_owner == slot && s_tx_generation == generation)
        {
            s_tx_pending = false;
            s_tx_owner = -1;
        }
        portEXIT_CRITICAL(&s_table);
    }
    xSemaphoreGive(s_lock);
    return err;
}
