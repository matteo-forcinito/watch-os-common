set(COMMON "${WATCH_OS_COMMON}/components")
set(GIORNATA "${WATCH_OS_COMMON}/giornata/main")
idf_component_register(
    SRCS "main.c" "runtime.c" "draw.c" "web.c" "calendar.c"
        "${COMMON}/core/settings.c" "${COMMON}/core/clock.c"
        "${COMMON}/services/alarm/alarm.c" "${COMMON}/services/storage/storage.c"
        "${COMMON}/apps/life/life_model.c" "${COMMON}/apps/life/life_store.c"
        "${GIORNATA}/store.c" "${GIORNATA}/agenda.c" "${GIORNATA}/ics.c"
    INCLUDE_DIRS "." "${COMMON}/core/include" "${COMMON}/services/include"
        "${COMMON}/apps/life" "${GIORNATA}"
    REQUIRES hardware extapp_sdk esp_timer esp_http_server esp_wifi esp_netif nvs_flash
        esp_event esp_pm esp_system esp_hw_support cjson fatfs wear_levelling esp_partition app_update
    EMBED_TXTFILES "dashboard.html" "${COMMON}/services/web/kit.js" "${COMMON}/services/web/kit.css"
        "${COMMON}/apps/life/raccolta_dashboard.html" "${COMMON}/apps/life/zampe_dashboard.html")
target_compile_definitions(${COMPONENT_LIB} PUBLIC WATCH_LITE=1 CONFIG_APPKIT_HOSTED=1)
if("$ENV{LITE_QA}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LITE_QA=1)
endif()
