# hid_link: the watch as a Bluetooth / USB keyboard, mouse and media remote.
# Used by the external Remote Control app. hid_hub.c adds the Remote Hub radio
# (several watches as one set of HID devices), merged by remote/hub/hub_state.c.
idf_component_register(
    SRCS
        "hid_link.c"
        "hid_keymap.c"
        "hid_ble.c"
        "hid_usb.c"
        "hid_hub.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../remote/hub/hub_state.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_LIST_DIR}/../../remote/hub"
    PRIV_REQUIRES
        bt
        esp_hid
        esp_tinyusb
        esp_event
        esp_hw_support
        esp_wifi
        esp_netif
        esp_timer
        esp_pm
        nvs_flash
        mbedtls
)
