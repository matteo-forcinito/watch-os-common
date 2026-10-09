# companion: phone link over Bluetooth LE (Gadgetbridge / Bangle.js protocol).
idf_component_register(
    SRCS
        "ble_companion.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        core
    PRIV_REQUIRES
        services
        hardware
        bt
        esp_timer
        cjson
)
