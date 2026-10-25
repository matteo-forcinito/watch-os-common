"""Create board-specific external-app wrappers, without changing the base OS."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[3]
boards = {
    "ESP32-S3-Amoled-Touch-1.8": "amoled",
    "ESP32-S3-Knob-Touch-LCD-1.8": "knob",
    "SenseCap-Watcher-W1-A": "watcher",
    "ESP32-S3-Touch-AMOLED-1.75": "amoled175",
    "ESP32-S3-Touch-AMOLED-1.75C": "amoled175c",
    "ESP32-4848S040C-I": "guition",
}
for folder, board in boards.items():
    base = root / folder
    app = base / "external_apps" / "solco"
    (app / "main").mkdir(parents=True, exist_ok=True)
    local = app / "local_components"
    local.mkdir(exist_ok=True)
    for component in ("lvgl__lvgl", "espressif__esp_lcd_sh8601", "espressif__cmake_utilities", "espressif__esp_codec_dev", "espressif__esp_lcd_spd2010", "espressif__esp_lcd_touch", "espressif__esp_lcd_touch_spd2010", "espressif__led_strip", "espressif__cjson", "espressif__esp_lcd_co5300"):
        target = base / "managed_components" / component
        link = local / component.split("__")[-1]
        if target.exists() and not link.exists():
            subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], check=True, capture_output=True)
    (app / ".gitignore").write_text("build/\nlocal_components/\nsdkconfig\nsdkconfig.old\nsolco-*.log\n", encoding="utf-8")
    (app / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.16)
if(DEFINED ENV{WATCH_OS_COMMON_DIR})
    set(WATCH_OS_COMMON "$ENV{WATCH_OS_COMMON_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../watch-os-common/solco/main")
    set(WATCH_OS_COMMON "${CMAKE_CURRENT_LIST_DIR}/../../watch-os-common")
else()
    set(WATCH_OS_COMMON "${CMAKE_CURRENT_LIST_DIR}/../../../watch-os-common")
endif()
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components" "${WATCH_OS_COMMON}/components")
if(DEFINED ENV{SOLCO_LOCAL_COMPONENTS})
    # Offline development: reuse the dependencies already installed for this board.
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/local_components")
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/local_components/cmake_utilities")
endif()
set(EXCLUDE_COMPONENTS "apps" "services" "companion" "webradio" "hid_link" "appkit" "watcher_camera")
set(COMPONENTS main cmake_utilities)
set(PROJECT_VER "0.1.0-dev.9")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(solco_''' + board + ''')
''', encoding="utf-8")
    (app / "main" / "CMakeLists.txt").write_text('''set(SOLCO_SHARED "${WATCH_OS_COMMON}/solco/main")
idf_component_register(
    SRCS "${SOLCO_SHARED}/main.c" "${SOLCO_SHARED}/install.c" "${SOLCO_SHARED}/engine.c" "${SOLCO_SHARED}/mix.c" "${SOLCO_SHARED}/vinyl.c" "${SOLCO_SHARED}/keylock.c" "${SOLCO_SHARED}/mp3.c" "${SOLCO_SHARED}/prepare.c" "${SOLCO_SHARED}/wav.c" "${SOLCO_SHARED}/studio.c" "${SOLCO_SHARED}/studio_motion.c" "${SOLCO_SHARED}/studio_clock.c"
    INCLUDE_DIRS "${SOLCO_SHARED}"
    PRIV_REQUIRES hardware core ui extapp_sdk esp_timer nvs_flash app_update bootloader_support mbedtls fatfs
    EMBED_FILES "${SOLCO_SHARED}/../assets/pulse.pcm" "${SOLCO_SHARED}/../assets/chord.pcm" "${SOLCO_SHARED}/../assets/import-test.mp3"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -O2)
# This override applies only to Solco's hardware component, never the base OS.
idf_component_get_property(SOLCO_HARDWARE_LIB hardware COMPONENT_LIB)
target_compile_definitions(${SOLCO_HARDWARE_LIB} PRIVATE WATCH_AUDIO_TX_DMA_FRAMES=128)
''' + f'\ntarget_compile_definitions(${{COMPONENT_LIB}} PRIVATE SOLCO_OUTPUT_CHANNELS={2 if board == "knob" else 1})\n', encoding="utf-8")
    if board in ("amoled", "amoled175", "amoled175c"):
        with (app / "main" / "CMakeLists.txt").open("a", encoding="utf-8") as file:
            file.write("\nidf_component_get_property(SOLCO_SENSOR_LIB board_drivers COMPONENT_LIB)\ntarget_compile_definitions(${SOLCO_SENSOR_LIB} PRIVATE WATCH_QMI8658_ODR=6 WATCH_QMI8658_RECOVER_SAME_PROFILE=1)\n")
    if board == "watcher":
        with (app / "main" / "CMakeLists.txt").open("a", encoding="utf-8") as file:
            file.write("\ntarget_compile_definitions(${COMPONENT_LIB} PRIVATE SOLCO_LOOP_EDGE_PAGES=16)\n")
        cmake = app / "main" / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("PRIV_REQUIRES hardware", "PRIV_REQUIRES esp_lcd_touch hardware"))
        with (app / "main" / "CMakeLists.txt").open("a", encoding="utf-8") as file:
            file.write("\nidf_component_get_property(SOLCO_TOUCH_LIB esp_lcd_touch_spd2010 COMPONENT_LIB)\ntarget_link_libraries(${SOLCO_TOUCH_LIB} PUBLIC idf::esp_driver_i2c idf::esp_lcd_touch)\n")
    # Use the existing board configuration, retaining its display and PM settings.
    defaults = (base / ("external_apps/webradio/sdkconfig.defaults" if board == "watcher" else "examples/hello_app/sdkconfig.defaults"))
    if not defaults.exists():
        defaults = base / "external_apps/motore/sdkconfig.defaults"
    if not defaults.exists():
        defaults = base / "sdkconfig.defaults"
    text = defaults.read_text(encoding="utf-8")
    text = "\n".join(line for line in text.splitlines()
                     if not line.startswith("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=")
                     and not line.startswith("CONFIG_WATCH_DEV_CONSOLE="))
    (app / "sdkconfig.defaults").write_text(text + '\nCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y\nCONFIG_WATCH_DEV_CONSOLE=y\n', encoding="utf-8")
    (app / "partitions.csv").write_bytes((base / "partitions.csv").read_bytes())
    (app / "manifest.json").write_text('{"name":"Solco","bin":"Solco.bin","version":"0.1.0-dev.9"}\n', encoding="utf-8")
    print(app)
