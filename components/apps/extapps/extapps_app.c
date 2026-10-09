#include "apps_internal.h"

#include "core/lv_port.h"
#include "core/sys.h"
#include "services/extapp.h"
#include "ui/ui.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * Start an external app (opened from the launcher's "Scheda SD" section,
 * arg = extapp_t * allocated by the caller, freed here).
 *
 *   already in the app slot  -> restarts into it at once
 *   new or changed           -> "Installa e apri": copies it with a progress
 *                               bar, then restarts into it
 *
 * The app comes back to the launcher with BOOT (see components/extapp_sdk;
 * old Arduino apps do it themselves).
 */

typedef struct
{
    extapp_t app;
    lv_obj_t *bar;
    lv_obj_t *status;
    lv_obj_t *button;
    bool installing;
} run_t;

typedef struct
{
    extapp_t app;
    int generation;
} job_t;

static run_t *s_run = NULL;
static volatile int s_percent = 0;
static volatile int s_generation = 0;   /* changes when the screen closes */

static void progress_update(void *arg)
{
    if ((int)(intptr_t)arg != s_generation || s_run == NULL)
    {
        return;
    }

    lv_obj_set_hidden(s_run->bar, false);
    lv_bar_set_value(s_run->bar, s_percent, LV_ANIM_ON);
    lv_label_set_text_fmt(s_run->status, s_percent >= 100 ? "Avvio..." : "Installazione %d%%", s_percent);
}

static void progress(int percent, void *ctx)
{
    if (percent != s_percent)
    {
        s_percent = percent;
        ui_async(progress_update, ctx);
    }
}

static void launch_failed(void *arg)
{
    if ((int)(intptr_t)arg != s_generation || s_run == NULL)
    {
        return;
    }

    s_run->installing = false;
    lv_label_set_text(s_run->status, extapp_last_error());
    lv_obj_set_style_text_color(s_run->status, lv_color_hex(UI_COLOR_RED), 0);
    lv_obj_set_hidden(s_run->button, false);
}

static void launch_task(void *arg)
{
    job_t *job = arg;
    void *generation = (void *)(intptr_t)job->generation;

    extapp_launch(&job->app, progress, generation);   /* restarts on success */

    free(job);
    ui_async(launch_failed, generation);
    vTaskDelete(NULL);
}

static void start(void)
{
    if (s_run == NULL || s_run->installing)
    {
        return;
    }

    job_t *job = malloc(sizeof(job_t));

    if (job == NULL)
    {
        return;
    }

    job->app = s_run->app;
    job->generation = s_generation;
    s_run->installing = true;
    s_percent = 0;

    lv_obj_set_hidden(s_run->button, true);
    lv_obj_set_style_text_color(s_run->status, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_label_set_text(s_run->status, extapp_is_cached(&s_run->app) ? "Avvio..." : "Preparazione...");

    if (!sys_task_create(launch_task, "extapp", 6144, job, 4, 0))
    {
        free(job);
        s_run->installing = false;
        lv_label_set_text(s_run->status, "Memoria insufficiente");
        lv_obj_set_hidden(s_run->button, false);
    }
}

static void on_start(lv_event_t *e)
{
    (void)e;
    start();
}

static void run_create(lv_obj_t *screen, void *arg)
{
    extapp_t *app = arg;

    s_run = calloc(1, sizeof(run_t));

    if (s_run == NULL || app == NULL)
    {
        free(app);
        return;
    }

    s_run->app = *app;
    free(app);

    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(screen, 24, 0);
    lv_obj_set_style_pad_row(screen, 14, 0);

    ui_icon_bubble(screen, s_run->app.icon[0] != '\0' ? s_run->app.icon : LV_SYMBOL_DIRECTORY, UI_COLOR_PURPLE, 120);

    lv_obj_t *name = lv_label_create(screen);
    lv_label_set_text(name, s_run->app.name);
    lv_obj_set_style_text_font(name, ui_font_headline, 0);

    bool cached = extapp_is_cached(&s_run->app);
    char info[64];
    snprintf(info, sizeof(info), "%u KB  ·  %s", (unsigned)(s_run->app.size / 1024), cached ? "pronta" : "da installare");
    lv_obj_t *size = lv_label_create(screen);
    lv_label_set_text(size, info);
    lv_obj_set_style_text_color(size, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    s_run->bar = lv_bar_create(screen);
    lv_obj_set_size(s_run->bar, LV_PCT(90), 14);
    lv_bar_set_range(s_run->bar, 0, 100);
    lv_obj_set_style_bg_color(s_run->bar, lv_color_hex(UI_COLOR_CARD_HI), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_run->bar, lv_color_hex(UI_COLOR_PURPLE), LV_PART_INDICATOR);
    lv_obj_set_hidden(s_run->bar, true);

    s_run->status = lv_label_create(screen);
    lv_label_set_text(s_run->status, cached ? "" : "Verrà copiata nella memoria dell'orologio.");
    lv_label_set_long_mode(s_run->status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_run->status, LV_PCT(100));
    lv_obj_set_style_text_align(s_run->status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_run->status, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    s_run->button = ui_button(screen, cached ? "Apri" : "Installa e apri", UI_COLOR_PURPLE, on_start, NULL);
    lv_obj_t *hint = ui_text(screen, "Nell'app premi BOOT per tornare qui.", true);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    if (cached)
    {
        start();   /* nothing to copy: open right away */
    }
}

static void run_destroy(void)
{
    s_generation++;
    free(s_run);
    s_run = NULL;
}

static bool run_back(void)
{
    return s_run != NULL && s_run->installing;   /* do not leave while flashing */
}

const app_t extapps_run_app = {
    .id = "extapps.run",
    .name = "App esterna",
    .icon = LV_SYMBOL_DIRECTORY,
    .color = UI_COLOR_PURPLE,
    .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON,
    .create = run_create,
    .destroy = run_destroy,
    .back = run_back,
};
