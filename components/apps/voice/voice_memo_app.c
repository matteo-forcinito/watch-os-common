#include "apps_internal.h"

#include "core/lv_port.h"
#include "core/settings.h"
#include "hardware/board.h"
#include "services/voice_memo.h"
#include "ui/ui.h"
#include "esp_attr.h"
#include <string.h>
#include <stdio.h>

/* The recording surface fills the display. Files and playback live in a
 * separate overlay so the main screen feels like one physical red button. */
static lv_obj_t *s_screen, *s_button, *s_time, *s_hint, *s_border, *s_menu, *s_lock;
static lv_obj_t *s_name, *s_play, *s_delete, *s_previous, *s_next;
static lv_timer_t *s_tick;
static lv_obj_t *s_meter, *s_levels[24];
static lv_obj_t *s_play_meter, *s_play_levels[24], *s_volume_label;
static int s_selected;
static bool s_held, s_locked, s_gesture_handled;
static int s_drawn_selected, s_drawn_count;
static char s_drawn_name[VOICE_MEMO_NAME_SIZE];
static voice_memo_phase_t s_drawn_phase;
static uint32_t s_drawn_second;
static EXT_RAM_BSS_ATTR voice_memo_snapshot_t s_snapshot;

static bool active(voice_memo_phase_t phase)
{
    return phase != VOICE_MEMO_IDLE && phase != VOICE_MEMO_ERROR;
}

static void depth(void *object, int32_t value)
{
    /* Colour step only: the button is large, scaling it or moving a shadow
     * under it would redraw most of the screen on every frame. */
    lv_obj_t *button = object;
    lv_obj_set_style_bg_color(button, value > 50 ? ui_glow(TK_RED, TK_GLOW_G1) : ui_glow(TK_RED, TK_GLOW_G2), 0);
}

static void animate_depth(bool pressed)
{
    lv_anim_delete(s_button, depth);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_button);
    lv_anim_set_exec_cb(&animation, depth);
    lv_anim_set_values(&animation, pressed ? 0 : 100, pressed ? 100 : 0);
    lv_anim_set_duration(&animation, pressed ? 110 : 180);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    voice_memo_snapshot(&s_snapshot);
    bool busy = active(s_snapshot.phase);
    if (s_selected >= s_snapshot.count)
    {
        s_selected = 0;
    }
    uint32_t ms = s_snapshot.elapsed_ms;
    bool show_levels = s_snapshot.phase == VOICE_MEMO_RECORDING || s_snapshot.phase == VOICE_MEMO_PLAYING;
    lv_obj_set_hidden(s_meter, !show_levels);
    if (s_play_meter != NULL)
    {
        lv_obj_set_style_opa(s_play_meter, s_snapshot.phase == VOICE_MEMO_PLAYING ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    lv_obj_set_hidden(s_time, !busy);
    lv_obj_set_hidden(s_hint, show_levels);
    lv_obj_set_hidden(s_lock, !s_locked);
    for (int i = 0; i < 24; i++)
    {
        int height = 1 + s_snapshot.levels[i] * 29 / 60;
        lv_obj_set_height(s_levels[i], height);
        lv_obj_set_y(s_levels[i], (30 - height) / 2);
        uint32_t color = s_snapshot.levels[i] >= 60 ? 0xFF625B :
                         s_snapshot.levels[i] >= 57 ? 0xFFD060 : 0xFFEBDD;
        lv_obj_set_style_bg_color(s_levels[i], lv_color_hex(color), 0);
        if (s_play_meter != NULL)
        {
            lv_obj_set_height(s_play_levels[i], height);
            lv_obj_set_y(s_play_levels[i], (30 - height) / 2);
            lv_obj_set_style_bg_color(s_play_levels[i], lv_color_hex(color), 0);
        }
    }
    if (ms / 1000 != s_drawn_second || s_snapshot.phase != s_drawn_phase)
    {
        lv_label_set_text_fmt(s_time, "%02lu:%02lu", (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60));
        s_drawn_second = ms / 1000;
    }
    if (s_snapshot.phase != s_drawn_phase)
    {
        const char *message = "Tieni premuto per registrare";
        switch (s_snapshot.phase)
        {
            case VOICE_MEMO_STARTING: message = "Avvio..."; break;
            case VOICE_MEMO_RECORDING: message = "Rilascia per salvare"; break;
            case VOICE_MEMO_PLAYING: message = "Riproduzione"; break;
            case VOICE_MEMO_SAVING: message = "Salvataggio..."; break;
            case VOICE_MEMO_SCANNING: message = "Lettura memo..."; break;
            case VOICE_MEMO_ERROR: message = "Operazione interrotta: riprova"; break;
            default: break;
        }
        lv_label_set_text(s_hint, message);
        lv_obj_set_style_border_color(s_button, s_snapshot.phase == VOICE_MEMO_RECORDING ? lv_color_hex(TK_RED)
                                                                                    : ui_glow(TK_RED, TK_GLOW_G1), 0);
        if (!busy && (s_drawn_phase == VOICE_MEMO_RECORDING || s_locked))
        {
            s_held = false;
            s_locked = false;
            lv_obj_set_hidden(s_lock, true);
            s_selected = 0;
        }
        if (!s_held && s_drawn_phase == VOICE_MEMO_RECORDING)
        {
            animate_depth(false);
        }
        s_drawn_phase = s_snapshot.phase;
    }
    lv_obj_set_disabled(s_button, busy && !s_held && !s_locked);
    if (s_border != NULL)
    {
        lv_obj_set_hidden(s_border, !busy);
        uint32_t limit = s_snapshot.phase == VOICE_MEMO_PLAYING ? s_snapshot.duration_ms : VOICE_MEMO_LIMIT_SECONDS * 1000;
        ui_border_progress_set(s_border, (int32_t)ms, (int32_t)limit);
    }
    if (s_menu != NULL)
    {
        if (s_selected != s_drawn_selected || s_snapshot.count != s_drawn_count ||
            (s_snapshot.count > 0 && strcmp(s_drawn_name, s_snapshot.names[s_selected]) != 0))
        {
            char date[64];
            if (s_snapshot.count > 0)
            {
                voice_memo_format_name(s_snapshot.names[s_selected], date, sizeof(date));
                lv_label_set_text_fmt(s_name, "%s\n%s %d / %d", date, s_selected == 0 ? "Ultima memo ·" : "Memo",
                                      s_selected + 1, s_snapshot.count);
            }
            else
            {
                lv_label_set_text(s_name, "Nessuna memo");
            }
            snprintf(s_drawn_name, sizeof(s_drawn_name), "%s", s_snapshot.count > 0 ? s_snapshot.names[s_selected] : "");
            s_drawn_selected = s_selected;
            s_drawn_count = s_snapshot.count;
        }
        lv_label_set_text(lv_obj_get_child(s_play, 0), s_snapshot.phase == VOICE_MEMO_PLAYING ? "Stop" : LV_SYMBOL_PLAY " Ascolta");
        lv_obj_set_disabled(s_play, (busy && s_snapshot.phase != VOICE_MEMO_PLAYING) || s_snapshot.count == 0);
        lv_obj_set_disabled(s_delete, busy || s_snapshot.count == 0);
        lv_obj_set_disabled(s_previous, busy || s_snapshot.count < 2);
        lv_obj_set_disabled(s_next, busy || s_snapshot.count < 2);
    }
}

static void record_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE)
    {
        if (!s_gesture_handled && (s_held || s_locked) &&
            lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_BOTTOM)
        {
            s_gesture_handled = true;
            s_locked = !s_locked;
            lv_obj_set_hidden(s_lock, !s_locked);
        }
    }
    else if (code == LV_EVENT_PRESSED)
    {
        s_gesture_handled = false;
        s_held = s_locked || voice_memo_record();
        if (s_held)
        {
            animate_depth(true);
        }
        else
        {
            ui_toast("Microfono occupato o scheda SD assente");
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        if (s_locked)
        {
            s_held = false;
            return;
        }
        if (s_held)
        {
            voice_memo_stop();
            s_held = false;
            animate_depth(false);
        }
    }
}

static void stop(void)
{
    s_held = false;
    s_locked = false;
    voice_memo_stop();
}

static void play_event(lv_event_t *e)
{
    (void)e;
    if (s_snapshot.phase == VOICE_MEMO_PLAYING)
    {
        voice_memo_stop();
    }
    else if (!voice_memo_play(s_selected))
    {
        ui_toast("Memo non disponibile");
    }
}

static void select_event(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_snapshot.count > 0)
    {
        s_selected = (s_selected + delta + s_snapshot.count) % s_snapshot.count;
        tick(NULL);
    }
}

static void delete_confirmed(bool confirmed, void *user)
{
    if (confirmed && !voice_memo_delete((int)(intptr_t)user))
    {
        ui_toast("Impossibile eliminare la memo");
    }
}

static void delete_event(lv_event_t *e)
{
    (void)e;
    ui_confirm("Eliminare la memo?", s_snapshot.names[s_selected], "Elimina", UI_COLOR_RED,
               delete_confirmed, (void *)(intptr_t)s_selected);
}

static void close_menu(lv_event_t *e)
{
    (void)e;
    stop();
    lv_obj_delete(s_menu);
    s_menu = NULL;
    s_play_meter = NULL;
}

static void exit_event(lv_event_t *e)
{
    (void)e;
    app_back();
}

static lv_obj_t *level_meter(lv_obj_t *parent, lv_obj_t **bars);

static void volume_event(lv_event_t *event)
{
    int percent = lv_slider_get_value(lv_event_get_target_obj(event));
    voice_memo_set_volume(percent);
    lv_label_set_text_fmt(s_volume_label, "Volume memo · %d%%", percent);
}

static void menu_event(lv_event_t *e)
{
    (void)e;
    if (s_menu != NULL)
    {
        return;
    }
    stop();
    s_selected = 0;
    s_drawn_selected = -1;
    s_drawn_count = -1;
    animate_depth(false);
    s_menu = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_size(s_menu, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_menu, lv_color_hex(TK_RED_G3), 0);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_menu, true);
    lv_obj_set_scrollable(s_menu, false);
    lv_obj_t *page = ui_page(s_menu, "Le tue memo");
    s_name = ui_text(page, "Lettura...", false);
    lv_obj_set_style_text_font(s_name, UI_FONT_SMALL, 0);
    s_play_meter = level_meter(page, s_play_levels);
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_previous = ui_button(row, LV_SYMBOL_LEFT, UI_COLOR_CARD_HI, select_event, (void *)(intptr_t)-1);
    s_next = ui_button(row, LV_SYMBOL_RIGHT, UI_COLOR_CARD_HI, select_event, (void *)(intptr_t)1);
    lv_obj_set_size(s_previous, 100, 44);
    lv_obj_set_size(s_next, 100, 44);
    s_play = ui_button(page, LV_SYMBOL_PLAY " Ascolta", UI_COLOR_BLUE, play_event, NULL);
    lv_obj_set_height(s_play, 48);
    lv_obj_t *volume = lv_obj_create(page);
    lv_obj_remove_style_all(volume);
    lv_obj_set_size(volume, LV_PCT(100), 54);
    lv_obj_set_scrollable(volume, false);
    s_volume_label = lv_label_create(volume);
    lv_obj_set_style_text_font(s_volume_label, UI_FONT_SMALL, 0);
    lv_label_set_text_fmt(s_volume_label, "Volume memo · %d%%", voice_memo_volume());
    lv_obj_align(s_volume_label, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *slider = lv_slider_create(volume);
    lv_obj_set_size(slider, 240, 8);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, voice_memo_volume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, volume_event, LV_EVENT_VALUE_CHANGED, NULL);
    s_delete = ui_button(page, "Elimina memo", UI_COLOR_CARD_HI, delete_event, NULL);
    lv_obj_set_height(s_delete, 48);
    lv_obj_set_height(ui_button(page, "Torna al microfono", UI_COLOR_RED, close_menu, NULL), 48);
    lv_obj_set_height(ui_button(page, "Chiudi app", UI_COLOR_CARD_HI, exit_event, NULL), 48);
    ui_text(page, "WAV locali sulla SD, non cifrati.\nMassimo 60 s. Nessun invio automatico.", true);
    if (s_border != NULL)
    {
        lv_obj_move_foreground(s_border);
    }
    voice_memo_scan();
    tick(NULL);
}

static lv_obj_t *level_meter(lv_obj_t *parent, lv_obj_t **bars)
{
    lv_obj_t *meter = lv_obj_create(parent);
    lv_obj_remove_style_all(meter);
    lv_obj_set_size(meter, 238, 30);
    lv_obj_set_clickable(meter, false);
    lv_obj_set_scrollable(meter, false);
    for (int i = 0; i < 24; i++)
    {
        bars[i] = lv_obj_create(meter);
        lv_obj_remove_style_all(bars[i]);
        lv_obj_set_size(bars[i], 6, 1);
        lv_obj_set_x(bars[i], i * 10);
        lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bars[i], 3, 0);
        lv_obj_set_clickable(bars[i], false);
    }
    return meter;
}

/* Vector microphone, independent of bitmap assets and missing font glyphs. */
static void microphone_icon(lv_obj_t *parent)
{
    lv_obj_t *body = lv_obj_create(parent);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 48, 78);
    lv_obj_set_style_radius(body, 24, 0);
    lv_obj_set_style_bg_color(body, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_clickable(body, false);
    lv_obj_set_scrollable(body, false);
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, 86, 92);
    lv_arc_set_bg_angles(arc, 0, 180);
    lv_obj_set_style_arc_color(arc, lv_color_white(), 0);
    lv_obj_set_style_arc_width(arc, 7, 0);
    lv_obj_set_style_arc_rounded(arc, true, 0);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_clickable(arc, false);
    lv_obj_set_scrollable(arc, false);
    lv_obj_t *stem = lv_obj_create(parent);
    lv_obj_remove_style_all(stem);
    lv_obj_set_size(stem, 7, 22);
    lv_obj_set_style_bg_color(stem, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_align(stem, LV_ALIGN_CENTER, 0, 43);
    lv_obj_set_clickable(stem, false);
    lv_obj_set_scrollable(stem, false);
    lv_obj_t *base = lv_obj_create(parent);
    lv_obj_remove_style_all(base);
    lv_obj_set_size(base, 42, 7);
    lv_obj_set_style_radius(base, 4, 0);
    lv_obj_set_style_bg_color(base, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_align(base, LV_ALIGN_CENTER, 0, 55);
    lv_obj_set_clickable(base, false);
    lv_obj_set_scrollable(base, false);
}

static bool button_event(button_id_t button, button_event_t event)
{
    if (button == BUTTON_BOOT && event == BUTTON_CLICK)
    {
        if (s_menu != NULL)
        {
            close_menu(NULL);
        }
        else
        {
            menu_event(NULL);
        }
        return true;
    }
    return false; /* Preserve long-press home and the power button. */
}

static void knob_event(lv_event_t *event)
{
    lv_port_knob_t *knob = lv_event_get_param(event);
    if (knob != NULL && knob->delta != 0)
    {
        if (s_menu == NULL)
        {
            menu_event(NULL);
        }
        else if (!active(s_snapshot.phase) && s_snapshot.count > 0)
        {
            s_selected = (s_selected + knob->delta % s_snapshot.count + s_snapshot.count) % s_snapshot.count;
            tick(NULL);
        }
        knob->handled = true;
    }
}

static bool back(void)
{
    if (s_menu != NULL)
    {
        close_menu(NULL);
        return true;
    }
    stop();
    return false;
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    s_screen = screen;
    s_selected = 0;
    s_held = false;
    s_locked = false;
    s_drawn_selected = -1;
    s_drawn_count = -1;
    s_menu = NULL;
    s_play_meter = NULL;
    s_drawn_phase = (voice_memo_phase_t)-1;
    s_drawn_second = UINT32_MAX;
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_hex(TK_VOID), 0);
    s_button = lv_button_create(screen);
    lv_obj_remove_style_all(s_button);
    lv_obj_set_size(s_button, ui_board_info()->width - 12, ui_board_info()->height - 20);
    lv_obj_set_pos(s_button, 6, 4);
    lv_obj_set_style_radius(s_button, ui_board_info()->round ? LV_RADIUS_CIRCLE : ui_board_info()->display_corner_radius, 0);
    lv_obj_set_style_bg_opa(s_button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_button, ui_glow(TK_RED, TK_GLOW_G2), 0);
    lv_obj_set_style_border_width(s_button, TK_LW_RING, 0);
    lv_obj_set_style_border_color(s_button, ui_glow(TK_RED, TK_GLOW_G1), 0);
    lv_obj_set_scrollable(s_button, false);
    lv_obj_set_gesture_bubble(s_button, false);
    lv_port_set_focusable(s_button);
    lv_obj_add_event_cb(s_button, record_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_button, record_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_button, record_event, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_button, record_event, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(screen, knob_event, lv_port_knob_event(), NULL);
    depth(s_button, 0);
    microphone_icon(s_button);
    s_meter = level_meter(s_button, s_levels);
    lv_obj_align(s_meter, LV_ALIGN_CENTER, 0, 90);
    s_lock = lv_label_create(s_button);
    lv_label_set_text(s_lock, "Registrazione bloccata");
    lv_obj_set_style_text_font(s_lock, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_lock, lv_color_white(), 0);
    lv_obj_align(s_lock, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_set_clickable(s_lock, false);
    s_time = lv_label_create(s_button);
    lv_obj_set_style_text_font(s_time, ui_font_title, 0);
    lv_obj_set_style_text_color(s_time, lv_color_white(), 0);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, 132);
    lv_obj_set_clickable(s_time, false);
    s_hint = lv_label_create(s_button);
    lv_obj_set_style_text_font(s_hint, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_white(), 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_set_clickable(s_hint, false);
    lv_obj_t *menu = ui_button(screen, "Memo", 0x890D09, menu_event, NULL);
    lv_obj_set_size(menu, 104, 42);
    lv_obj_align(menu, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_hidden(menu, ui_board_info()->has_buttons || ui_board_info()->has_knob);
    s_border = settings_get()->border_progress ? ui_border_progress_create(screen, 0xFFEADB) : NULL;
    voice_memo_scan();
    s_tick = lv_timer_create(tick, 100, NULL);
    tick(NULL);
}

static void destroy(void)
{
    stop();
    lv_anim_delete(s_button, depth);
    if (s_tick != NULL)
    {
        lv_timer_delete(s_tick);
        s_tick = NULL;
    }
    s_border = NULL;
    s_menu = NULL;
    s_play_meter = NULL;
}

const app_t voice_memo_app = {
    .id = "voice_memo", .name = "Memo vocali", .icon = LV_SYMBOL_AUDIO,
    .color = UI_COLOR_RED, .flags = APP_FLAG_NO_BACK_GESTURE,
    .create = create, .destroy = destroy, .pause = stop, .back = back, .button = button_event,
};
