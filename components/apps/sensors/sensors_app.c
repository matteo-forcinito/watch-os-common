#include "apps_internal.h"
#include "services/motion.h"
#include "core/power.h"
#include "ui/ui.h"
#include <stdlib.h>

/* A retired screen can outlive the next one during a transition. */
typedef struct
{
    lv_timer_t *tick;
    lv_obj_t *acceleration;
    lv_obj_t *rotation;
    lv_obj_t *status;
    bool visible;
} sensors_ui_t;
static sensors_ui_t *s_ui;

static const char *error_text(esp_err_t error)
{
    if (error == ESP_ERR_NO_MEM)
    {
        return "Memoria insufficiente";
    }
    if (error == ESP_ERR_INVALID_STATE)
    {
        return "Sensore occupato";
    }
    return "Sensore non disponibile: riprova";
}

static void refresh(lv_timer_t *timer)
{
    sensors_ui_t *ui = lv_timer_get_user_data(timer);
    if (ui != s_ui || !ui->visible)
    {
        return;
    }
    if (app_current() != &sensors_app)
    {
        ui->visible = false;
        motion_stop();
        return;
    }
    if (power_screen() >= POWER_SCREEN_AOD)
    {
        return;
    }
    motion_snapshot_t state;
    motion_snapshot(&state);
    /* A quick return may arrive while the previous worker is cleaning up. */
    if (!state.running && state.error == ESP_OK)
    {
        motion_start();
        motion_snapshot(&state);
    }
    if (!state.valid)
    {
        lv_label_set_text(ui->acceleration, "X  —   Y  —   Z  —");
        lv_label_set_text(ui->rotation, "X  —   Y  —   Z  —");
        lv_label_set_text(ui->status, state.error != ESP_OK ? error_text(state.error) :
                          (state.running ? "Avvio sensore…" : "Sensore in pausa"));
        return;
    }
    const imu_sample_t *p = &state.sample;
    lv_label_set_text_fmt(ui->acceleration, "X  %+.2f\nY  %+.2f\nZ  %+.2f", (double)p->acceleration_g[0],
                         (double)p->acceleration_g[1], (double)p->acceleration_g[2]);
    lv_label_set_text_fmt(ui->rotation, "X  %+.1f\nY  %+.1f\nZ  %+.1f", (double)p->angular_dps[0],
                         (double)p->angular_dps[1], (double)p->angular_dps[2]);
    lv_label_set_text_fmt(ui->status, "Chip %.1f °C · %lu letture", (double)p->temperature_c,
                         (unsigned long)state.samples);
}
static void resume(void)
{
    if (s_ui != NULL)
    {
        s_ui->visible = true;
        motion_start();
    }
}
static void pause(void)
{
    if (s_ui != NULL)
    {
        s_ui->visible = false;
    }
    motion_stop();
}
static void deleted(lv_event_t *e)
{
    sensors_ui_t *ui = lv_event_get_user_data(e);
    if (ui == s_ui)
    {
        pause();
        s_ui = NULL;
    }
    if (ui->tick != NULL)
    {
        lv_timer_delete(ui->tick);
    }
    free(ui);
}
static void retry(lv_event_t *e)
{
    (void)e;
    esp_err_t err = motion_start();
    if (err != ESP_OK)
    {
        ui_toast(err == ESP_ERR_INVALID_STATE ? "Sensore già attivo: attendi" : "Sensore non disponibile");
    }
}
static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    pause();
    s_ui = NULL;
    lv_obj_t *page = ui_page(screen, "Sensori");
    sensors_ui_t *ui = calloc(1, sizeof(*ui));
    if (ui == NULL)
    {
        ui_text(page, "Memoria insufficiente", true);
        return;
    }
    s_ui = ui;
    lv_obj_add_event_cb(screen, deleted, LV_EVENT_DELETE, ui);
    ui_section(page, "ACCELERAZIONE · g");
    ui->acceleration = ui_text(page, "Avvio…", false);
    ui_section(page, "ROTAZIONE · °/s");
    ui->rotation = ui_text(page, "Avvio…", false);
    ui->status = ui_text(page, "", true);
    ui_text(page, "Assi del sensore. Temperatura del chip, non dell'ambiente.\nIl sensore si ferma uscendo dall'app.", true);
    ui_button(page, "Riprova", UI_COLOR_BLUE, retry, NULL);
    ui->tick = lv_timer_create(refresh, 200, ui);
    if (ui->tick == NULL)
    {
        lv_label_set_text(ui->status, "Memoria insufficiente");
        return;
    }
    resume();
}
const app_t sensors_app = {
    .id = "sensors", .name = "Sensori", .icon = LV_SYMBOL_GPS,
    .color = UI_COLOR_BLUE, .create = create,
    .pause = pause, .resume = resume,
};
