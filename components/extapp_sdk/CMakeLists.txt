# extapp_sdk: two calls an external app needs to live next to the launcher.
idf_component_register(
    SRCS "extapp_sdk.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_update esp_system
)
