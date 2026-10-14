# appkit: alarms + web dashboard + dashboard page for apps (see include/appkit.h).
# kit.js / kit.css are embedded once by services (web/), shared with the system dashboard.
# External apps: own server (appkit_web.c) and the OS alarms (appkit_alarms.c + ring_app).
# OS firmwares (CONFIG_APPKIT_HOSTED): the internal apps' dashboards live on the OS server.
if(CONFIG_APPKIT_HOSTED)
    set(APPKIT_SRCS "appkit_http.c" "appkit_hosted.c" "appkit_ui.c")
else()
    set(APPKIT_SRCS "appkit_http.c" "appkit_web.c" "appkit_alarms.c" "appkit_ui.c" "../apps/alarms/ring_app.c")
endif()
idf_component_register(
    SRCS ${APPKIT_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../apps"
    REQUIRES core ui services esp_http_server cjson
    PRIV_REQUIRES hardware esp_timer esp_wifi esp_netif nvs_flash mdns
)
