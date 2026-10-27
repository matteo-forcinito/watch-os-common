"""Adapt a muse-gadget-sdk checkout to run as a Watch OS external app.

Usage: python apply_watch_os.py <muse-gadget-sdk>/esp32

Muse then lives in ota_1 of our partition table and shares our NVS:
- it never erases NVS (the Watch OS settings live there too);
- Settings gets "Watch OS", which boots ota_0 (our launcher) again.
Every replacement must match exactly once, so an upstream change stops the
script instead of producing a silently different firmware.
"""
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve()

EDITS = [
    ("components/muse/muse_settings.c",
     """    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }""",
     """#if CONFIG_MUSE_WATCH_OS_SLOT
    /* The NVS partition belongs to Watch OS as well: never erase it. */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS needs recovery; not erasing the shared partition: %s", esp_err_to_name(err));
    }
#else
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
#endif"""),
    ("components/muse/muse_settings_ui.c",
     '#include "esp_timer.h"\n',
     '#include "esp_timer.h"\n#if CONFIG_MUSE_WATCH_OS_SLOT\n#include "esp_ota_ops.h"\n#include "esp_system.h"\n#endif\n'),
    ("components/muse/muse_settings_ui.c",
     "static void build_home(lv_obj_t *tile)\n{",
     """#if CONFIG_MUSE_WATCH_OS_SLOT
/* Back to the Watch OS launcher in ota_0; Muse stays installed in ota_1. */
static void on_watch_os(lv_event_t *e)
{
    (void)e;
    const esp_partition_t *os = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (os && esp_ota_set_boot_partition(os) == ESP_OK) {
        esp_restart();
    }
}
#endif

static void build_home(lv_obj_t *tile)
{"""),
    ("components/muse/muse_settings_ui.c",
     '    s_home = page(tile, "SETTINGS", false, &list);\n',
     '    s_home = page(tile, "SETTINGS", false, &list);\n#if CONFIG_MUSE_WATCH_OS_SLOT\n    row(list, LV_SYMBOL_HOME, "Watch OS", NULL, on_watch_os, NULL);\n#endif\n'),
    ("components/muse/CMakeLists.txt",
     "    esp_driver_pcnt efuse\n",
     "    esp_driver_pcnt efuse app_update\n"),
    ("components/muse/Kconfig",
     'menu "Muse"\n',
     '''menu "Muse"

    config MUSE_WATCH_OS_SLOT
        bool "Run as a Watch OS external app"
        default n
        help
            Muse runs from ota_1 beside the Watch OS launcher in ota_0 and
            shares its NVS: never erase NVS, and offer a way back to Watch OS.
'''),
]


def main():
    for rel, old, new in EDITS:
        path = ROOT / rel
        text = path.read_text(encoding="utf-8")
        if new in text:
            continue
        if text.count(old) != 1:
            raise SystemExit("Upstream changed, stopping: " + rel)
        path.write_text(text.replace(old, new), encoding="utf-8", newline="")
    print("Watch OS adaptation applied to", ROOT)


if __name__ == "__main__":
    main()
