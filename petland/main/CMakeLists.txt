set(MG "${WATCH_OS_COMMON}/components/apps/minigames")
idf_component_register(
    SRCS "${WATCH_OS_COMMON}/petland/main/main.c" "${WATCH_OS_COMMON}/petland/main/world.c"
         "${MG}/minigame.c" "${MG}/minigame_canvas.c" "${MG}/minigame_progress.c" "${MG}/minigame_screen.c"
         "${WATCH_OS_COMMON}/components/appkit/appkit_alarms.c"
         "${WATCH_OS_COMMON}/components/apps/alarms/ring_app.c"
         "${WATCH_OS_COMMON}/components/services/alarm/alarm.c"
         "${WATCH_OS_COMMON}/components/services/sound/sound.c"
         "${WATCH_OS_COMMON}/components/services/storage/storage.c"
    INCLUDE_DIRS "${WATCH_OS_COMMON}/petland/main"
    PRIV_INCLUDE_DIRS "${WATCH_OS_COMMON}/components/appkit/include"
                      "${WATCH_OS_COMMON}/components/services/include" "${WATCH_OS_COMMON}/components/apps"
    PRIV_REQUIRES hardware core ui orbi extapp_sdk esp_timer nvs_flash cjson fatfs wear_levelling
)
