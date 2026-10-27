/* Risonanza: a virtual Chladni plate driven by measured vibration or sound.
 * Left: live signal, spectrum and dominant frequency. Right: grains gathering
 * on the nodal lines of a simulated square plate. The figure is a simulation
 * guided by the sensor, never a reconstruction of the measured surface. */
#include "core/app.h"
#include "core/clock.h"
#include "core/devcon.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/sys.h"
#include "dsp.h"
#include "esp_log.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "hardware/microphone.h"
#include "install.h"
#include "measure.h"
#include "plate.h"
#include "store.h"
#include "ui/ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "risonanza";

enum { SOURCE_IMU, SOURCE_MIC, SOURCE_EXPLORE };

static int s_width, s_height;
static bool s_landscape;
static r_plate_t *s_plate;
static lv_obj_t *s_signal, *s_spectrum, *s_hz, *s_info, *s_status, *s_source_text, *s_freeze_text;
static lv_obj_t *s_compare, *s_compare_text, *s_plate_area;
static r_frame_t s_frame, s_frozen, s_slot[2];
static bool s_slot_valid[2];
static int s_source = SOURCE_IMU, s_mode, s_explore_mode = 4, s_compare_slot = -1;
static float s_energy, s_explore_energy = .6f;
static bool s_frozen_on;
static unsigned s_sequence;
static uint32_t s_message_until;

static const char *source_name(int source)
{
    return source == SOURCE_IMU ? "IMU" : source == SOURCE_MIC ? "MIC" : "ESPLORA";
}

/* Log-spaced: the sixteen modes cover the band of the active source. */
static int mode_for(const r_frame_t *frame)
{
    float low = frame->source == SOURCE_MIC ? 60 : 2, high = frame->source == SOURCE_MIC ? 2400 : 40;
    float t = logf(fmaxf(frame->hz, low) / low) / logf(high / low);
    int mode = (int)lroundf(t * 15);
    return mode < 0 ? 0 : mode > 15 ? 15 : mode;
}

/* 0 at three times the noise floor, 1 forty decibels above it. */
static float energy_for(const r_frame_t *frame)
{
    float floor = fmaxf(frame->noise * 3, 1e-6f);
    if (frame->rms <= floor)
    {
        return 0;
    }
    return fminf(1, log10f(frame->rms / floor) / 2);
}

static void message(const char *text)
{
    lv_label_set_text(s_status, text);
    s_message_until = lv_tick_get() + 2500;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int left, int top, int width,
                       const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, left, top);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int left, int top, int width, int height,
                        lv_event_cb_t callback, int code)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_set_pos(obj, left, top);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x1C2C3B), 0);
    lv_obj_set_style_radius(obj, 10, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_ext_click_area(obj, 4);
    lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, (void *)(intptr_t)code);
    lv_obj_t *text_obj = lv_label_create(obj);
    lv_label_set_text(text_obj, text);
    lv_obj_set_style_text_font(text_obj, &lv_font_montserrat_14, 0);
    lv_obj_center(text_obj);
    return text_obj;
}

static lv_obj_t *panel(lv_obj_t *parent, int left, int top, int width, int height)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, left, top);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x101B26), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    return obj;
}

static const r_frame_t *shown_frame(void)
{
    return s_frozen_on ? &s_frozen : &s_frame;
}

static void draw_signal(lv_event_t *event)
{
    const r_frame_t *frame = shown_frame();
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    int width = lv_area_get_width(&area), half = lv_area_get_height(&area) / 2 - 2;
    int middle = area.y1 + lv_area_get_height(&area) / 2;
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = 1;
    line.color = lv_color_hex(0x2A3B4C);
    line.p1 = (lv_point_precise_t){area.x1, middle};
    line.p2 = (lv_point_precise_t){area.x2, middle};
    lv_draw_line(layer, &line);
    if (s_source == SOURCE_EXPLORE && !s_frozen_on)
    {
        return;
    }
    float peak = fmaxf(frame->peak, frame->noise * 4);
    if (peak <= 0)
    {
        return;
    }
    line.width = 2;
    line.color = lv_color_hex(frame->valid ? 0x74FADE : 0x5D7A8F);
    /* 64 segments over 256 samples keep the redraw cheap. */
    for (int i = 0; i < 64; i++)
    {
        float a = frame->samples[i * 4] / peak, b = frame->samples[i * 4 + 3 < R_N ? i * 4 + 3 : R_N - 1] / peak;
        line.p1 = (lv_point_precise_t){area.x1 + i * (width - 1) / 64, middle - (int)(fmaxf(-1, fminf(1, a)) * half)};
        line.p2 = (lv_point_precise_t){area.x1 + (i + 1) * (width - 1) / 64,
                                       middle - (int)(fmaxf(-1, fminf(1, b)) * half)};
        lv_draw_line(layer, &line);
    }
}

static void draw_spectrum(lv_event_t *event)
{
    const r_frame_t *frame = shown_frame();
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    int width = lv_area_get_width(&area), height = lv_area_get_height(&area) - 2;
    lv_layer_t *layer = lv_event_get_layer(event);
    if (s_source == SOURCE_EXPLORE && !s_frozen_on)
    {
        return;
    }
    float maximum = 1e-9f;
    for (int i = 0; i < R_BINS; i++)
    {
        maximum = fmaxf(maximum, frame->spectrum[i]);
    }
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.bg_color = lv_color_hex(frame->valid ? 0xEECE8A : 0x5D7A8F);
    for (int i = 0; i < R_BINS; i++)
    {
        int h = (int)(frame->spectrum[i] / maximum * height);
        if (h < 1)
        {
            continue;
        }
        lv_area_t at = {area.x1 + i * width / R_BINS, area.y2 - h, area.x1 + (i + 1) * width / R_BINS - 2, area.y2};
        lv_draw_rect(layer, &bar, &at);
    }
}

static void update_labels(void)
{
    const r_frame_t *frame = shown_frame();
    char text[96];
    int m, n;
    r_mode(s_mode, &m, &n);
    if (s_source == SOURCE_EXPLORE && !s_frozen_on)
    {
        snprintf(text, sizeof(text), "modo (%d,%d)", m, n);
        lv_label_set_text(s_hz, text);
        snprintf(text, sizeof(text), "Trascina sulla piastra:\norizzontale modo, verticale energia %d%%",
                 (int)(s_explore_energy * 100));
        lv_label_set_text(s_info, text);
        return;
    }
    if (frame->valid)
    {
        snprintf(text, sizeof(text), frame->hz < 100 ? "%.1f Hz" : "%.0f Hz", (double)frame->hz);
    }
    else
    {
        snprintf(text, sizeof(text), "-- Hz");
    }
    lv_label_set_text(s_hz, text);
    const char *unit = frame->source == SOURCE_IMU ? "g" : "fs";
    snprintf(text, sizeof(text), "RMS %.4f %s  picco %.4f\nfiducia %d%%  modo (%d,%d)%s%s", (double)frame->rms, unit,
             (double)frame->peak, (int)(frame->confidence * 100), m, n, frame->clipped ? "  SATURO" : "",
             frame->timing_ok ? "" : "  TEMPI IRREGOLARI");
    lv_label_set_text(s_info, text);
}

static void show_compare(void)
{
    char text[200];
    bool valid = false;
    float ratio = s_slot_valid[0] && s_slot_valid[1] ? r_ratio(&s_slot[0], &s_slot[1], &valid) : 0;
    int length = 0;
    for (int i = 0; i < 2; i++)
    {
        if (s_slot_valid[i])
        {
            length += snprintf(text + length, sizeof(text) - length, "%c: %s %s  RMS %.4f\n", 'A' + i,
                               source_name(s_slot[i].source), s_slot[i].valid ? "" : "(senza picco)",
                               (double)s_slot[i].rms);
            if (s_slot[i].valid)
            {
                length += snprintf(text + length, sizeof(text) - length, "   %.1f Hz\n", (double)s_slot[i].hz);
            }
        }
        else
        {
            length += snprintf(text + length, sizeof(text) - length, "%c: vuoto\n", 'A' + i);
        }
    }
    if (valid)
    {
        snprintf(text + length, sizeof(text) - length, "A/B %.2fx  (%+.1f dB)", (double)ratio,
                 (double)(20 * log10f(fmaxf(ratio, 1e-9f))));
    }
    else if (s_slot_valid[0] && s_slot_valid[1])
    {
        snprintf(text + length, sizeof(text) - length, "Ampiezze non confrontabili\n(sorgente o asse diversi)");
    }
    lv_label_set_text(s_compare_text, text);
}

typedef struct
{
    int slot;
    r_frame_t frame;
} save_job_t;

static void save_done(void *arg)
{
    int result = (int)(intptr_t)arg;
    message(result < 0 ? "Salvataggio non riuscito" : result ? "Salvato in NVS e su SD" : "Salvato in NVS");
}

/* NVS and SD writes run in the sys worker, never in the LVGL task. */
static void save_job(void *arg)
{
    save_job_t *job = arg;
    int result = r_store_save(job->slot, &job->frame);
    free(job);
    ui_async(save_done, (void *)(intptr_t)result);
}

static void load_job(void *arg)
{
    (void)arg;
    r_frame_t frame;
    for (int slot = 0; slot < 2; slot++)
    {
        if (r_store_load(slot, &frame))
        {
            lv_port_lock();
            s_slot[slot] = frame;
            s_slot_valid[slot] = true;
            lv_port_unlock();
        }
    }
}

static void source_action(lv_event_t *event)
{
    (void)event;
    s_source = (s_source + 1) % 3;
    if (s_source == SOURCE_MIC && !microphone_supported())
    {
        s_source = SOURCE_EXPLORE;
        message("Microfono non disponibile su questa build");
    }
    r_measure_source(s_source);
    s_frozen_on = false;
    lv_label_set_text(s_freeze_text, LV_SYMBOL_PAUSE);
    lv_label_set_text(s_source_text, source_name(s_source));
    memset(&s_frame, 0, sizeof(s_frame));
    s_frame.source = s_source;
    s_sequence = 0;
}

static void calibrate_action(lv_event_t *event)
{
    (void)event;
    if (s_source != SOURCE_IMU)
    {
        message("Calibrazione solo con IMU");
        return;
    }
    r_measure_calibrate();
    message("Calibrazione: lascia fermo il dispositivo");
}

static void freeze_action(lv_event_t *event)
{
    (void)event;
    s_frozen_on = !s_frozen_on;
    if (s_frozen_on)
    {
        s_frozen = s_frame;
    }
    lv_label_set_text(s_freeze_text, s_frozen_on ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

static void save_action(lv_event_t *event)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(event);
    const r_frame_t *frame = shown_frame();
    if (s_source == SOURCE_EXPLORE && !s_frozen_on)
    {
        message("Niente da salvare in Esplora");
        return;
    }
    if (frame->sequence == 0 || frame->rate <= 0)
    {
        message("Attendi una misura");
        return;
    }
    save_job_t *job = malloc(sizeof(*job));
    if (!job)
    {
        message("Memoria insufficiente");
        return;
    }
    job->slot = slot;
    job->frame = *frame;
    s_slot[slot] = *frame;
    s_slot_valid[slot] = true;
    if (!sys_post(save_job, job))
    {
        free(job);
        message("Sistema occupato, riprova");
        return;
    }
    message(slot ? "Salvataggio B..." : "Salvataggio A...");
}

static void compare_action(lv_event_t *event)
{
    int code = (int)(intptr_t)lv_event_get_user_data(event);
    if (code < 0)
    {
        s_compare_slot = -1;
        lv_obj_set_hidden(s_compare, true);
        return;
    }
    if (code < 2)
    {
        s_compare_slot = s_slot_valid[code] ? code : -1;
        return;
    }
    show_compare();
    lv_obj_set_hidden(s_compare, false);
}

/* Esplora: horizontal position picks the mode, vertical the energy. */
static void plate_touch(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (s_source != SOURCE_EXPLORE || (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING))
    {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (!indev)
    {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_area_t area;
    lv_obj_get_coords(s_plate_area, &area);
    float fx = (float)(point.x - area.x1) / lv_area_get_width(&area);
    float fy = (float)(point.y - area.y1) / lv_area_get_height(&area);
    s_explore_mode = (int)fmaxf(0, fminf(15, fx * 16));
    s_explore_energy = fmaxf(.05f, fminf(1, 1 - fy));
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    r_measure_t measure;
    r_measure_get(&measure);
    bool fresh = measure.frame.sequence != s_sequence && measure.source == s_source;
    if (fresh)
    {
        s_sequence = measure.frame.sequence;
        s_frame = measure.frame;
        lv_obj_invalidate(s_signal);
        lv_obj_invalidate(s_spectrum);
    }
    int mode = s_mode;
    float target = 0;
    if (s_compare_slot >= 0)
    {
        mode = mode_for(&s_slot[s_compare_slot]);
        target = fmaxf(.35f, energy_for(&s_slot[s_compare_slot]));
    }
    else if (s_source == SOURCE_EXPLORE)
    {
        mode = s_explore_mode;
        target = s_explore_energy;
    }
    else if (s_frozen_on)
    {
        target = 0;
    }
    else
    {
        if (s_frame.valid)
        {
            mode = mode_for(&s_frame);
        }
        target = energy_for(&s_frame);
    }
    /* Grains settle slowly and react quickly, like a real plate. */
    s_energy += (target - s_energy) * (target > s_energy ? .35f : .08f);
    s_mode = mode;
    r_plate_frame(s_plate, s_mode, s_energy, s_frozen_on && s_compare_slot < 0);
    if (lv_tick_get() > s_message_until)
    {
        if (measure.error != ESP_OK && s_source != SOURCE_EXPLORE)
        {
            char text[64];
            snprintf(text, sizeof(text), "%s: %s", source_name(s_source), esp_err_to_name(measure.error));
            lv_label_set_text(s_status, text);
        }
        else
        {
            lv_label_set_text(s_status, s_frozen_on ? "Congelato" : s_source == SOURCE_EXPLORE ? "Simulazione libera"
                                                                     : "Simulazione guidata dal sensore");
        }
    }
    if (fresh || s_source == SOURCE_EXPLORE)
    {
        update_labels();
    }
}

/* Development console: "risonanza" prints the live measurement. */
bool devcon_app_command(char *line)
{
    if (strcmp(line, "risonanza") != 0)
    {
        return false;
    }
    r_measure_t measure;
    r_measure_get(&measure);
    printf("@RISONANZA source=%d running=%d errors=%u seq=%u rate=%.2f hz=%.2f valid=%d rms=%.6f mode=%d\n",
           measure.source, measure.running, measure.errors, measure.frame.sequence, (double)measure.frame.rate,
           (double)measure.frame.hz, measure.frame.valid, (double)measure.frame.rms, s_mode);
    printf("@OK risonanza\n");
    return true;
}

static bool home_back(void)
{
    if (!lv_obj_is_hidden(s_compare))
    {
        s_compare_slot = -1;
        lv_obj_set_hidden(s_compare, true);
        return true;
    }
    r_measure_stop();
    extapp_return_to_launcher();
    return true;
}

static void create_ui(void)
{
    s_width = lv_display_get_horizontal_resolution(lv_port_display());
    s_height = lv_display_get_vertical_resolution(lv_port_display());
    bool round = board_info()->round;
    s_landscape = !round && s_width > s_height;
    lv_obj_t *home = lv_obj_create(NULL);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, s_width, s_height);
    lv_obj_set_style_bg_color(home, lv_color_hex(0x070D14), 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(home, lv_color_white(), 0);
    lv_obj_set_scrollable(home, false);

    /* Landscape: data column on the left, plate on the right, controls below.
     * Portrait and round boards stack the plate above a compact data strip. */
    int bar = 46, gap = 8, top = 34;
    int plate, plate_x, plate_y, column_x, column_y, column_w;
    if (s_landscape)
    {
        plate = s_height - top - bar - gap * 2;
        plate_x = s_width - plate - gap;
        plate_y = top;
        column_x = gap;
        column_y = top;
        column_w = plate_x - gap * 2;
    }
    else
    {
        int inset = round ? s_width / 7 : gap;
        plate = (s_height - top - bar) * 52 / 100;
        plate = plate > s_width - inset * 2 ? s_width - inset * 2 : plate;
        plate_x = (s_width - plate) / 2;
        plate_y = top;
        column_x = inset;
        column_y = plate_y + plate + gap;
        column_w = s_width - inset * 2;
    }
    lv_obj_t *title = label(home, "RISONANZA", round ? s_width / 2 - 70 : gap, 6, 140, &lv_font_montserrat_16,
                            0xE5F8F4);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    s_status = label(home, "", round ? s_width / 2 - 100 : 160, round ? 24 : 8,
                     round ? 200 : s_width - 160 - gap, &lv_font_montserrat_12, 0x8FA7BF);
    if (round)
    {
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    }
    else
    {
        lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_RIGHT, 0);
    }

    s_plate_area = lv_obj_create(home);
    lv_obj_remove_style_all(s_plate_area);
    lv_obj_set_pos(s_plate_area, plate_x, plate_y);
    lv_obj_set_size(s_plate_area, plate, plate);
    lv_obj_set_scrollable(s_plate_area, false);
    lv_obj_set_clickable(s_plate_area, true);
    lv_obj_add_event_cb(s_plate_area, plate_touch, LV_EVENT_ALL, NULL);
    s_plate = r_plate_create(s_plate_area, 0, 0, plate);
    if (s_plate)
    {
        lv_obj_set_clickable(lv_obj_get_child(s_plate_area, 0), false);
    }

    int column_h = s_landscape ? plate : s_height - column_y - bar - gap;
    int graph_h = s_landscape ? (column_h - 110) / 2 : column_h / 3;
    s_hz = label(home, "-- Hz", column_x, column_y, column_w, s_landscape ? &lv_font_montserrat_32 : &lv_font_montserrat_24,
                 0x74FADE);
    int y = column_y + (s_landscape ? 42 : 30);
    lv_obj_t *box = panel(home, column_x, y, column_w, graph_h);
    s_signal = lv_obj_create(box);
    lv_obj_remove_style_all(s_signal);
    lv_obj_set_size(s_signal, column_w, graph_h);
    lv_obj_set_clickable(s_signal, false);
    lv_obj_add_event_cb(s_signal, draw_signal, LV_EVENT_DRAW_MAIN, NULL);
    y += graph_h + 6;
    box = panel(home, column_x, y, column_w, graph_h);
    s_spectrum = lv_obj_create(box);
    lv_obj_remove_style_all(s_spectrum);
    lv_obj_set_pos(s_spectrum, 4, 4);
    lv_obj_set_size(s_spectrum, column_w - 8, graph_h - 6);
    lv_obj_set_clickable(s_spectrum, false);
    lv_obj_add_event_cb(s_spectrum, draw_spectrum, LV_EVENT_DRAW_MAIN, NULL);
    y += graph_h + 6;
    s_info = label(home, "", column_x, y, column_w, &lv_font_montserrat_12, 0xAEC3D4);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);

    /* Bottom bar: source, calibrate, freeze, A, B, compare. */
    int inset = round ? s_width / 6 : gap;
    int width = s_width - inset * 2, count = 6, bw = (width - (count - 1) * 6) / count;
    int by = s_height - bar - (round ? s_height / 14 : 4);
    s_source_text = button(home, source_name(s_source), inset, by, bw, bar - 6, source_action, 0);
    button(home, "CAL", inset + (bw + 6), by, bw, bar - 6, calibrate_action, 0);
    s_freeze_text = button(home, LV_SYMBOL_PAUSE, inset + (bw + 6) * 2, by, bw, bar - 6, freeze_action, 0);
    button(home, "A", inset + (bw + 6) * 3, by, bw, bar - 6, save_action, 0);
    button(home, "B", inset + (bw + 6) * 4, by, bw, bar - 6, save_action, 1);
    button(home, "A|B", inset + (bw + 6) * 5, by, bw, bar - 6, compare_action, 2);

    int cw = s_landscape ? column_w : width, ch = s_landscape ? plate : s_height - top - bar - gap * 2;
    s_compare = panel(home, s_landscape ? column_x : inset, top, cw, ch);
    lv_obj_set_clickable(s_compare, true);
    lv_obj_set_style_bg_color(s_compare, lv_color_hex(0x13222F), 0);
    lv_obj_set_style_border_width(s_compare, 1, 0);
    lv_obj_set_style_border_color(s_compare, lv_color_hex(0x2C4255), 0);
    s_compare_text = label(s_compare, "", 10, 8, cw - 20, &lv_font_montserrat_14, 0xE5F8F4);
    lv_label_set_long_mode(s_compare_text, LV_LABEL_LONG_WRAP);
    int sw = (cw - 32) / 3;
    button(s_compare, "MOSTRA A", 8, ch - 44, sw, 36, compare_action, 0);
    button(s_compare, "MOSTRA B", 16 + sw, ch - 44, sw, 36, compare_action, 1);
    button(s_compare, "CHIUDI", 24 + sw * 2, ch - 44, sw, 36, compare_action, -1);
    lv_obj_set_hidden(s_compare, true);

    app_manager_set_home(home, home_back);
    lv_timer_create(refresh, 33, NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(extapp_sdk_init());
    sys_check_last_reset();
    sys_worker_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    /* Rectangular portrait panels run landscape; the launcher orientation is untouched. */
    bool rectangle = !board_info()->round && board_info()->width != board_info()->height;
    ESP_ERROR_CHECK(lv_port_init_orientation(rectangle && board_info()->width < board_info()->height ? 1 : 0));
    clock_init();
    lv_port_lock();
    ui_init();
    create_ui();
    lv_port_unlock();
    app_manager_init();
    ESP_ERROR_CHECK(power_init());
    power_keep_screen_on(true);
    r_measure_source(SOURCE_IMU);
    r_measure_start();
    sys_post(load_job, NULL);
    devcon_start();
    sys_post(risonanza_install_launcher, NULL);
    sys_heap_log("risonanza ready");
    ESP_LOGI(TAG, "Ready: %s; IMU %s, microphone %s", board_info()->name, "on demand",
             microphone_supported() ? "available" : "not built");
}
