#include "apps_internal.h"

#include "core/power.h"
#include "hardware/sdcard.h"
#include "ui/ui.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#include <stdio.h>

/*
 * USB drive: the microSD card appears on the PC as a USB stick (TinyUSB
 * mass storage), to copy apps, icons, radios.txt, wifi.txt...
 *
 * The ESP32-S3 has one USB port: while the drive runs it is no longer the
 * serial console, and the watch cannot use the card. So the only way out is a
 * restart ("Fine" button, BOOT or back): eject the drive on the PC first.
 */

static const char *TAG = "usb_drive";

static lv_obj_t *s_status = NULL;
static lv_obj_t *s_icon = NULL;
static volatile bool s_attached = false;
static bool s_started = false;
static bool s_touched = false;   /* the card was taken from the file system */
static lv_timer_t *s_timer = NULL;
static esp_pm_lock_handle_t s_pm_lock = NULL;
static esp_pm_lock_handle_t s_cpu_lock = NULL;
static tinyusb_msc_storage_handle_t s_storage = NULL;

/* TinyUSB task: only a flag here, the screen is updated by the timer. */
static void usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;

    if (event->id == TINYUSB_EVENT_ATTACHED)
    {
        s_attached = true;
    }
    else if (event->id == TINYUSB_EVENT_DETACHED)
    {
        s_attached = false;
    }
}

static const char *start_drive(void)
{
    sdmmc_card_t *card = NULL;

    if (sdcard_open_raw(&card) != ESP_OK)
    {
        return "Scheda SD non trovata";
    }

    /* The storage must exist before the USB stack starts, so the PC sees it at once. */
    tinyusb_msc_driver_config_t msc_config = {
        .user_flags.auto_mount_off = 1,   /* never mount the files ourselves */
    };

    if (tinyusb_msc_install_driver(&msc_config) != ESP_OK)
    {
        return "Errore USB (MSC)";
    }

    tinyusb_msc_storage_config_t storage_config = {
        .medium.card = card,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {.do_not_format = true},
    };

    if (tinyusb_msc_new_storage_sdmmc(&storage_config, &s_storage) != ESP_OK)
    {
        return "Errore USB (scheda)";
    }

    tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG(usb_event);

    if (tinyusb_driver_install(&usb_config) != ESP_OK)
    {
        return "Errore USB";
    }

    s_started = true;
    return NULL;
}

static void tick(lv_timer_t *timer)
{
    (void)timer;

    if (!s_started)
    {
        return;
    }

    lv_label_set_text(s_status, s_attached ? "Collegato al PC.\nEspelli l'unità prima di uscire."
                                           : "Collega il cavo USB al PC.");
    lv_obj_set_style_bg_color(s_icon, lv_color_hex(s_attached ? UI_COLOR_GREEN : UI_COLOR_BLUE), 0);
}

static void on_finish(lv_event_t *e)
{
    (void)e;
    power_restart();
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(screen, 24, 0);
    lv_obj_set_style_pad_row(screen, 16, 0);

    s_icon = ui_icon_bubble(screen, LV_SYMBOL_USB, UI_COLOR_BLUE, 110);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Unità USB");
    lv_obj_set_style_text_font(title, ui_font_title, 0);

    s_status = lv_label_create(screen);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_status, LV_PCT(100));
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    /*
     * The USB stack must keep running: no light sleep while the drive is on,
     * and full CPU speed so the PC is never kept waiting (a slow drive hangs
     * Windows Explorer). The cable powers the watch meanwhile.
     */
    if (s_pm_lock == NULL)
    {
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb_drive", &s_pm_lock);
    }

    if (s_cpu_lock == NULL)
    {
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "usb_drive_cpu", &s_cpu_lock);
    }

    if (s_pm_lock != NULL)
    {
        esp_pm_lock_acquire(s_pm_lock);
    }

    if (s_cpu_lock != NULL)
    {
        esp_pm_lock_acquire(s_cpu_lock);
    }

    s_touched = true;
    const char *error = start_drive();

    if (error != NULL)
    {
        ESP_LOGE(TAG, "%s", error);
        lv_label_set_text(s_status, error);
        lv_obj_set_style_text_color(s_status, lv_color_hex(UI_COLOR_RED), 0);
    }

    ui_button(screen, "Fine (riavvia)", UI_COLOR_BLUE, on_finish, NULL);

    s_timer = lv_timer_create(tick, 500, NULL);
    tick(s_timer);
}

/* Leaving any way restarts: the card and the USB port must be given back cleanly. */
static bool back(void)
{
    if (s_touched)
    {
        power_restart();
    }

    return false;
}

static void destroy(void)
{
    if (s_timer != NULL)
    {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    if (s_pm_lock != NULL && !s_started)
    {
        esp_pm_lock_release(s_pm_lock);
    }

    if (s_cpu_lock != NULL && !s_started)
    {
        esp_pm_lock_release(s_cpu_lock);
    }
}

const app_t usb_drive_app = {
    .id = "usb",
    .name = "Unità USB",
    .icon = LV_SYMBOL_USB,
    .color = UI_COLOR_BLUE,
    .flags = APP_FLAG_KEEP_SCREEN_ON | APP_FLAG_STAY_ON_WAKE | APP_FLAG_NO_BACK_GESTURE,
    .create = create,
    .destroy = destroy,
    .back = back,
};
