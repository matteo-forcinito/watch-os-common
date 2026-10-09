#include "apps_internal.h"

#include "core/lv_port.h"
#include "core/power.h"
#include "core/state.h"
#include "core/sys.h"
#include "hardware/board.h"
#include "hardware/display.h"
#include "hardware/pmu.h"
#include "services/battery_health.h"
#include "ui/ui.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Battery: a face that tells the state at a glance, then the numbers.
 *
 *   face shape   round on round panels, a rounded square on square ones
 *   colour/mouth the charge: green big smile ... red frown
 *   eyes         the HEALTH: normal, with glasses when the battery ages,
 *                tired with a sweat drop when it is worn out
 *   extras       a bolt while charging, closed happy eyes when full,
 *                a blink now and then
 *
 * "Test salute" measures the voltage drop under a fixed load (screen white
 * at full brightness vs black at minimum) - see services/battery_health.h.
 */

#define FACE_SIZE      150
#define REFRESH_MS     2000
#define BLINK_MS       4200
#define BLINK_SHUT_MS  140
#define EYE_W          20
#define EYE_H          28
#define TEST_SETTLE_MS 1500
#define TEST_SAMPLES   10
#define TEST_MIN_PCT   20

#define COLOR_DARK     0x1C1C1E
#define COLOR_DROP     0x64D2FF

typedef enum
{
    MOUTH_BIG_SMILE,
    MOUTH_SMILE,
    MOUTH_FLAT,
    MOUTH_FROWN,
} mouth_t;

typedef enum
{
    EYES_NORMAL,     /* healthy */
    EYES_GLASSES,    /* ageing */
    EYES_TIRED,      /* worn out */
} eyes_t;

typedef struct
{
    lv_obj_t *box;
    lv_obj_t *eye[2];
    lv_obj_t *happy_eye[2];   /* closed "^ ^" eyes when full */
    lv_obj_t *glasses;
    lv_obj_t *drop;
    lv_obj_t *smile;          /* arc: smile or frown */
    lv_obj_t *flat;           /* straight mouth */
    lv_obj_t *bolt;
    int key;                  /* what is drawn now, to redraw only on changes */
} face_t;

typedef struct
{
    face_t face;
    lv_obj_t *percent;
    lv_obj_t *status;
    lv_obj_t *health_row;
    lv_obj_t *cycles_row;
    lv_obj_t *age_row;
    lv_obj_t *resistance_row;
    lv_obj_t *time_row;
    lv_obj_t *test_overlay;
    lv_timer_t *refresh;
    lv_timer_t *blink;
    bool testing;
} battery_ui_t;

static battery_ui_t *s_ui = NULL;

/* ---------------------------------------------------------------- face */

static uint32_t charge_color(int percent)
{
    if (percent < 0)
    {
        return UI_COLOR_GRAY;
    }

    return percent >= 60 ? UI_COLOR_GREEN : percent >= 30 ? UI_COLOR_YELLOW : percent >= 15 ? UI_COLOR_ORANGE
                                                                                            : UI_COLOR_RED;
}

static lv_obj_t *dark_shape(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(COLOR_DARK), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_clickable(obj, false);
    return obj;
}

static lv_obj_t *dark_arc(lv_obj_t *parent, int32_t size, int32_t width)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(arc, false);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_DARK), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    return arc;
}

static void face_create(face_t *f, lv_obj_t *parent)
{
    const int32_t s = FACE_SIZE;
    const int32_t eye_y = s * 40 / 100;
    const int32_t eye_x[2] = {s * 34 / 100, s * 66 / 100};

    f->box = lv_obj_create(parent);
    lv_obj_remove_style_all(f->box);
    lv_obj_set_size(f->box, s, s);
    /* The face has the shape of the screen. */
    lv_obj_set_style_radius(f->box, board_info()->round ? LV_RADIUS_CIRCLE : s / 4, 0);
    lv_obj_set_style_bg_opa(f->box, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(f->box, false);
    lv_obj_set_clickable(f->box, false);
    lv_obj_set_overflow_visible(f->box, true);   /* the bolt sticks out */

    for (int i = 0; i < 2; i++)
    {
        f->eye[i] = dark_shape(f->box, EYE_W, EYE_H);
        lv_obj_set_pos(f->eye[i], eye_x[i] - EYE_W / 2, eye_y - EYE_H / 2);

        f->happy_eye[i] = dark_arc(f->box, 30, 6);
        lv_arc_set_bg_angles(f->happy_eye[i], 200, 340);   /* upper half: "^" */
        lv_arc_set_angles(f->happy_eye[i], 200, 340);
        lv_obj_set_pos(f->happy_eye[i], eye_x[i] - 15, eye_y - 8);
    }

    /* glasses: two rings and a bridge around the eyes */
    f->glasses = lv_obj_create(f->box);
    lv_obj_remove_style_all(f->glasses);
    lv_obj_set_size(f->glasses, s, s);
    lv_obj_set_clickable(f->glasses, false);

    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *ring = lv_obj_create(f->glasses);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, 44, 44);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 5, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(COLOR_DARK), 0);
        lv_obj_set_pos(ring, eye_x[i] - 22, eye_y - 22);
    }

    lv_obj_t *bridge = dark_shape(f->glasses, eye_x[1] - eye_x[0] - 44 + 6, 5);
    lv_obj_set_pos(bridge, eye_x[0] + 22 - 3, eye_y - 3);

    f->drop = lv_obj_create(f->box);
    lv_obj_remove_style_all(f->drop);
    lv_obj_set_size(f->drop, 16, 22);
    lv_obj_set_style_radius(f->drop, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(f->drop, lv_color_hex(COLOR_DROP), 0);
    lv_obj_set_style_bg_opa(f->drop, LV_OPA_COVER, 0);
    lv_obj_set_pos(f->drop, s * 80 / 100, s * 18 / 100);

    f->smile = dark_arc(f->box, 76, 8);
    f->flat = dark_shape(f->box, 46, 8);
    lv_obj_set_pos(f->flat, (s - 46) / 2, s * 70 / 100);

    f->bolt = lv_obj_create(f->box);
    lv_obj_remove_style_all(f->bolt);
    lv_obj_set_size(f->bolt, 46, 46);
    lv_obj_set_style_radius(f->bolt, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(f->bolt, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(f->bolt, LV_OPA_COVER, 0);
    lv_obj_set_pos(f->bolt, s - 38, -8);
    lv_obj_t *bolt = lv_label_create(f->bolt);
    lv_label_set_text(bolt, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(bolt, UI_FONT_ICON, 0);
    lv_obj_set_style_text_color(bolt, lv_color_hex(UI_COLOR_GREEN), 0);
    lv_obj_center(bolt);

    f->key = -1;
}

static void face_update(face_t *f, const battery_report_t *r)
{
    mouth_t mouth = r->percent >= 60 ? MOUTH_BIG_SMILE : r->percent >= 30 ? MOUTH_SMILE
                  : r->percent >= 15 ? MOUTH_FLAT : MOUTH_FROWN;
    eyes_t eyes = r->health < 0 || r->health >= 80 ? EYES_NORMAL : r->health >= 60 ? EYES_GLASSES : EYES_TIRED;
    bool charging = r->charging && !r->full;
    int key = (int)mouth | ((int)eyes << 2) | (charging << 4) | (r->full << 5) | ((int)charge_color(r->percent) << 6);

    if (key == f->key)
    {
        return;   /* nothing changed: no redraw */
    }

    f->key = key;
    lv_obj_set_style_bg_color(f->box, lv_color_hex(charge_color(r->percent)), 0);

    const int32_t s = FACE_SIZE;
    bool happy = r->full && eyes != EYES_TIRED;

    for (int i = 0; i < 2; i++)
    {
        lv_obj_set_hidden(f->eye[i], happy);
        lv_obj_set_hidden(f->happy_eye[i], !happy);
        lv_obj_set_height(f->eye[i], eyes == EYES_TIRED ? 8 : EYE_H);
        lv_obj_set_y(f->eye[i], s * 40 / 100 - (eyes == EYES_TIRED ? 0 : EYE_H / 2));
    }

    lv_obj_set_hidden(f->glasses, eyes != EYES_GLASSES);
    lv_obj_set_hidden(f->drop, eyes != EYES_TIRED);
    lv_obj_set_hidden(f->bolt, !charging);

    lv_obj_set_hidden(f->flat, mouth != MOUTH_FLAT);
    lv_obj_set_hidden(f->smile, mouth == MOUTH_FLAT);

    if (mouth == MOUTH_FROWN)
    {
        /* the top of a circle placed low: a frown */
        lv_arc_set_bg_angles(f->smile, 220, 320);
        lv_arc_set_angles(f->smile, 220, 320);
        lv_obj_set_pos(f->smile, (s - 76) / 2, s * 66 / 100);
    }
    else
    {
        uint16_t from = mouth == MOUTH_BIG_SMILE ? 25 : 55;
        lv_arc_set_bg_angles(f->smile, from, 180 - from);
        lv_arc_set_angles(f->smile, from, 180 - from);
        lv_obj_set_pos(f->smile, (s - 76) / 2, s * 34 / 100);
    }
}

/* Eyes shut for a moment: only two small objects redraw. */
static void blink_open(lv_timer_t *timer)
{
    (void)timer;

    if (s_ui != NULL)
    {
        for (int i = 0; i < 2; i++)
        {
            lv_obj_set_height(s_ui->face.eye[i], EYE_H);
            lv_obj_set_y(s_ui->face.eye[i], FACE_SIZE * 40 / 100 - EYE_H / 2);
        }
    }
}

static void blink(lv_timer_t *timer)
{
    (void)timer;

    if (s_ui == NULL || s_ui->testing || lv_obj_get_height(s_ui->face.eye[0]) != EYE_H ||
        lv_obj_is_hidden(s_ui->face.eye[0]))
    {
        return;   /* tired or closed eyes do not blink */
    }

    for (int i = 0; i < 2; i++)
    {
        lv_obj_set_height(s_ui->face.eye[i], 4);
        lv_obj_set_y(s_ui->face.eye[i], FACE_SIZE * 40 / 100 - 2);
    }

    lv_timer_t *open = lv_timer_create(blink_open, BLINK_SHUT_MS, NULL);
    lv_timer_set_repeat_count(open, 1);
}

/* --------------------------------------------------------------- texts */

static const char *health_word(int health)
{
    return health >= 90 ? "ottima" : health >= 80 ? "buona" : health >= 65 ? "discreta"
         : health >= 50 ? "consumata" : "da sostituire";
}

static void set_value(lv_obj_t *row, const char *text)
{
    lv_label_set_text(ui_row_value(row), text);
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;

    if (s_ui == NULL)
    {
        return;
    }

    battery_report_t r;
    char text[64];
    battery_health_get(&r);

    face_update(&s_ui->face, &r);

    if (r.percent < 0)
    {
        lv_label_set_text(s_ui->percent, "--");
    }
    else
    {
        lv_label_set_text_fmt(s_ui->percent, "%d%%", r.percent);
    }

    const char *state = r.full ? "Carica completa" : r.charging ? "In carica" : r.usb ? "Alimentato via USB"
                                                                                   : "In uso a batteria";
    if (r.mv > 0)
    {
        snprintf(text, sizeof(text), "%s · %d,%02d V", state, r.mv / 1000, (r.mv % 1000) / 10);
    }
    else
    {
        snprintf(text, sizeof(text), "%s", state);
    }

    lv_label_set_text(s_ui->status, text);

    if (r.health < 0)
    {
        set_value(s_ui->health_row, "-");
    }
    else
    {
        snprintf(text, sizeof(text), "%d%% · %s", r.health, health_word(r.health));
        set_value(s_ui->health_row, text);
    }

    if (r.first_seen > 0)
    {
        struct tm t;
        time_t since = (time_t)r.first_seen;
        localtime_r(&since, &t);
        snprintf(text, sizeof(text), "%lu,%lu dal %02d/%02d/%02d", (unsigned long)(r.cycles_x10 / 10),
                 (unsigned long)(r.cycles_x10 % 10), t.tm_mday, t.tm_mon + 1, t.tm_year % 100);
    }
    else
    {
        snprintf(text, sizeof(text), "%lu,%lu", (unsigned long)(r.cycles_x10 / 10), (unsigned long)(r.cycles_x10 % 10));
    }

    set_value(s_ui->cycles_row, text);

    if (r.days < 0)
    {
        set_value(s_ui->age_row, "ora non impostata");
    }
    else
    {
        snprintf(text, sizeof(text), "%d giorni · %d%%", r.days, r.health_age);
        set_value(s_ui->age_row, text);
    }

    if (r.tests == 0)
    {
        set_value(s_ui->resistance_row, "fai il test");
    }
    else if (r.tests == 1)
    {
        set_value(s_ui->resistance_row, "1 test (riferimento)");
    }
    else
    {
        snprintf(text, sizeof(text), "%+d%% · %d test", r.resistance_change, r.tests);
        set_value(s_ui->resistance_row, text);
    }

    if (r.usb || r.charging)
    {
        set_value(s_ui->time_row, "-");
    }
    else if (r.minutes_left < 0)
    {
        set_value(s_ui->time_row, "in calcolo");
    }
    else
    {
        snprintf(text, sizeof(text), "~%d h %02d min", r.minutes_left / 60, r.minutes_left % 60);
        set_value(s_ui->time_row, text);
    }
}

/* --------------------------------------------------------- health test */

typedef struct
{
    int idle_mv;
    int load_mv;
    int percent;
} test_result_t;

static void test_overlay_color(void *arg)
{
    if (s_ui != NULL && s_ui->test_overlay != NULL)
    {
        lv_obj_set_style_bg_color(s_ui->test_overlay, arg != NULL ? lv_color_white() : lv_color_black(), 0);
    }
}

static void test_done(void *arg)
{
    test_result_t *result = arg;

    power_settings_changed();   /* the user's brightness back */
    power_keep_screen_on(false);

    esp_err_t err = battery_health_add_test(result->idle_mv, result->load_mv, result->percent);
    free(result);

    if (s_ui == NULL)
    {
        return;
    }

    if (s_ui->test_overlay != NULL)
    {
        lv_obj_delete(s_ui->test_overlay);
        s_ui->test_overlay = NULL;
    }

    s_ui->testing = false;
    ui_toast(err == ESP_OK ? "Test salvato" : "Misura non affidabile: riprova lontano dal cavo");
    refresh(NULL);
}

static int average_mv(void)
{
    int sum = 0;
    int count = 0;

    for (int i = 0; i < TEST_SAMPLES; i++)
    {
        int mv = pmu_battery_mv_fresh();

        if (mv > 0)
        {
            sum += mv;
            count++;
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }

    return count > 0 ? sum / count : 0;
}

/* Black + minimum light, then white + full light: the drop is the battery's sag. */
static void test_task(void *arg)
{
    test_result_t *result = arg;

    display_set_brightness(1);
    vTaskDelay(pdMS_TO_TICKS(TEST_SETTLE_MS));
    result->idle_mv = average_mv();

    ui_async(test_overlay_color, (void *)1);
    vTaskDelay(pdMS_TO_TICKS(200));   /* white drawn */
    display_set_brightness(255);
    vTaskDelay(pdMS_TO_TICKS(TEST_SETTLE_MS));
    result->load_mv = average_mv();

    display_set_brightness(1);

    if (!ui_async(test_done, result))
    {
        free(result);
    }

    vTaskDelete(NULL);
}

static void test_confirmed(bool confirmed, void *user_data)
{
    (void)user_data;

    if (!confirmed || s_ui == NULL || s_ui->testing)
    {
        return;
    }

    test_result_t *result = calloc(1, sizeof(test_result_t));

    if (result == NULL)
    {
        return;
    }

    result->percent = state_get(STATE_BATTERY);
    s_ui->testing = true;
    power_keep_screen_on(true);

    s_ui->test_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ui->test_overlay);
    lv_obj_set_size(s_ui->test_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_ui->test_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ui->test_overlay, lv_color_black(), 0);
    lv_obj_set_clickable(s_ui->test_overlay, true);   /* no touches meanwhile */

    if (!sys_task_create(test_task, "bat_test", 3072, result, 3, SYS_CORE_ANY))
    {
        free(result);
        lv_obj_delete(s_ui->test_overlay);
        s_ui->test_overlay = NULL;
        s_ui->testing = false;
        power_keep_screen_on(false);
    }
}

static void on_test(lv_event_t *e)
{
    (void)e;

    battery_report_t r;
    battery_health_get(&r);

    if (r.percent < 0)
    {
        ui_toast("Nessuna batteria");
    }
    else if (r.usb || r.charging)
    {
        ui_toast("Scollega il cavo USB per il test");
    }
    else if (r.percent < TEST_MIN_PCT)
    {
        ui_toast("Carica troppo bassa per il test (minimo 20%)");
    }
    else
    {
        ui_confirm("Test salute", "Lo schermo diventa nero e poi bianco per 3 secondi: si misura quanto cala la "
                   "tensione sotto carico.", "Avvia", UI_COLOR_GREEN, test_confirmed, NULL);
    }
}

static void reset_confirmed(bool confirmed, void *user_data)
{
    (void)user_data;

    if (confirmed)
    {
        battery_health_reset();
        ui_toast("Dati azzerati");

        if (s_ui != NULL)
        {
            s_ui->face.key = -1;
            refresh(NULL);
        }
    }
}

static void on_reset(lv_event_t *e)
{
    (void)e;
    ui_confirm("Batteria nuova?", "Cicli, età e test ripartono da zero. Fallo solo dopo aver cambiato la batteria.",
               "Azzera", UI_COLOR_RED, reset_confirmed, NULL);
}

/* ------------------------------------------------------------------ app */

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    s_ui = calloc(1, sizeof(battery_ui_t));

    if (s_ui == NULL)
    {
        return;
    }

    lv_obj_t *page = ui_page(screen, "Batteria");

    face_create(&s_ui->face, page);

    s_ui->percent = lv_label_create(page);
    lv_obj_set_style_text_font(s_ui->percent, ui_font_big, 0);

    s_ui->status = ui_text(page, "", true);
    lv_obj_set_style_text_align(s_ui->status, LV_TEXT_ALIGN_CENTER, 0);

    ui_section(page, "SALUTE");
    s_ui->health_row = ui_row(page, LV_SYMBOL_BATTERY_FULL, UI_COLOR_GREEN, "Salute stimata", "", NULL, NULL);
    s_ui->cycles_row = ui_row(page, LV_SYMBOL_REFRESH, UI_COLOR_BLUE, "Cicli", "", NULL, NULL);
    s_ui->age_row = ui_row(page, LV_SYMBOL_LOOP, UI_COLOR_INDIGO, "Età", "", NULL, NULL);
    s_ui->resistance_row = ui_row(page, LV_SYMBOL_CHARGE, UI_COLOR_ORANGE, "Resistenza", "", NULL, NULL);
    s_ui->time_row = ui_row(page, LV_SYMBOL_EYE_OPEN, UI_COLOR_TEAL, "Autonomia", "", NULL, NULL);

    ui_row(page, LV_SYMBOL_PLAY, UI_COLOR_GREEN, "Test salute", NULL, on_test, NULL);
    ui_text(page, "La salute è una stima (nessun chip misura la capacità): il peggiore tra cicli contati, "
                  "età e resistenza interna. Fai il test ogni tanto, lontano dal cavo: confronta il calo di "
                  "tensione sotto carico con la prima misura.", true);
    ui_row(page, LV_SYMBOL_TRASH, UI_COLOR_RED, "Batteria nuova", NULL, on_reset, NULL);

    refresh(NULL);
    s_ui->refresh = lv_timer_create(refresh, REFRESH_MS, NULL);
    s_ui->blink = lv_timer_create(blink, BLINK_MS, NULL);
}

static void destroy(void)
{
    if (s_ui == NULL)
    {
        return;
    }

    lv_timer_delete(s_ui->refresh);
    lv_timer_delete(s_ui->blink);

    if (s_ui->test_overlay != NULL)
    {
        lv_obj_delete(s_ui->test_overlay);   /* the test task still ends with test_done() */
    }

    free(s_ui);
    s_ui = NULL;
}

const app_t battery_app = {
    .id = "battery",
    .name = "Batteria",
    .icon = LV_SYMBOL_BATTERY_3,
    .color = UI_COLOR_GREEN,
    .create = create,
    .destroy = destroy,
};
