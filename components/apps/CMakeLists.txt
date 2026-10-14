# apps: the watch shell (home screen) and the built-in apps.
# Apps that also exist as external apps (Pieno...) keep their code in
# watch-os-common/<app>/main and are compiled in here when the OS hosts them.
set(INTERNAL_SRCS "")
set(INTERNAL_INCLUDES "")
set(INTERNAL_PAGES "")
if(CONFIG_APPKIT_HOSTED)
    set(PIENO "${CMAKE_CURRENT_LIST_DIR}/../../pieno/main")
    list(APPEND INTERNAL_SRCS "${PIENO}/app.c" "${PIENO}/feed.c" "${PIENO}/fuel.c" "${PIENO}/ui.c" "${PIENO}/web.c")
    list(APPEND INTERNAL_INCLUDES "${PIENO}")
    list(APPEND INTERNAL_PAGES "${PIENO}/pieno_dashboard.html")
    set(GIORNATA "${CMAKE_CURRENT_LIST_DIR}/../../giornata/main")
    list(APPEND INTERNAL_SRCS "${GIORNATA}/app.c" "${GIORNATA}/ics.c" "${GIORNATA}/store.c" "${GIORNATA}/sync.c"
         "${GIORNATA}/agenda.c" "${GIORNATA}/ui.c" "${GIORNATA}/web.c")
    list(APPEND INTERNAL_INCLUDES "${GIORNATA}")
    list(APPEND INTERNAL_PAGES "${GIORNATA}/giornata_dashboard.html" "${GIORNATA}/gts_root_r1.pem")
    set(COPERTURA "${CMAKE_CURRENT_LIST_DIR}/../../copertura/main")
    list(APPEND INTERNAL_SRCS "${COPERTURA}/app.c" "${COPERTURA}/advice.c" "${COPERTURA}/probe.c" "${COPERTURA}/store.c"
         "${COPERTURA}/sound.c" "${COPERTURA}/ui.c" "${COPERTURA}/web.c")
    list(APPEND INTERNAL_INCLUDES "${COPERTURA}")
    list(APPEND INTERNAL_PAGES "${COPERTURA}/copertura_dashboard.html")
    set(NEWS "${CMAKE_CURRENT_LIST_DIR}/../../news/main")
    list(APPEND INTERNAL_SRCS "${NEWS}/app.c" "${NEWS}/text.c" "${NEWS}/rss.c" "${NEWS}/store.c" "${NEWS}/fetch.c"
         "${NEWS}/ui.c" "${NEWS}/web.c")
    list(APPEND INTERNAL_INCLUDES "${NEWS}")
    list(APPEND INTERNAL_PAGES "${NEWS}/news_dashboard.html")
    set(SENTINEL "${CMAKE_CURRENT_LIST_DIR}/../../sentinel")
    list(APPEND INTERNAL_SRCS "${SENTINEL}/lite/app.c" "${SENTINEL}/lite/engine.c" "${SENTINEL}/lite/ui.c"
         "${SENTINEL}/lite/web.c" "${SENTINEL}/main/packet.c" "${SENTINEL}/main/oui_lite.c")
    list(APPEND INTERNAL_INCLUDES "${SENTINEL}/lite" "${SENTINEL}/main")
    list(APPEND INTERNAL_PAGES "${SENTINEL}/lite/sentinel_dashboard.html")
    set(OMBRA "${CMAKE_CURRENT_LIST_DIR}/../../ombra/main")
    list(APPEND INTERNAL_SRCS "${OMBRA}/app.c" "${OMBRA}/scanner.c" "${OMBRA}/classify.c" "${OMBRA}/sound.c" "${OMBRA}/ui.c")
    list(APPEND INTERNAL_INCLUDES "${OMBRA}")
endif()
idf_component_register(
    SRCS
        "apps.c"
        "shell/shell.c"
        "shell/watchface.c"
        "shell/launcher.c"
        "shell/control_center.c"
        "shell/notifications.c"
        "shell/now_playing.c"
        "alarms/alarms_app.c"
        "alarms/ring_app.c"
        "weather/weather_app.c"
        "settings/settings_app.c"
        "misc/misc_apps.c"
        "web/web_app.c"
        "extapps/extapps_app.c"
        "market/market_app.c"
        "timer/timer_app.c"
        "voice/voice_memo_app.c"
        "sensors/sensors_app.c"
        "usb/usb_drive_app.c"
        "sdcard/sdcard_app.c"
        "battery/battery_app.c"
        "update/update_app.c"
        ${INTERNAL_SRCS}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
        ${INTERNAL_INCLUDES}
    REQUIRES
        core
    PRIV_REQUIRES
        ui
        services
        companion
        hardware
        esp_pm
        esp_tinyusb
        esp_timer
        app_update
        esp_app_format
        # internal apps (requirements cannot depend on Kconfig: always listed)
        appkit
        esp_http_client
        esp_http_server
        mbedtls
        cjson
        nvs_flash
        esp_wifi
        esp_netif
        lwip
        bt
    EMBED_TXTFILES
        ${INTERNAL_PAGES}
)
