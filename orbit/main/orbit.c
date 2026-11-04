/* ORBIT: an offline kinetic space for focus, breaks, paced breathing, short
 * guided meditations and generated ambient sound.
 *
 * Motion follows real time (esp_timer), never whole seconds: the planet's
 * size is recomputed every frame and only applied when it moves by a pixel
 * per side, so a slow breath costs a handful of small redraws per second.
 * The planet has no shadow and no transform; its glow is opaque.
 * A one-shot system timer owns the session deadline; a 100 ms guide timer
 * plays the phase tones even while the display sleeps. */
#include "orbit.h"
#include "orbit_audio.h"
#include "core/app.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/sys.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "services/sound.h"
#include "ui/ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAME_MS 33
#define DAYS     7

enum
{
    MODE_FOCUS,
    MODE_BREAK,
    MODE_BREATH,
    MODE_MEDITATE,
};

typedef struct
{
    const char *name;
    uint16_t ms[4]; /* inhale, hold, exhale, hold */
} breath_t;

typedef struct
{
    const char *text;
    uint16_t seconds;
} step_t;

typedef struct
{
    const char *title;
    const char *button;
    const step_t *steps;
    int count;
} meditation_t;

static const breath_t s_breaths[] = {
    {"Coerenza 5-5", {5000, 0, 5000, 0}},
    {"Quadrato 4-4-4-4", {4000, 4000, 4000, 4000}},
    {"Calma 4-7-8", {4000, 7000, 8000, 0}},
    {"Rilassamento 4-8", {4000, 0, 8000, 0}},
};
#define BREATHS ((int)(sizeof(s_breaths) / sizeof(s_breaths[0])))
static const char *s_phase_names[] = {"Inspira", "Trattieni", "Espira", "Trattieni"};

static const step_t s_presence[] = {
    {"Siediti comodo.\nLascia andare le spalle.", 20},
    {"Porta l'attenzione al respiro, così com'è.", 30},
    {"Senti l'aria entrare fresca e uscire tiepida.", 30},
    {"Se la mente vaga, torna al respiro. Senza giudizio.", 40},
    {"Allarga l'attenzione a tutto il corpo.", 30},
    {"Ancora tre respiri lenti.", 20},
    {"Porta con te questa calma.", 10},
};
static const step_t s_body_scan[] = {
    {"Chiudi gli occhi.\nFai un respiro profondo.", 20},
    {"Senti i piedi: il peso, il calore, il contatto.", 45},
    {"Sali alle gambe.\nLascia che si rilassino.", 45},
    {"Bacino e pancia morbidi, a ogni espiro.", 45},
    {"Il petto si alza e si abbassa da solo.", 45},
    {"Mani, braccia, spalle: sciogli ogni tensione.", 50},
    {"Collo, mascella, fronte.\nTutto si distende.", 50},
    {"Senti il corpo intero, presente e calmo.", 40},
    {"Muovi piano le dita.\nTorna quando vuoi.", 20},
};
static const step_t s_gratitude[] = {
    {"Respira con calma.\nAmmorbidisci il viso.", 20},
    {"Pensa a una persona che ti ha aiutato.", 45},
    {"Ricorda un momento semplice di oggi.", 45},
    {"Senti la gratitudine nel petto, come un calore.", 45},
    {"Augura una buona giornata a te e a chi ami.", 45},
    {"Tre respiri lenti.\nSorridi, se ti va.", 30},
    {"Grazie.\nRiprendi con leggerezza.", 10},
};
static const step_t s_sleep[] = {
    {"Sdraiati.\nLascia che il letto ti sostenga.", 30},
    {"Espira più a lungo di quanto inspiri.", 60},
    {"Ogni espiro scioglie un pensiero della giornata.", 60},
    {"Le palpebre sono pesanti.\nIl respiro rallenta.", 60},
    {"Immagina un cielo notturno, ampio e quieto.", 60},
    {"Conta all'indietro da dieci, piano.", 70},
    {"Non c'è niente da fare adesso.", 60},
    {"Lasciati andare al sonno.", 80},
};
#define STEPS(a) a, (int)(sizeof(a) / sizeof(a[0]))
static const meditation_t s_meditations[] = {
    {"Tre minuti di presenza", "Presenza", STEPS(s_presence)},
    {"Scansione del corpo", "Corpo", STEPS(s_body_scan)},
    {"Gratitudine", "Gratitudine", STEPS(s_gratitude)},
    {"Prima di dormire", "Notte", STEPS(s_sleep)},
};
#define MEDITATIONS ((int)(sizeof(s_meditations) / sizeof(s_meditations[0])))
static const uint8_t s_sleep_minutes[] = {0, 15, 30, 45, 60, 90};

typedef struct
{
    uint8_t breath;
    uint8_t breath_minutes;
    uint8_t focus_minutes;
    uint8_t volume;
    uint8_t sleep_index;
    uint8_t tones;
} orbit_cfg_t;

typedef struct
{
    uint32_t day;            /* days since 1970 of minutes[0] */
    uint16_t minutes[DAYS];  /* [0] today, [1] yesterday... */
} orbit_days_t;

typedef struct
{
    lv_obj_t *screen;
    lv_obj_t *content;
    uint32_t color;    /* colour of the scene */
    lv_obj_t *planet;
    lv_obj_t *moon[4];
    lv_obj_t *headline;
    lv_obj_t *count;
    lv_obj_t *caption;
    lv_obj_t *border;
    lv_obj_t *ambient_row;
    lv_obj_t *sleep_row;
    lv_obj_t *breath_row;
    lv_obj_t *focus_button, *breath_button; /* labels carry the minutes */
    lv_timer_t *animation;
    int page; /* 0 scenic, 1 controls, 2 session */
    int mode;
    int meditation;
    int last_seconds;
    int last_phase;
    int last_step;
    int diameter;      /* applied planet diameter */
    int base;          /* planet diameter at full breath */
    int cx, cy;        /* planet centre offset from the screen centre */
    int dx, dy;        /* touch parallax */
    int64_t moon_us;   /* moon clock, stops while paused */
    int64_t last_frame;
    bool paused;
    bool session;
    bool keep_on;
    int64_t total_us;
    int64_t remaining_us;
} orbit_ui_t;

static const char *TAG = "orbit";
static orbit_ui_t *s_ui;
static orbit_cfg_t s_cfg = {.breath = 0, .breath_minutes = 3, .focus_minutes = 25, .volume = 45, .sleep_index = 2, .tones = 1};
static esp_timer_handle_t s_deadline_timer;
static esp_timer_handle_t s_guide_timer;
static portMUX_TYPE s_session_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_active;
static int64_t s_deadline;
static int64_t s_total_us;
static int s_guide_mode;
static int s_guide_item; /* breath pattern or meditation */
static uint32_t s_session_packed;
static atomic_uint s_finish_pending;
static atomic_uint s_minutes;
static atomic_uint s_sessions;
static orbit_days_t s_days;
static portMUX_TYPE s_days_lock = portMUX_INITIALIZER_UNLOCKED;

static void render(orbit_ui_t *ui);

static uint32_t mode_color(int mode)
{
    static const uint32_t colors[] = {UI_COLOR_PURPLE, UI_COLOR_ORANGE, UI_COLOR_TEAL, UI_COLOR_PINK};
    return colors[mode & 3];
}

static int screen_w(void)
{
    return lv_display_get_horizontal_resolution(NULL);
}

static int screen_h(void)
{
    return lv_display_get_vertical_resolution(NULL);
}

static int screen_min(void)
{
    return screen_w() < screen_h() ? screen_w() : screen_h();
}

/* ---- persistence (sys task) */

static void save_cfg_job(void *arg)
{
    (void)arg;
    nvs_handle_t handle;
    if (nvs_open("orbit", NVS_READWRITE, &handle) != ESP_OK) return;
    orbit_cfg_t copy = s_cfg;
    nvs_set_blob(handle, "cfg", &copy, sizeof(copy));
    nvs_commit(handle);
    nvs_close(handle);
}

static void save_cfg(void)
{
    sys_post(save_cfg_job, NULL);
}

static uint32_t today(void)
{
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    if (local.tm_year + 1900 < 2024) return 0; /* clock not set yet */
    /* Days since 1970-01-01 of the local date (civil calendar, no time zone math). */
    int y = local.tm_year + 1900 - (local.tm_mon < 2);
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (local.tm_mon + (local.tm_mon < 2 ? 10 : -2)) + 2) / 5 + local.tm_mday - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + doe - 719468);
}

/* Shift the 7-day window so that minutes[0] is `day`. Caller holds s_days_lock. */
static void days_roll(orbit_days_t *days, uint32_t day)
{
    if (day == 0 || day <= days->day) return;
    uint32_t shift = day - days->day;
    if (days->day == 0 || shift >= DAYS) memset(days->minutes, 0, sizeof(days->minutes));
    else
    {
        memmove(&days->minutes[shift], &days->minutes[0], (DAYS - shift) * sizeof(days->minutes[0]));
        memset(days->minutes, 0, shift * sizeof(days->minutes[0]));
    }
    days->day = day;
}

static void finish(void *arg)
{
    uint32_t packed = (uint32_t)(uintptr_t)arg;
    int mode = packed & 3;
    uint32_t minutes = packed >> 2;
    if (mode != MODE_BREAK)
    {
        atomic_fetch_add(&s_minutes, minutes);
        atomic_fetch_add(&s_sessions, 1);
        orbit_days_t copy;
        uint32_t day = today();
        portENTER_CRITICAL(&s_days_lock);
        days_roll(&s_days, day);
        if (day != 0 && s_days.minutes[0] < 60000) s_days.minutes[0] += minutes;
        copy = s_days;
        portEXIT_CRITICAL(&s_days_lock);
        nvs_handle_t handle;
        if (nvs_open("orbit", NVS_READWRITE, &handle) == ESP_OK)
        {
            nvs_set_u32(handle, "minutes", atomic_load(&s_minutes));
            nvs_set_u32(handle, "sessions", atomic_load(&s_sessions));
            if (day != 0) nvs_set_blob(handle, "days", &copy, sizeof(copy));
            nvs_commit(handle);
            nvs_close(handle);
        }
    }
    if (sound_alarm_playing()) return;
    if (orbit_audio_available() && (s_cfg.tones || orbit_audio_active())) orbit_audio_tone(ORBIT_TONE_END);
    else sound_notification();
}

/* ---- session clock */

static void expired(void *arg)
{
    (void)arg;
    uint32_t packed = 0;
    portENTER_CRITICAL(&s_session_lock);
    if (s_active && esp_timer_get_time() >= s_deadline)
    {
        s_active = false;
        packed = s_session_packed;
    }
    portEXIT_CRITICAL(&s_session_lock);
    if (packed != 0)
    {
        atomic_store(&s_finish_pending, packed);
        power_wake(POWER_WAKE_NOTIFICATION);
    }
}

static int64_t remaining(orbit_ui_t *ui)
{
    if (ui->paused) return ui->remaining_us;
    int64_t deadline;
    portENTER_CRITICAL(&s_session_lock);
    deadline = s_deadline;
    portEXIT_CRITICAL(&s_session_lock);
    int64_t value = deadline - esp_timer_get_time();
    return value > 0 ? value : 0;
}

/* Breath phase at `elapsed_ms` into the session: returns the planet scale
 * 0..1 (eased), the phase index and the ms left in that phase. */
static float breath_at(const breath_t *breath, int64_t elapsed_ms, int *phase, int *left_ms)
{
    int cycle = breath->ms[0] + breath->ms[1] + breath->ms[2] + breath->ms[3];
    int at = (int)(elapsed_ms % cycle);
    for (int i = 0; i < 4; i++)
    {
        if (at < breath->ms[i])
        {
            float x = (float)at / breath->ms[i];
            float eased = 0.5f - 0.5f * cosf(x * (float)M_PI);
            *phase = i;
            *left_ms = breath->ms[i] - at;
            return i == 0 ? eased : i == 1 ? 1.0f : i == 2 ? 1.0f - eased : 0.0f;
        }
        at -= breath->ms[i];
    }
    *phase = 0;
    *left_ms = 0;
    return 0.0f;
}

static int meditation_step(const meditation_t *meditation, int64_t elapsed_ms)
{
    int64_t at = elapsed_ms / 1000;
    for (int i = 0; i < meditation->count; i++)
    {
        if (at < meditation->steps[i].seconds) return i;
        at -= meditation->steps[i].seconds;
    }
    return meditation->count - 1;
}

/* esp_timer task, every 100 ms during guided sessions: phase tones that keep
 * playing with the display off (eyes closed). Touches only its own statics. */
static void guide_tick(void *arg)
{
    (void)arg;
    static int last = -1;
    static int64_t last_deadline;
    int64_t deadline, total;
    bool active;
    int mode, item;
    portENTER_CRITICAL(&s_session_lock);
    active = s_active;
    deadline = s_deadline;
    total = s_total_us;
    mode = s_guide_mode;
    item = s_guide_item;
    portEXIT_CRITICAL(&s_session_lock);
    if (!active || !s_cfg.tones || sound_alarm_playing()) return;
    if (deadline != last_deadline)
    {
        /* New session or resumed after a pause: a resume re-arms the deadline. */
        last_deadline = deadline;
        last = -2;
    }
    int64_t elapsed_ms = (total - (deadline - esp_timer_get_time())) / 1000;
    if (elapsed_ms < 0) elapsed_ms = 0;
    int now;
    if (mode == MODE_BREATH)
    {
        int left;
        breath_at(&s_breaths[item], elapsed_ms, &now, &left);
    }
    else now = meditation_step(&s_meditations[item], elapsed_ms);
    if (now == last) return;
    bool resumed = last == -2 && elapsed_ms > 1500;
    last = now;
    if (resumed) return; /* no tone for the phase we resume in the middle of */
    if (mode == MODE_BREATH)
        orbit_audio_tone(now == 0 ? ORBIT_TONE_INHALE : now == 2 ? ORBIT_TONE_EXHALE : ORBIT_TONE_HOLD);
    else orbit_audio_tone(ORBIT_TONE_BELL);
}

static void keep_screen(orbit_ui_t *ui, bool keep)
{
    if (ui->keep_on == keep) return;
    ui->keep_on = keep;
    power_keep_screen_on(keep);
}

static void guide(orbit_ui_t *ui, bool on)
{
    bool guided = on && (ui->mode == MODE_BREATH || ui->mode == MODE_MEDITATE);
    esp_timer_stop(s_guide_timer);
    if (guided && orbit_audio_available()) esp_timer_start_periodic(s_guide_timer, 100 * 1000);
    orbit_audio_hold(guided && s_cfg.tones && orbit_audio_available());
    /* Breathing follows the planet: keep the screen on. Meditation may be eyes closed. */
    keep_screen(ui, on && ui->mode == MODE_BREATH);
}

static void stop_session(orbit_ui_t *ui)
{
    portENTER_CRITICAL(&s_session_lock);
    s_active = false;
    portEXIT_CRITICAL(&s_session_lock);
    esp_timer_stop(s_deadline_timer);
    guide(ui, false);
    ui->session = false;
    ui->paused = false;
}

static bool arm(orbit_ui_t *ui, int64_t duration)
{
    if (duration < 1) return false;
    uint32_t minutes = (uint32_t)((ui->total_us + 30000000) / 60000000);
    portENTER_CRITICAL(&s_session_lock);
    s_deadline = esp_timer_get_time() + duration;
    s_total_us = ui->total_us;
    s_guide_mode = ui->mode;
    s_guide_item = ui->mode == MODE_BREATH ? s_cfg.breath : ui->meditation;
    s_session_packed = ((minutes < 1 ? 1 : minutes) << 2) | (uint32_t)ui->mode;
    s_active = true;
    portEXIT_CRITICAL(&s_session_lock);
    if (esp_timer_start_once(s_deadline_timer, duration) != ESP_OK)
    {
        portENTER_CRITICAL(&s_session_lock);
        s_active = false;
        portEXIT_CRITICAL(&s_session_lock);
        ui_toast("Timer non disponibile");
        return false;
    }
    guide(ui, true);
    return true;
}

/* ---- scene */

static lv_obj_t *text(lv_obj_t *parent, const char *value, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, value);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    return object;
}

static lv_obj_t *circle(lv_obj_t *parent, int size, lv_color_t color, lv_opa_t opa)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, size, size);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, opa, 0);
    lv_obj_set_clickable(object, false);
    lv_obj_set_scrollable(object, false);
    return object;
}

/* The planet and its glow: one object of fixed size, drawn by planet_draw().
 * Glow and rim follow ui->diameter; the gradient core never changes size, so a
 * breath only redraws the frame around the square inscribed in the core (four
 * rectangles): the text inside the planet is never redrawn while it moves. */
static int core_size(const orbit_ui_t *ui)
{
    return (int)(ui->base * 0.84f) - 10;
}

static int halo_size(int d)
{
    return d * 116 / 100 & ~1;
}

static void planet_draw(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t box;
    lv_obj_get_coords(lv_event_get_target_obj(event), &box);
    int cx = (box.x1 + box.x2 + 1) / 2, cy = (box.y1 + box.y2 + 1) / 2;
    int d = ui->diameter;
    const int sizes[4] = {halo_size(d), d * 106 / 100 & ~1, d, core_size(ui)};
    const uint8_t glow[3] = {26, 58, 150};
    for (int i = 0; i < 4; i++)
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius = LV_RADIUS_CIRCLE;
        dsc.bg_opa = LV_OPA_COVER;
        if (i < 3)
        {
            /* Opaque glow and rim: the colour mixed over the black sky, no blending. */
            dsc.bg_color = ui_glow(ui->color, glow[i]);
            if (i == 2)
            {
                dsc.border_width = 2;
                dsc.border_color = lv_color_lighten(lv_color_hex(ui->color), 110);
                dsc.border_opa = LV_OPA_COVER;
            }
        }
        else
        {
            dsc.bg_grad.dir = LV_GRAD_DIR_VER;
            dsc.bg_grad.stops_count = 2;
            dsc.bg_grad.stops[0].color = lv_color_hex(ui->color);
            dsc.bg_grad.stops[0].opa = LV_OPA_COVER;
            dsc.bg_grad.stops[0].frac = 0;
            dsc.bg_grad.stops[1].color = ui_glow(ui->color, 30);
            dsc.bg_grad.stops[1].opa = LV_OPA_COVER;
            dsc.bg_grad.stops[1].frac = 255;
        }
        int r = sizes[i] / 2;
        lv_area_t area = {cx - r, cy - r, cx + r - 1, cy + r - 1};
        lv_draw_rect(layer, &dsc, &area);
    }
}

/* Planet at scale 0..1 (0 = smallest). Applies only pixel changes. */
static void planet_set(orbit_ui_t *ui, float scale)
{
    int d = (int)(ui->base * (0.84f + 0.16f * scale));
    d &= ~1; /* even: grows one pixel per side, stays centred */
    if (d == ui->diameter) return;
    int old = ui->diameter;
    ui->diameter = d;
    lv_area_t box;
    lv_obj_get_coords(ui->planet, &box);
    if (old == 0)
    {
        lv_obj_invalidate(ui->planet);
        return;
    }
    int cx = (box.x1 + box.x2 + 1) / 2, cy = (box.y1 + box.y2 + 1) / 2;
    int r = halo_size(old > d ? old : d) / 2 + 2;
    int q = core_size(ui) * 707 / 2000 - 2; /* half side of the square inside the core */
    const lv_area_t frame[4] = {
        {cx - r, cy - r, cx + r, cy - q - 1},
        {cx - r, cy + q + 1, cx + r, cy + r},
        {cx - r, cy - q, cx - q - 1, cy + q},
        {cx + q + 1, cy - q, cx + r, cy + q},
    };
    for (int i = 0; i < 4; i++) lv_obj_invalidate_area(ui->planet, &frame[i]);
}

static void planet_place(orbit_ui_t *ui)
{
    lv_obj_align(ui->planet, LV_ALIGN_CENTER, ui->cx + ui->dx, ui->cy + ui->dy);
}

static void moons_place(orbit_ui_t *ui)
{
    int radius = screen_min() * 39 / 100;
    int w = screen_w(), h = screen_h();
    for (int i = 0; i < 4; i++)
    {
        /* Each moon has its own period so the system never looks mechanical. */
        int period = 36000 + i * 9000;
        int angle = (int)((ui->moon_us / 1000 + i * period / 4) % period * 360 / period);
        int size = lv_obj_get_width(ui->moon[i]);
        lv_obj_set_pos(ui->moon[i], w / 2 + ui->cx + lv_trigo_cos(angle) * radius / 32768 - size / 2,
                       h / 2 + ui->cy + lv_trigo_sin(angle) * radius / 32768 - size / 2);
    }
}

static void set_text(lv_obj_t *label, const char *value)
{
    if (label != NULL && strcmp(lv_label_get_text(label), value) != 0) lv_label_set_text(label, value);
}

static void session_labels(orbit_ui_t *ui, int64_t rem)
{
    int seconds = (int)((rem + 999999) / 1000000);
    int total = (int)(ui->total_us / 1000000);
    char line[64];
    if (seconds != ui->last_seconds)
    {
        ui->last_seconds = seconds;
        ui_border_progress_set(ui->border, total - seconds, total);
    }
    if (seconds == 0)
    {
        set_text(ui->headline, "Fatto.");
        set_text(ui->count, "");
        set_text(ui->caption, ui->mode == MODE_BREAK ? "Bentornato. Si riparte." : "Un piccolo passo. Un'orbita nuova.");
        return;
    }
    int64_t elapsed_ms = (ui->total_us - rem) / 1000;
    if (ui->mode == MODE_BREATH)
    {
        const breath_t *breath = &s_breaths[s_cfg.breath];
        int phase, left;
        breath_at(breath, elapsed_ms, &phase, &left);
        int cycle = breath->ms[0] + breath->ms[1] + breath->ms[2] + breath->ms[3];
        set_text(ui->headline, s_phase_names[phase]);
        snprintf(line, sizeof(line), "%d", (left + 999) / 1000);
        set_text(ui->count, line);
        if (ui->paused) set_text(ui->caption, "In pausa · tocca per riprendere");
        else
        {
            snprintf(line, sizeof(line), "Ciclo %d di %d · %d:%02d", (int)(elapsed_ms / cycle) + 1,
                     (int)(ui->total_us / 1000 / cycle), seconds / 60, seconds % 60);
            set_text(ui->caption, line);
        }
    }
    else if (ui->mode == MODE_MEDITATE)
    {
        const meditation_t *meditation = &s_meditations[ui->meditation];
        int step = meditation_step(meditation, elapsed_ms);
        if (step != ui->last_step)
        {
            ui->last_step = step;
            set_text(ui->headline, meditation->steps[step].text);
            lv_obj_fade_in(ui->headline, 700, 0);
        }
        if (ui->paused) set_text(ui->caption, "In pausa · tocca per riprendere");
        else
        {
            snprintf(line, sizeof(line), "%d di %d · %d:%02d", step + 1, meditation->count, seconds / 60, seconds % 60);
            set_text(ui->caption, line);
        }
    }
    else
    {
        snprintf(line, sizeof(line), "%02d:%02d", seconds / 60, seconds % 60);
        set_text(ui->headline, line);
        set_text(ui->caption, ui->paused ? "In pausa · tocca per riprendere" : "Tocca per mettere in pausa");
    }
}

/* Line under the planet on the scenic page: what is playing. */
static void scenic_labels(orbit_ui_t *ui)
{
    orbit_ambient_t ambient = orbit_audio_ambient();
    char line[64];
    if (ambient == ORBIT_AMBIENT_OFF) snprintf(line, sizeof(line), "Il tuo tempo. Il tuo spazio.");
    else
    {
        int left = orbit_audio_sleep_left();
        if (left > 0) snprintf(line, sizeof(line), LV_SYMBOL_AUDIO " %s · %d min", orbit_audio_name(ambient), (left + 59) / 60);
        else snprintf(line, sizeof(line), LV_SYMBOL_AUDIO " %s", orbit_audio_name(ambient));
    }
    set_text(ui->caption, line);
}

static void animate(lv_timer_t *timer)
{
    orbit_ui_t *ui = lv_timer_get_user_data(timer);
    uint32_t pending = atomic_exchange(&s_finish_pending, 0);
    if (pending != 0)
    {
        if (!sys_post(finish, (void *)(uintptr_t)pending)) atomic_store(&s_finish_pending, pending);
        guide(ui, false);
    }
    int64_t now = esp_timer_get_time();
    int64_t step = ui->last_frame == 0 ? 0 : now - ui->last_frame;
    ui->last_frame = now;
    if (lv_screen_active() != ui->screen || power_screen() >= POWER_SCREEN_AOD || ui->page == 1) return;
    if (ui_motion_paused()) return;
    if (step > 200000) step = 200000; /* after a sleep: no jump */

    int64_t rem = ui->session ? remaining(ui) : 0;
    bool running = ui->page == 2 && ui->session && rem > 0 && !ui->paused;
    if (!ui->paused) ui->moon_us += step;

    float scale;
    if (ui->page == 2 && ui->session && ui->mode == MODE_BREATH && rem > 0)
    {
        int phase, left;
        scale = breath_at(&s_breaths[s_cfg.breath], (ui->total_us - rem) / 1000, &phase, &left);
    }
    else
    {
        /* Idle drift: one slow breath every 8 s (meditation: 6 s, deeper). */
        float period = ui->page == 2 && ui->mode == MODE_MEDITATE && running ? 6.0f : 8.0f;
        float depth = ui->page == 2 && ui->mode == MODE_MEDITATE ? 0.5f : 0.3f;
        float t = (float)(ui->moon_us % (int64_t)(period * 1e6f)) / (period * 1e6f);
        scale = 0.6f + depth * (0.5f - 0.5f * cosf(t * 2.0f * (float)M_PI));
    }
    planet_set(ui, scale);
    moons_place(ui);
    static int64_t next_stats;
    if (now >= next_stats)
    {
        /* Slowest LVGL pass over the last 10 s of animation (target: under 33 ms). */
        uint32_t pass_ms, timeouts;
        lv_port_take_stats(&pass_ms, &timeouts);
        if (next_stats != 0) ESP_LOGI(TAG, "frame max %lu ms (page %d mode %d)", (unsigned long)pass_ms, ui->page, ui->mode);
        next_stats = now + 10000000;
    }
    if (ui->page == 2 && ui->session) session_labels(ui, rem);
    else if (ui->page == 0 && now / 1000000 != ui->last_seconds)
    {
        ui->last_seconds = (int)(now / 1000000);
        scenic_labels(ui);
    }
}

static void to_controls(orbit_ui_t *ui)
{
    ui->page = 1;
    render(ui);
}

static void pause_resume(orbit_ui_t *ui)
{
    if (ui->paused)
    {
        if (arm(ui, ui->remaining_us))
        {
            ui->paused = false;
            ui->last_seconds = -1;
        }
        return;
    }
    ui->remaining_us = remaining(ui);
    portENTER_CRITICAL(&s_session_lock);
    s_active = false;
    portEXIT_CRITICAL(&s_session_lock);
    esp_timer_stop(s_deadline_timer);
    guide(ui, false);
    ui->paused = true;
    ui->last_seconds = -1;
    set_text(ui->caption, "In pausa · tocca per riprendere");
}

static void planet_tap(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    if (ui->page == 0) to_controls(ui);
    else if (remaining(ui) == 0)
    {
        stop_session(ui);
        ui->page = 1;
        render(ui);
    }
    else pause_resume(ui);
}

static void gesture(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (direction == LV_DIR_TOP) to_controls(ui);
    else if (direction == LV_DIR_BOTTOM && ui->page != 0) orbit_back();
}

static void pressure(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    if (ui->page == 1 || ui->planet == NULL) return;
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;
    if (indev == NULL) return;
    lv_indev_get_point(indev, &point);
    int dx = (point.x - screen_w() / 2) / 9, dy = (point.y - screen_h() / 2) / 12;
    if (dx == ui->dx && dy == ui->dy) return;
    ui->dx = dx;
    ui->dy = dy;
    planet_place(ui);
}

static void release(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    if (ui->planet == NULL || ui->page == 1 || (ui->dx == 0 && ui->dy == 0)) return;
    ui->dx = ui->dy = 0;
    planet_place(ui);
}

static void knob(lv_event_t *event)
{
    lv_port_knob_t *turn = lv_event_get_param(event);
    orbit_ui_t *ui = lv_event_get_user_data(event);
    if (turn == NULL || ui->page == 1 || orbit_audio_ambient() == ORBIT_AMBIENT_OFF) return;
    int volume = orbit_audio_volume() + turn->delta * 5;
    volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    orbit_audio_set_volume(volume);
    s_cfg.volume = (uint8_t)volume;
    save_cfg();
    char line[32];
    snprintf(line, sizeof(line), "Volume %d%%", volume);
    ui_toast(line);
    turn->handled = true;
}

static void scene(orbit_ui_t *ui)
{
    uint32_t color = ui->page == 0 ? UI_COLOR_PURPLE : mode_color(ui->mode);
    lv_obj_t *root = ui->content;
    int w = screen_w(), h = screen_h(), s = screen_min();
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_clickable(root, true);
    lv_obj_add_event_cb(root, planet_tap, LV_EVENT_CLICKED, ui);
    lv_obj_set_gesture_bubble(root, false);
    lv_obj_add_event_cb(root, gesture, LV_EVENT_GESTURE, ui);
    lv_obj_add_event_cb(root, pressure, LV_EVENT_PRESSING, ui);
    lv_obj_add_event_cb(root, release, LV_EVENT_RELEASED, ui);
    for (int i = 0; i < 22; i++)
    {
        lv_obj_t *star = circle(root, i % 5 == 0 ? 3 : 1, lv_color_hex(UI_COLOR_TEXT_DIM), i % 5 == 0 ? LV_OPA_70 : LV_OPA_40);
        lv_obj_set_style_radius(star, i % 5 == 0 ? 2 : 0, 0);
        lv_obj_set_pos(star, (i * 83 + 19) % w, (i * 137 + 43) % h);
    }
    ui->base = s * 62 / 100; /* with its glow: the largest disc redrawn under 33 ms */
    ui->cx = 0;
    ui->cy = -s / 15;
    ui->dx = ui->dy = 0;
    ui->diameter = 0;
    ui->color = color;
    ui->planet = lv_obj_create(root);
    lv_obj_remove_style_all(ui->planet);
    lv_obj_set_size(ui->planet, halo_size(ui->base) + 4, halo_size(ui->base) + 4);
    lv_obj_set_clickable(ui->planet, false);
    lv_obj_set_scrollable(ui->planet, false);
    lv_obj_add_event_cb(ui->planet, planet_draw, LV_EVENT_DRAW_MAIN, ui);
    planet_place(ui);
    for (int i = 0; i < 4; i++)
        ui->moon[i] = circle(root, 5 + i * 2, lv_color_hex(i == 1 ? UI_COLOR_TEAL : UI_COLOR_TEXT), LV_OPA_COVER);

    bool meditating = ui->page == 2 && ui->mode == MODE_MEDITATE;
    const lv_font_t *font = ui->page == 0 ? ui_font_title
                            : meditating ? UI_FONT_BODY
                            : ui->mode == MODE_BREATH ? ui_font_headline : ui_font_big;
    ui->headline = text(root, ui->page == 0 ? "ORBIT" : "", font, UI_COLOR_TEXT);
    if (meditating)
    {
        lv_obj_set_width(ui->headline, core_size(ui) * 80 / 100); /* about the core's square: rarely redrawn */
        lv_label_set_long_mode(ui->headline, LV_LABEL_LONG_WRAP);
    }
    lv_obj_align(ui->headline, LV_ALIGN_CENTER, ui->cx, ui->cy - (ui->mode == MODE_BREATH && ui->page == 2 ? 14 : 0));
    ui->count = text(root, "", UI_FONT_LARGE, UI_COLOR_TEXT_DIM);
    lv_obj_align(ui->count, LV_ALIGN_CENTER, ui->cx, ui->cy + 24);
    ui->caption = text(root, "", UI_FONT_SMALL, UI_COLOR_TEXT);
    lv_obj_set_width(ui->caption, w * 86 / 100);
    lv_obj_align(ui->caption, LV_ALIGN_CENTER, 0, ui->cy + ui->base / 2 + s / 14);
    lv_obj_t *hint = text(root, ui->page == 0 ? "Tocca · esplora · respira" : "Scorri in alto per i controlli", UI_FONT_SMALL, UI_COLOR_TEXT_DIM);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, ui->cy + ui->base / 2 + s / 14 + 24);
    if (ui->page == 2)
    {
        const char *name = ui->mode == MODE_FOCUS ? "FOCUS" : ui->mode == MODE_BREAK ? "PAUSA"
                           : ui->mode == MODE_BREATH ? s_breaths[s_cfg.breath].name
                                                     : s_meditations[ui->meditation].title;
        lv_obj_t *label = text(root, name, UI_FONT_SMALL, UI_COLOR_TEXT_DIM);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, ui->cy - ui->base / 2 - s / 16);
    }
    ui->last_seconds = -1;
    ui->last_step = -1;
    ui->last_frame = 0;
    ui_border_progress_color(ui->border, color);
    if (ui->border != NULL) lv_obj_set_hidden(ui->border, ui->page != 2);
    animate(ui->animation);
}

/* ---- controls */

static void start(orbit_ui_t *ui, int mode, int meditation)
{
    stop_session(ui);
    ui->mode = mode;
    ui->meditation = meditation;
    if (mode == MODE_FOCUS) ui->total_us = (int64_t)s_cfg.focus_minutes * 60 * 1000000;
    else if (mode == MODE_BREAK) ui->total_us = 5LL * 60 * 1000000;
    else if (mode == MODE_BREATH)
    {
        /* Whole cycles only: the session ends on a calm exhale. */
        const breath_t *breath = &s_breaths[s_cfg.breath];
        int64_t cycle = breath->ms[0] + breath->ms[1] + breath->ms[2] + breath->ms[3];
        int64_t cycles = ((int64_t)s_cfg.breath_minutes * 60000 + cycle / 2) / cycle;
        ui->total_us = (cycles < 1 ? 1 : cycles) * cycle * 1000;
    }
    else
    {
        int seconds = 0;
        for (int i = 0; i < s_meditations[meditation].count; i++) seconds += s_meditations[meditation].steps[i].seconds;
        ui->total_us = (int64_t)seconds * 1000000;
    }
    ui->remaining_us = ui->total_us;
    ui->session = arm(ui, ui->remaining_us);
    if (!ui->session) return;
    ui->page = 2;
    render(ui);
}

static void replace_session(bool confirmed, void *arg)
{
    int code = (int)(intptr_t)arg;
    if (confirmed) start(s_ui, code & 3, code >> 2);
}

static void start_mode(lv_event_t *event)
{
    int code = (int)(intptr_t)lv_event_get_user_data(event);
    if (s_ui->session && remaining(s_ui) > 0)
        ui_confirm("Nuova orbita", "Terminare la sessione in corso?", "Inizia", mode_color(code & 3), replace_session, (void *)(intptr_t)code);
    else start(s_ui, code & 3, code >> 2);
}

/* "Concentrazione · 25 min": the slider's value is on the button it starts. */
static void minutes_label(lv_obj_t *button, const char *name, int minutes)
{
    if (button != NULL) lv_label_set_text_fmt(lv_obj_get_child(button, 0), "%s · %d min", name, minutes);
}

static void focus_minutes(lv_event_t *event)
{
    s_cfg.focus_minutes = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(event));
    minutes_label(s_ui->focus_button, "Concentrazione", s_cfg.focus_minutes);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) save_cfg();
}

static void breath_minutes(lv_event_t *event)
{
    s_cfg.breath_minutes = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(event));
    minutes_label(s_ui->breath_button, "Respira", s_cfg.breath_minutes);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) save_cfg();
}

static void breath_next(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    s_cfg.breath = (uint8_t)((s_cfg.breath + 1) % BREATHS);
    lv_label_set_text(ui_row_value(ui->breath_row), s_breaths[s_cfg.breath].name);
    save_cfg();
}

static void ambient_next(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    orbit_ambient_t next = (orbit_ambient_t)((orbit_audio_ambient() + 1) % ORBIT_AMBIENT_COUNT);
    orbit_audio_set_volume(s_cfg.volume);
    if (next != ORBIT_AMBIENT_OFF && orbit_audio_ambient() == ORBIT_AMBIENT_OFF)
        orbit_audio_set_sleep_timer(s_sleep_minutes[s_cfg.sleep_index]);
    orbit_audio_set_ambient(next);
    lv_label_set_text(ui_row_value(ui->ambient_row), orbit_audio_name(next));
}

static void sleep_next(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    s_cfg.sleep_index = (uint8_t)((s_cfg.sleep_index + 1) % sizeof(s_sleep_minutes));
    int minutes = s_sleep_minutes[s_cfg.sleep_index];
    if (orbit_audio_ambient() != ORBIT_AMBIENT_OFF) orbit_audio_set_sleep_timer(minutes);
    char value[16];
    if (minutes == 0) snprintf(value, sizeof(value), "Mai");
    else snprintf(value, sizeof(value), "%d min", minutes);
    lv_label_set_text(ui_row_value(ui->sleep_row), value);
    save_cfg();
}

static void volume(lv_event_t *event)
{
    s_cfg.volume = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(event));
    orbit_audio_set_volume(s_cfg.volume);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) save_cfg();
}

static void tones(lv_event_t *event)
{
    s_cfg.tones = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    save_cfg();
    if (s_ui->session && !s_ui->paused) guide(s_ui, true);
}

static void resume_session(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    ui->page = 2;
    render(ui);
}

static void stop(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    stop_session(ui);
    ui->page = 0;
    render(ui);
}

static void leave(lv_event_t *event)
{
    (void)event;
    orbit_audio_set_ambient(ORBIT_AMBIENT_OFF);
    extapp_return_to_launcher();
}

/* Release events save the slider value once, at the end of the drag. */
static lv_obj_t *slider(lv_obj_t *page, const char *icon, const char *label, int min, int max, int value, lv_event_cb_t cb, void *data)
{
    lv_obj_t *object = ui_slider_row(page, icon, label, min, max, value, cb, data);
    lv_obj_add_event_cb(object, cb, LV_EVENT_RELEASED, data);
    return object;
}

/* Seven bars, today on the right, with the day initials under them. */
static void week_chart(lv_obj_t *page)
{
    static const char *initials[] = {"D", "L", "M", "M", "G", "V", "S"};
    orbit_days_t days;
    uint32_t day = today();
    portENTER_CRITICAL(&s_days_lock);
    days_roll(&s_days, day);
    days = s_days;
    portEXIT_CRITICAL(&s_days_lock);
    int width = screen_min() * 64 / 100, height = 64, bar = width / DAYS - 8;
    uint32_t top = 1, week = 0;
    for (int i = 0; i < DAYS; i++)
    {
        top = days.minutes[i] > top ? days.minutes[i] : top;
        week += days.minutes[i];
    }
    lv_obj_t *chart = lv_obj_create(page);
    lv_obj_remove_style_all(chart);
    lv_obj_set_size(chart, width, height + 20);
    lv_obj_set_style_align(chart, LV_ALIGN_TOP_MID, 0);
    lv_obj_set_scrollable(chart, false);
    lv_obj_set_clickable(chart, false);
    for (int i = 0; i < DAYS; i++)
    {
        int age = DAYS - 1 - i;
        int value = days.minutes[age];
        int bar_h = value == 0 ? 3 : 6 + (height - 6) * value / (int)top;
        lv_obj_t *column = circle(chart, bar, lv_color_hex(UI_COLOR_TEAL), age == 0 ? LV_OPA_COVER : LV_OPA_50);
        lv_obj_set_size(column, bar, bar_h);
        lv_obj_set_style_radius(column, 3, 0);
        lv_obj_set_pos(column, i * (width / DAYS) + 4, height - bar_h);
        if (day != 0)
        {
            lv_obj_t *letter = text(chart, initials[(day - age + 4) % 7], UI_FONT_CAPTION, age == 0 ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM);
            lv_obj_set_width(letter, width / DAYS);
            lv_obj_set_pos(letter, i * (width / DAYS), height + 4);
        }
    }
    char line[96];
    if (day == 0) snprintf(line, sizeof(line), "Imposta l'ora per vedere i giorni.");
    else snprintf(line, sizeof(line), "Oggi %u min · 7 giorni %lu min", (unsigned)days.minutes[0], (unsigned long)week);
    ui_text(page, line, true);
}

static void controls(orbit_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Il tuo spazio");
    ui_text(page, "Una cosa alla volta.", true);
    if (ui->session)
    {
        ui_button(page, "Torna alla sessione", UI_COLOR_CARD_HI, resume_session, ui);
        ui_button(page, "Termina sessione", UI_COLOR_RED, stop, ui);
    }
    ui_section(page, "Concentrazione");
    slider(page, LV_SYMBOL_LOOP, "Minuti", 1, 90, s_cfg.focus_minutes, focus_minutes, ui);
    ui->focus_button = ui_button(page, "", mode_color(MODE_FOCUS), start_mode, (void *)(intptr_t)MODE_FOCUS);
    minutes_label(ui->focus_button, "Concentrazione", s_cfg.focus_minutes);
    ui_button(page, "Pausa · 5 minuti", mode_color(MODE_BREAK), start_mode, (void *)(intptr_t)MODE_BREAK);

    ui_section(page, "Respiro");
    ui->breath_row = ui_row(page, LV_SYMBOL_REFRESH, mode_color(MODE_BREATH), "Ritmo", s_breaths[s_cfg.breath].name, breath_next, ui);
    slider(page, LV_SYMBOL_LOOP, "Minuti di respiro", 1, 20, s_cfg.breath_minutes, breath_minutes, ui);
    ui->breath_button = ui_button(page, "", mode_color(MODE_BREATH), start_mode, (void *)(intptr_t)MODE_BREATH);
    minutes_label(ui->breath_button, "Respira", s_cfg.breath_minutes);

    ui_section(page, "Meditazioni guidate");
    for (int i = 0; i < MEDITATIONS; i++)
    {
        int seconds = 0;
        for (int k = 0; k < s_meditations[i].count; k++) seconds += s_meditations[i].steps[k].seconds;
        char label[64];
        snprintf(label, sizeof(label), "%s · %d min", s_meditations[i].button, (seconds + 30) / 60);
        ui_button(page, label, mode_color(MODE_MEDITATE), start_mode, (void *)(intptr_t)(MODE_MEDITATE | i << 2));
    }

    if (orbit_audio_available())
    {
        ui_section(page, "Suoni");
        ui->ambient_row = ui_row(page, LV_SYMBOL_AUDIO, UI_COLOR_BLUE, "Suono", orbit_audio_name(orbit_audio_ambient()), ambient_next, ui);
        slider(page, LV_SYMBOL_VOLUME_MAX, "Volume", 0, 100, s_cfg.volume, volume, ui);
        char value[16];
        int minutes = s_sleep_minutes[s_cfg.sleep_index];
        if (minutes == 0) snprintf(value, sizeof(value), "Mai");
        else snprintf(value, sizeof(value), "%d min", minutes);
        ui->sleep_row = ui_row(page, LV_SYMBOL_POWER, UI_COLOR_INDIGO, "Spegni dopo", value, sleep_next, ui);
        ui_switch_row(page, LV_SYMBOL_BELL, UI_COLOR_TEAL, "Toni guida", s_cfg.tones, tones, ui);
        ui_text(page, board_info()->has_knob ? "Generati in tempo reale. La rotella regola il volume sulla scena."
                                             : "Generati in tempo reale, nessun file.", true);
    }

    ui_section(page, "Il tuo percorso");
    week_chart(page);
    char stats[96];
    snprintf(stats, sizeof(stats), "%lu %s · %lu minuti dedicati a ciò che conta.",
             (unsigned long)atomic_load(&s_sessions), atomic_load(&s_sessions) == 1 ? "sessione" : "sessioni",
             (unsigned long)atomic_load(&s_minutes));
    ui_text(page, stats, true);
    ui_text(page, "Funziona offline. Il display può riposare: il timer continua.", true);
    ui_button(page, "Torna al launcher", UI_COLOR_CARD_HI, leave, ui);
}

static void render(orbit_ui_t *ui)
{
    /* Delete a scene container instead of repeatedly accumulating callbacks. */
    if (ui->content != NULL) lv_obj_delete(ui->content);
    ui->content = lv_obj_create(ui->screen);
    lv_obj_remove_style_all(ui->content);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(ui->content, false);
    lv_obj_move_to_index(ui->content, 0);
    ui->planet = NULL;
    ui->headline = ui->count = ui->caption = NULL;
    ui->ambient_row = ui->sleep_row = ui->breath_row = NULL;
    ui->focus_button = ui->breath_button = NULL;
    /* Breathing keeps the screen on only while its scene is visible. */
    keep_screen(ui, ui->page == 2 && ui->session && !ui->paused && ui->mode == MODE_BREATH);
    if (ui->page == 1)
    {
        if (ui->border != NULL) lv_obj_set_hidden(ui->border, true);
        controls(ui);
    }
    else scene(ui);
}

bool orbit_back(void)
{
    if (s_ui == NULL || s_ui->page == 0)
    {
        orbit_audio_set_ambient(ORBIT_AMBIENT_OFF);
        extapp_return_to_launcher();
        return true;
    }
    s_ui->page = s_ui->page == 2 ? 1 : s_ui->session ? 2 : 0;
    render(s_ui);
    return true;
}

void orbit_create(lv_obj_t *screen)
{
    s_ui = calloc(1, sizeof(*s_ui));
    ESP_ERROR_CHECK(s_ui == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_ui->screen = screen;
    nvs_handle_t handle;
    if (nvs_open("orbit", NVS_READONLY, &handle) == ESP_OK)
    {
        uint32_t value = 0;
        if (nvs_get_u32(handle, "minutes", &value) == ESP_OK) atomic_store(&s_minutes, value);
        if (nvs_get_u32(handle, "sessions", &value) == ESP_OK) atomic_store(&s_sessions, value);
        orbit_cfg_t cfg;
        size_t size = sizeof(cfg);
        if (nvs_get_blob(handle, "cfg", &cfg, &size) == ESP_OK && size == sizeof(cfg))
        {
            if (cfg.breath < BREATHS) s_cfg.breath = cfg.breath;
            if (cfg.breath_minutes >= 1 && cfg.breath_minutes <= 20) s_cfg.breath_minutes = cfg.breath_minutes;
            if (cfg.focus_minutes >= 1 && cfg.focus_minutes <= 90) s_cfg.focus_minutes = cfg.focus_minutes;
            if (cfg.volume <= 100) s_cfg.volume = cfg.volume;
            if (cfg.sleep_index < sizeof(s_sleep_minutes)) s_cfg.sleep_index = cfg.sleep_index;
            s_cfg.tones = cfg.tones != 0;
        }
        size = sizeof(s_days);
        if (nvs_get_blob(handle, "days", &s_days, &size) != ESP_OK || size != sizeof(s_days)) memset(&s_days, 0, sizeof(s_days));
        nvs_close(handle);
    }
    orbit_audio_init();
    orbit_audio_set_volume(s_cfg.volume);
    const esp_timer_create_args_t args = {.callback = expired, .name = "orbit_deadline"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_deadline_timer));
    const esp_timer_create_args_t guide_args = {.callback = guide_tick, .name = "orbit_guide"};
    ESP_ERROR_CHECK(esp_timer_create(&guide_args, &s_guide_timer));
    s_ui->border = ui_border_progress_create(screen, UI_COLOR_PURPLE);
    s_ui->animation = lv_timer_create(animate, FRAME_MS, s_ui);
    ESP_ERROR_CHECK(s_ui->animation == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    if (board_info()->has_knob) lv_obj_add_event_cb(screen, knob, lv_port_knob_event(), s_ui);
    render(s_ui);
    ESP_LOGI(TAG, "Orbit 2.0 ready (%dx%d, audio %d)", screen_w(), screen_h(), orbit_audio_available());
}
