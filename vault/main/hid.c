/* Credential typing never queues a secret. One worker sends press/release,
 * checks the same connection generation and aborts immediately on lock. */
#include "vault.h"
#include "hid_private.h"
#include "hardware/board.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdatomic.h>

static atomic_bool s_caps;
static atomic_uint s_led_version;

void vault_hid_caps(bool enabled)
{
    if (atomic_exchange(&s_caps, enabled) != enabled)
        atomic_fetch_add(&s_led_version, 1);
}

/* Overrides the weak hook in hid_link/hid_usb.c (USB keyboard LED report). */
void hid_usb_keyboard_leds(uint8_t leds)
{
    vault_hid_caps((leds & 2) != 0);
}

esp_err_t vault_hid_init(void)
{
    uint8_t transport = HID_LINK_BLE, layout = HID_HOST_IT;
    nvs_handle_t handle;
    if (nvs_open("vault_ui", NVS_READONLY, &handle) == ESP_OK)
    {
        nvs_get_u8(handle, "transport", &transport);
        nvs_get_u8(handle, "layout", &layout);
        nvs_close(handle);
    }
    if (transport != HID_LINK_USB || !board_info()->has_usb_otg)
        transport = HID_LINK_BLE;
    hid_link_set_host_layout(layout == HID_HOST_US ? HID_HOST_US : HID_HOST_IT);
    return hid_link_start(transport, "Watch Vault");
}

static esp_err_t send(const uint8_t data[8])
{
    return hid_link_transport() == HID_LINK_USB ? hid_usb_send(HID_REPORT_KEYBOARD, data, 8)
                                                : hid_ble_send(HID_REPORT_KEYBOARD, data, 8);
}

esp_err_t vault_hid_type(const char *text)
{
    if (text == NULL || !vault_unlocked() || !vault_hid_authenticated())
        return ESP_ERR_INVALID_STATE;
    uint8_t modifiers, keycode;
    /* The shared map simplifies some Unicode. Passwords must never be altered:
     * reject the entire field before sending, including all control characters. */
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 32 || *p > 126 ||
            !hid_keymap_lookup(hid_link_host_layout(), *p, &modifiers, &keycode))
            return ESP_ERR_NOT_SUPPORTED;
    uint32_t generation = hid_link_version();
    unsigned leds = atomic_load(&s_led_version);
    bool caps = atomic_load(&s_caps);
    uint8_t neutral[8] = {0};
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    {
        if (!vault_unlocked() || !vault_hid_authenticated() || generation != hid_link_version() ||
            leds != atomic_load(&s_led_version))
        {
            send(neutral);
            return ESP_ERR_INVALID_STATE;
        }
        hid_keymap_lookup(hid_link_host_layout(), *p, &modifiers, &keycode);
        if (caps && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
            modifiers ^= HIDL_MOD_SHIFT;
        uint8_t report[8] = {modifiers, 0, keycode};
        esp_err_t result = ESP_ERR_TIMEOUT;
        for (unsigned attempt = 0; attempt < 20; attempt++)
        {
            if (!vault_unlocked() || generation != hid_link_version())
                break;
            result = send(report);
            if (result == ESP_OK)
                break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vault_wipe(report, sizeof(report));
        if (result != ESP_OK)
        {
            send(neutral);
            return result;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        result = ESP_ERR_TIMEOUT;
        for (unsigned attempt = 0; attempt < 20; attempt++)
        {
            result = send(neutral);
            if (result == ESP_OK)
                break;
            if (generation != hid_link_version())
                break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (result != ESP_OK)
            return result;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* No Tab, Enter, clipboard or automatic form submission is ever appended. */
    return ESP_OK;
}
