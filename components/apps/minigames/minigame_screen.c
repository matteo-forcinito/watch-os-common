/* Shared LVGL host. The game's own code never touches widgets or memory
 * allocation. All allocations happen here at open, and all are owned by root. */
#include "minigame_screen.h"
#include "minigame_canvas.h"
#include "minigame_progress.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/state.h"
#include "hardware/board.h"
#include "ui/ui.h"
#include "lvgl_private.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "minigame";
typedef struct
{
    mg_screen_config_t config;
    const mg_game_t *game;
    mg_session_t *session;
    mg_canvas_t canvas;
    lv_draw_buf_t *buffer;
    lv_obj_t *root, *parent, *image, *hud, *hint, *feedback, *result, *retry, *pad;
    lv_timer_t *timer;
    uint8_t grid[PET_GRID * PET_GRID];
    uint16_t colors[PET_COLORS];
    char hud_text[MG_HUD_BYTES];
    char feedback_text[128];
    char hint_text[128];
    int actor_variant;
    int frames, animation, report_at, hud_at;
    int64_t began, refresh_start, refresh_sum, refresh_max, tick_sum;
    unsigned refresh_count;
    uint64_t checkpoint_at;
    bool was_active;
} mg_screen_t;

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }
static bool active(const mg_screen_t *s)
{
    return power_screen() == POWER_SCREEN_ON && app_current() == s->config.owner && lv_obj_is_visible(s->root);
}
static lv_obj_t *box(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, ui_color(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}
static lv_obj_t *label(mg_screen_t *s, const char *text, int y, bool title)
{
    lv_obj_t *o = lv_label_create(s->root);
    lv_obj_set_width(o, s->canvas.screen_width * 78 / 100);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(o, ui_color(0xFFFFFF), 0);
    lv_obj_set_style_text_font(o, title ? ui_font_title : UI_FONT_SMALL, 0);
    lv_label_set_text(o, text);
    lv_obj_align(o, LV_ALIGN_TOP_MID, 0, y);
    return o;
}
static void refresh(lv_event_t *e)
{
    mg_screen_t *s = lv_event_get_user_data(e);
    if (!active(s) || mg_done(s->session)) return;
    int64_t now = esp_timer_get_time();
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) s->refresh_start = now;
    else if (s->refresh_start)
    {
        int64_t cost = now - s->refresh_start;
        s->refresh_sum += cost;
        if (cost > s->refresh_max) s->refresh_max = cost;
        s->refresh_count++;
        s->refresh_start = 0;
    }
}
static void screen_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)subject;
    mg_screen_t *s = lv_observer_get_user_data(observer);
    /* The observer runs even when LVGL's timers are stopped for screen-off. */
    mg_advance(s->session, now_ms(), false);
    if (s->game->continuous) mg_progress_save(s->game, mg_state(s->session));
}
static void touch(lv_event_t *e)
{
    mg_screen_t *s = lv_event_get_user_data(e);
    if (!active(s)) return;
    if (lv_event_get_code(e) == LV_EVENT_PRESS_LOST)
    {
        /* Losing the pad cancels a gesture; it must never confirm a purchase. */
        mg_advance(s->session, now_ms(), false);
        return;
    }
    lv_point_t point = {0};
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &point);
    int x = point.x * 1000 / s->canvas.width;
    int y = (point.y - s->canvas.top) * 1000 / s->canvas.height;
    mg_input(s->session, lv_event_get_code(e) == LV_EVENT_PRESSED ? MG_PRESS :
             lv_event_get_code(e) == LV_EVENT_PRESSING ? MG_MOVE : MG_RELEASE,
             x, y, x < 500 ? -1 : 1);
}
static void knob(lv_event_t *e)
{
    mg_screen_t *s = lv_event_get_user_data(e);
    if (!active(s) || mg_done(s->session)) return;
    lv_port_knob_t *input = lv_event_get_param(e);
    input->handled = true;
    mg_input(s->session, MG_TURN, 0, 0, input->delta);
}
bool mg_screen_click(lv_obj_t *root)
{
    mg_screen_t *s = root ? lv_obj_get_user_data(root) : NULL;
    if (!s || !active(s) || mg_done(s->session)) return false;
    mg_input(s->session, MG_CLICK, 0, 0, 0);
    return true;
}
bool mg_screen_checkpoint(lv_obj_t *root)
{
    mg_screen_t *s = root ? lv_obj_get_user_data(root) : NULL;
    return s && (!s->game->continuous || mg_progress_save(s->game, mg_state(s->session)));
}
static void leave(lv_event_t *e)
{
    mg_screen_t *s = lv_event_get_user_data(e);
    if (s->game->continuous && !mg_progress_save(s->game, mg_state(s->session)))
    {
        ui_toast("Salvataggio in attesa: riprova");
        return;
    }
    if (s->config.exit) s->config.exit(s->config.user);
}
static void retry(lv_event_t *e)
{
    mg_screen_t *s = lv_event_get_user_data(e);
    mg_restart(s->session, esp_random(), now_ms());
    s->began = esp_timer_get_time();
    s->animation = s->hud_at = s->report_at = -1;
    s->frames = 0;
    s->tick_sum = s->refresh_sum = s->refresh_max = s->refresh_start = 0;
    s->refresh_count = 0;
    s->feedback_text[0] = 0;
    lv_obj_set_hidden(s->result, true);
    lv_obj_set_hidden(s->retry, true);
    lv_obj_set_hidden(s->hint, false);
    lv_obj_set_hidden(s->feedback, false);
    lv_obj_set_hidden(s->pad, false);
    lv_timer_ready(s->timer);
}
static void finish(mg_screen_t *s)
{
    mg_result_t result;
    if (!mg_take_result(s->session, &result)) return;
    bool queued = s->config.reward && s->config.reward(s->game->id, result.score, s->config.user);
    lv_label_set_text_fmt(s->result, "%s\n%d / 100\n%s", result.won ? (s->game->win_text ? s->game->win_text : "Bravo!") : s->game->lose_text ? s->game->lose_text : "Partita finita!",
                          result.score, queued ? "Premio in salvataggio" : "Premio non salvato");
    lv_obj_set_hidden(s->result, false);
    lv_obj_set_hidden(s->retry, false);
    lv_obj_set_hidden(s->hint, true);
    lv_obj_set_hidden(s->feedback, true);
    lv_obj_set_hidden(s->pad, true);
    ESP_LOGI(TAG, "%s finished: score %d frames %d", s->game->id, result.score, s->frames);
}
static void tick(lv_timer_t *timer)
{
    mg_screen_t *s = lv_timer_get_user_data(timer);
    int64_t started = esp_timer_get_time();
    bool visible = active(s);
    mg_advance(s->session, (uint64_t)(started / 1000), visible);
    if (!visible && s->was_active && s->game->continuous)
        mg_progress_save(s->game, mg_state(s->session));
    s->was_active = visible;
    if (!visible || mg_done(s->session))
    {
        if (visible) finish(s);
        return;
    }
    int elapsed = mg_elapsed(s->session);
    if (s->game->continuous && now_ms() - s->checkpoint_at >= 30000)
    {
        if (mg_progress_save(s->game, mg_state(s->session))) s->checkpoint_at = now_ms();
        else s->checkpoint_at = now_ms() - 25000;
    }
    if (s->frames++ % 15 == 0) power_user_activity();
    pet_look_t actor = s->config.actor;
    int variant = s->game->actor_variant ? s->game->actor_variant(mg_state(s->session)) : 0;
    variant = variant < 0 ? 0 : variant % 256;
    int count = pet_variant_count(actor.species);
    if (count > 0) actor.variant = (uint8_t)((actor.variant + variant) % count);
    if (s->actor_variant != variant)
    {
        uint32_t colors[PET_COLORS];
        pet_palette(&actor, 1, colors);
        for (int i = 0; i < PET_COLORS; i++) s->colors[i] = mg_color(colors[i]);
        s->actor_variant = variant;
        s->animation = -1;
    }
    if (s->animation != elapsed / 100)
    {
        s->animation = elapsed / 100;
        pet_draw(&actor, s->game->actor_walk ? PET_POSE_WALK : PET_POSE_HAPPY,
                 (uint32_t)s->animation, false, s->grid);
    }
    mg_canvas_begin(&s->canvas);
    s->game->draw(mg_state(s->session), &s->canvas);
    if (s->canvas.full) lv_obj_invalidate(s->image);
    else
    {
        lv_area_t origin;
        lv_obj_get_coords(s->image, &origin);
        for (int i = 0; i < s->canvas.dirty_count; i++)
        {
            mg_rect_t *r = &s->canvas.dirty[i];
            lv_area_t area = {origin.x1 + r->x1, origin.y1 + r->y1, origin.x1 + r->x2, origin.y1 + r->y2};
            lv_obj_invalidate_area(s->image, &area);
        }
    }
    if (s->hud_at != elapsed / 200)
    {
        s->hud_at = elapsed / 200;
        if (s->game->hud) s->game->hud(mg_state(s->session), s->hud_text);
        else snprintf(s->hud_text, sizeof(s->hud_text), "%ds", (MG_MAX_DURATION_MS - elapsed + 999) / 1000);
        s->hud_text[MG_HUD_BYTES - 1] = 0;
        lv_label_set_text_static(s->hud, s->hud_text);
        const char *help = s->game->help ? s->game->help(mg_state(s->session)) : s->game->instructions;
        if (help && strcmp(help, s->hint_text))
        {
            snprintf(s->hint_text, sizeof(s->hint_text), "%s", help);
            lv_label_set_text_static(s->hint, s->hint_text);
        }
    }
    const char *feedback = s->game->feedback ? s->game->feedback(mg_state(s->session)) : "";
    if (s->game->continuous && mg_progress_failed(s->game)) feedback = "Salvataggio fallito: riprovo";
    if (!feedback) feedback = "";
    if (strcmp(feedback, s->feedback_text))
    {
        snprintf(s->feedback_text, sizeof(s->feedback_text), "%s", feedback);
        lv_label_set_text_static(s->feedback, s->feedback_text);
    }
    s->tick_sum += esp_timer_get_time() - started;
    if (elapsed / 5000 != s->report_at)
    {
        s->report_at = elapsed / 5000;
        if (elapsed >= 5000)
            ESP_LOGI(TAG, "%s perf: wall %d sim %d frames %d refresh %u avg %d us max %d us tick %d us",
                     s->game->id, (int)((started - s->began) / 1000), elapsed, s->frames, s->refresh_count,
                     (int)(s->refresh_sum / (s->refresh_count ? s->refresh_count : 1)), (int)s->refresh_max,
                     (int)(s->tick_sum / s->frames));
    }
}
static void deleted(lv_event_t *e)
{
    mg_screen_t *s = lv_event_get_user_data(e);
    if (s->game->continuous && !mg_progress_save(s->game, mg_state(s->session)))
        ESP_LOGW(TAG, "%s final checkpoint not queued", s->game->id);
    if (s->timer) lv_timer_delete(s->timer);
    lv_obj_remove_event_cb_with_user_data(s->parent, knob, s);
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), refresh, s);
    lv_image_set_src(s->image, NULL);
    lv_image_cache_drop(s->buffer);
    lv_draw_buf_destroy(s->buffer);
    mg_destroy(s->session);
    lv_obj_set_user_data(s->root, NULL);
    free(s);
}
lv_obj_t *mg_screen_open(lv_obj_t *parent, const mg_game_t *game, const mg_screen_config_t *config)
{
    int W = ui_board_info()->width, H = ui_board_info()->height;
    if (!parent || !game || !config || !config->owner || !config->exit || W <= 0 || W > 512 || H <= 0 || H > 512) return NULL;
    mg_screen_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) return NULL;
    s->config = *config;
    s->actor_variant = -1;
    s->game = game;
    s->parent = parent;
    s->session = mg_create(game, esp_random(), now_ms());
    s->buffer = lv_draw_buf_create(W, H * 45 / 100, LV_COLOR_FORMAT_RGB565_SWAPPED, 0);
    if (!s->session || !s->buffer)
    {
        if (s->buffer) lv_draw_buf_destroy(s->buffer);
        mg_destroy(s->session);
        free(s);
        return NULL;
    }
    if (game->continuous && mg_progress_load(game, mg_state(s->session)))
        ESP_LOGI(TAG, "%s resumed checkpoint", game->id);
    s->checkpoint_at = now_ms();
    uint32_t palette[PET_COLORS];
    pet_palette(&config->actor, 1, palette);
    for (int i = 0; i < PET_COLORS; i++) s->colors[i] = mg_color(palette[i]);
    s->canvas = (mg_canvas_t){.pixels = s->buffer->data, .width = W, .height = H * 45 / 100,
                              .stride = (int)s->buffer->header.stride, .screen_width = W, .screen_height = H,
                              .top = H * 28 / 100, .actor_grid = s->grid, .actor_palette = s->colors,
                              .actor_size = PET_GRID * (W >= 400 ? 2 : 1), .actor_colors = PET_COLORS};
    mg_fill_pixels(&s->canvas, game->sky);
    s->root = box(parent, game->sky);
    lv_obj_set_user_data(s->root, s);
    lv_obj_set_size(s->root, W, H);
    lv_obj_set_clickable(s->root, true);
    lv_obj_t *ground = box(s->root, game->ground);
    lv_obj_set_size(ground, W, H - H * 73 / 100);
    lv_obj_set_pos(ground, 0, H * 73 / 100);
    s->image = lv_image_create(s->root);
    lv_image_set_src(s->image, s->buffer);
    lv_obj_set_pos(s->image, 0, s->canvas.top);
    label(s, game->title, H / 12, true);
    s->hud = label(s, "Pronti?", H / 12 + 28, false);
    s->feedback = label(s, "", H * 23 / 100, false);
    s->hint = label(s, game->instructions ? game->instructions : "", H * 75 / 100, false);
    /* Choose legible footer text on both snow and dark game backgrounds. */
    if (((game->ground >> 16) & 255) * 299 + ((game->ground >> 8) & 255) * 587 + (game->ground & 255) * 114 > 128000) lv_obj_set_style_text_color(s->hint, ui_color(0x102C40), 0);
    s->pad = box(s->root, 0);
    lv_obj_set_style_bg_opa(s->pad, LV_OPA_TRANSP, 0);
    lv_obj_set_size(s->pad, W * 84 / 100, H * 55 / 100);
    lv_obj_set_pos(s->pad, W * 8 / 100, H / 5);
    lv_obj_set_clickable(s->pad, true);
    lv_obj_add_event_cb(s->pad, touch, LV_EVENT_PRESSED, s);
    lv_obj_add_event_cb(s->pad, touch, LV_EVENT_PRESSING, s);
    lv_obj_add_event_cb(s->pad, touch, LV_EVENT_RELEASED, s);
    lv_obj_add_event_cb(s->pad, touch, LV_EVENT_PRESS_LOST, s);
    s->result = label(s, "", H * 33 / 100, true);
    lv_obj_set_style_bg_color(s->result, ui_color(0x101E31), 0);
    lv_obj_set_style_bg_opa(s->result, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s->result, 12, 0);
    s->retry = ui_button(s->root, "Riprova", 0x245544, retry, s);
    lv_obj_set_width(s->retry, W / 3);
    lv_obj_align(s->retry, LV_ALIGN_TOP_MID, 0, H * 65 / 100);
    lv_obj_t *exit = ui_button(s->root, game->continuous ? "Salva ed esci" : "Esci", 0x24415C, leave, s);
    lv_obj_set_width(exit, game->continuous ? W / 2 : W / 3);
    lv_obj_align(exit, LV_ALIGN_TOP_MID, 0, H * 82 / 100);
    lv_obj_set_hidden(s->result, true);
    lv_obj_set_hidden(s->retry, true);
    lv_obj_add_event_cb(s->root, deleted, LV_EVENT_DELETE, s);
    lv_obj_add_event_cb(parent, knob, lv_port_knob_event(), s);
    lv_display_add_event_cb(lv_display_get_default(), refresh, LV_EVENT_REFR_START, s);
    lv_display_add_event_cb(lv_display_get_default(), refresh, LV_EVENT_REFR_READY, s);
    lv_subject_add_observer_obj(state_subject(STATE_SCREEN), screen_changed, s->root, s);
    s->animation = s->hud_at = s->report_at = -1;
    s->began = esp_timer_get_time();
    s->timer = lv_timer_create(tick, 33, s);
    lv_timer_ready(s->timer);
    power_user_activity();
    ESP_LOGI(TAG, "%s begin: %dx%d state %u bytes", game->id, W, H, (unsigned)game->state_size);
    return s->root;
}
