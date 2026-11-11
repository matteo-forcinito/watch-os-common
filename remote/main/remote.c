#include "remote.h"
#include "core/settings.h"
#include "hid_link/hid_hub.h"

#include "core/state.h"
#include "esp_log.h"
#include "hardware/board.h"
#include "nvs.h"
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "remote";

#define NVS_NAMESPACE "remote"
#define NVS_KEY "settings"

/* Dictionaries embedded in the app (main/CMakeLists.txt EMBED_TXTFILES). */
extern const char dict_it_start[] asm("_binary_dict_it_txt_start");
extern const char dict_it_end[] asm("_binary_dict_it_txt_end");
extern const char dict_en_start[] asm("_binary_dict_en_txt_start");
extern const char dict_en_end[] asm("_binary_dict_en_txt_end");

static remote_settings_t s_settings = {
    .transport = HID_LINK_BLE,
    .host_layout = HID_HOST_IT,
    .dictionary = DICT_IT,
    .swipe = true,
    .auto_caps = true,
    .pointer_speed = 2,
};

static swipe_dict_t *s_dict[2] = {NULL, NULL};

const remote_settings_t *remote_settings(void)
{
    return &s_settings;
}

int8_t remote_wheel(int8_t steps)
{
    return s_settings.scroll_flags & 1 ? (int8_t)-steps : steps;
}

int8_t remote_pan(int8_t steps)
{
    return s_settings.scroll_flags & 2 ? (int8_t)-steps : steps;
}

void remote_settings_load(void)
{
    nvs_handle_t nvs;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK)
    {
        remote_settings_t saved = s_settings;
        size_t size = sizeof(saved);

        if (nvs_get_blob(nvs, NVS_KEY, &saved, &size) == ESP_OK && size >= 5 && size <= sizeof(saved))
        {
            s_settings = saved;
        }

        nvs_close(nvs);
    }

    if (s_settings.transport != HID_LINK_BLE && s_settings.transport != HID_LINK_USB)
    {
        s_settings.transport = HID_LINK_BLE;
    }

    if (!board_info()->has_usb_otg)
    {
        s_settings.transport = HID_LINK_BLE;
    }

    if (s_settings.dictionary > DICT_EN)
        s_settings.dictionary = DICT_IT;
    if (s_settings.host_layout > HID_HOST_US)
        s_settings.host_layout = HID_HOST_IT;
    if (s_settings.host_os > 1)
        s_settings.host_os = 0;
    if (s_settings.meeting > 1)
        s_settings.meeting = 0;
    if (s_settings.pointer_speed < 1 || s_settings.pointer_speed > 3)
        s_settings.pointer_speed = 2;
    s_settings.air_flags &= 7;
    s_settings.scroll_flags &= 3; /* older blobs end before it: 0, as before */

    hid_link_set_host_layout((hid_host_layout_t)s_settings.host_layout);
}

void remote_settings_save(const remote_settings_t *settings)
{
    s_settings = *settings;
    if (!board_info()->has_usb_otg)
    {
        s_settings.transport = HID_LINK_BLE;
    }

    hid_link_set_host_layout((hid_host_layout_t)s_settings.host_layout);

    nvs_handle_t nvs;

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_blob(nvs, NVS_KEY, &s_settings, sizeof(s_settings));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

const swipe_dict_t *remote_dictionary(void)
{
    remote_dict_t which = s_settings.dictionary == DICT_EN ? DICT_EN : DICT_IT;

    if (s_dict[which] == NULL)
    {
        const char *start = which == DICT_EN ? dict_en_start : dict_it_start;
        const char *end = which == DICT_EN ? dict_en_end : dict_it_end;
        size_t length = (size_t)(end - start);

        /* EMBED_TXTFILES adds a final NUL byte. */
        if (length > 0 && start[length - 1] == '\0')
        {
            length--;
        }

        s_dict[which] = swipe_dict_create(start, length);
        ESP_LOGI(TAG, "Dictionary %s: %d words", which == DICT_EN ? "EN" : "IT", swipe_dict_size(s_dict[which]));
    }

    return s_dict[which];
}

void remote_dictionary_select(remote_dict_t dict)
{
    remote_settings_t s = s_settings;
    s.dictionary = (uint8_t)dict;
    remote_settings_save(&s); /* loaded when the keyboard opens */
}

void remote_connect(void)
{
    /* A satellite talks only to its hub; a hub keeps the normal PC link and adds the radio. */
    hid_hub_role_t role = hid_hub_role();
    hid_link_transport_t transport =
        role == HID_HUB_SATELLITE ? HID_LINK_SATELLITE : (hid_link_transport_t)s_settings.transport;
    esp_err_t err = hid_link_start(transport, remote_device_name());

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Connection start: %s", esp_err_to_name(err));
    }
    if (role != HID_HUB_OFF && (err = hid_hub_start(remote_device_name())) != ESP_OK)
    {
        ESP_LOGW(TAG, "Hub radio: %s", esp_err_to_name(err));
    }
}

const char *remote_device_name(void)
{
    return settings_device_name();   /* one watch, one name: the same as the companion's */
}

bool remote_send_ok(bool accepted)
{
    if (!accepted)
    {
        ui_toast(hid_link_state() == HID_LINK_CONNECTED ? "Invio occupato: riprova" : "Associa Remote al computer");
    }
    return accepted;
}

const char *remote_status_text(void)
{
    hid_link_transport_t transport = hid_link_transport();
    hid_link_state_t state = hid_link_state();

    if (transport == HID_LINK_NONE || state == HID_LINK_OFF)
    {
        return "Non attivo";
    }

    if (transport == HID_LINK_SATELLITE)
    {
        return state == HID_LINK_CONNECTED ? "Satellite: collegato all'hub" : "Satellite: cerco l'hub...";
    }

    bool hub = hid_hub_role() == HID_HUB_HUB;
    if (transport == HID_LINK_BLE)
    {
        return state == HID_LINK_CONNECTED ? (hub ? "Hub Bluetooth: connesso" : "Bluetooth: connesso")
                                           : "Bluetooth: in attesa di associazione";
    }

    return state == HID_LINK_CONNECTED ? (hub ? "Hub USB: connesso" : "USB: connesso") : "USB: collega il cavo";
}

/* ---- battery ----
 *
 * The pill at the top centre is the core's (app_manager_battery_pill, on in
 * every external app). Remote hides it while the text keyboard of square
 * panels writes in the top strip, and sends the level to the computers
 * through the Bluetooth Battery Service. */

static bool battery_covered(void)
{
    return !board_info()->round && app_current() == &keyboard_app;
}

static void battery_tick(lv_timer_t *timer)
{
    (void)timer;
    static int sent = -2;
    int percent = (int)state_get(STATE_BATTERY);
    if (percent >= 0 && percent != sent)
    {
        sent = percent;
        hid_link_set_battery(percent);
    }
}

void remote_battery_start(void)
{
    app_manager_battery_pill(true);   /* every Remote layout keeps the top strip free */
    app_manager_battery_pill_filter(battery_covered);
    lv_timer_create(battery_tick, 2000, NULL);
}
