# hid_link: the watch as a Bluetooth / USB keyboard, mouse and media remote.
# Used by the external Remote Control app.
idf_component_register(
    SRCS
        "hid_link.c"
        "hid_keymap.c"
        "hid_ble.c"
        "hid_usb.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        bt
        esp_hid
        esp_tinyusb
        esp_event
)
