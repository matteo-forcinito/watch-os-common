# appkit: alarms + web dashboard + dashboard page for the external apps (see include/appkit.h).
# Not part of the base OS (excluded in each firmware's root CMakeLists).
idf_component_register(
    SRCS "appkit_alarms.c" "appkit_web.c" "appkit_ui.c" "../apps/alarms/ring_app.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../apps"
    REQUIRES core ui services esp_http_server cjson
    PRIV_REQUIRES hardware esp_timer esp_wifi esp_netif nvs_flash mdns
    EMBED_TXTFILES "kit.js" "kit.css"
)
