#include "ui/ui.h"

#include "core/lv_port.h"
#include "core/recovery.h"

#include <string.h>

/*
 * Boot splash (NEON NOIR), on lv_layer_top() so the home is built under it
 * without knowing about it.
 *
 *     t = 0      a cyan line opens from the centre          (line, 2 px)
 *     250 ms     "WATCH OS_" is typed, 40 ms per letter      (~300 x 40 px)
 *                boot log: DISPLAY OK / CORE OK / LINK OK    (one small box)
 *     home up    2 glitch frames (magenta copy, 2 px off), the line closes
 *                to the centre and the splash is deleted
 *
 * Every step touches a small area; the only full redraw is the final
 * deletion, which the first frame of the home pays anyway. While app_main
 * holds the LVGL lock (ui_init / apps_init) the timer cannot run: the
 * splash simply holds still, so the boot is never delayed by it.
 */

#define SPLASH_WORD        "WATCH OS"
#define SPLASH_LINE_MS     250
#define SPLASH_CHAR_MS     40
#define SPLASH_MIN_MS      600    /* shorter would read as a flash */
#define SPLASH_GLITCH_TICKS 2

typedef enum
{
    SPLASH_INTRO,
    SPLASH_GLITCH,
    SPLASH_OUTRO,
} splash_phase_t;

typedef struct
{
    lv_obj_t *cover;
    lv_obj_t *line;
    lv_obj_t *word;
    lv_obj_t *ghost;
    lv_obj_t *names;
    lv_obj_t *status;
    lv_obj_t *boot_screen;
    lv_timer_t *timer;
    uint32_t start;
    int typed;
    int log_lines;
    int glitch_ticks;
    bool cursor_on;
    splash_phase_t phase;
    int32_t line_width;
} splash_t;

static splash_t s_splash;

static void set_width(void *obj, int32_t width)
{
    lv_obj_set_width(obj, width < 1 ? 1 : width);
}

static void splash_deleted(lv_anim_t *a)
{
    (void)a;

    if (s_splash.cover != NULL)
    {
        lv_obj_delete(s_splash.cover);
    }

    memset(&s_splash, 0, sizeof(s_splash));
}

static void line_anim(int32_t from, int32_t to, uint32_t ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_splash.line);
    lv_anim_set_exec_cb(&a, set_width);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static void set_log(int lines)
{
    static const char *const names[] = {"DISPLAY", "DISPLAY\nCORE", "DISPLAY\nCORE\nLINK"};
    static const char *const status[] = {"OK", "OK\nOK", "OK\nOK\nOK"};

    if (lines == s_splash.log_lines || lines < 1 || lines > 3)
    {
        return;
    }

    s_splash.log_lines = lines;
    lv_label_set_text_static(s_splash.names, names[lines - 1]);
    lv_label_set_text_static(s_splash.status, status[lines - 1]);
}

static void set_typed(int typed, bool cursor)
{
    if (typed == s_splash.typed && cursor == s_splash.cursor_on)
    {
        return;   /* unchanged: no redraw */
    }

    char text[sizeof(SPLASH_WORD) + 1];
    memcpy(text, SPLASH_WORD, (size_t)typed);
    text[typed] = cursor ? '_' : ' ';
    text[typed + 1] = '\0';
    s_splash.typed = typed;
    s_splash.cursor_on = cursor;
    lv_label_set_text(s_splash.word, text);
}

static void splash_tick(lv_timer_t *timer)
{
    uint32_t elapsed = lv_tick_elaps(s_splash.start);
    /* Cap: never leave the touch-blocking cover up if the home never loads. */
    bool home_up = lv_screen_active() != s_splash.boot_screen || elapsed > 5000;
    const int word_len = (int)strlen(SPLASH_WORD);

    switch (s_splash.phase)
    {
        case SPLASH_INTRO:
        {
            int typed = elapsed < SPLASH_LINE_MS ? 0 : (int)((elapsed - SPLASH_LINE_MS) / SPLASH_CHAR_MS);
            typed = typed > word_len ? word_len : typed;
            set_typed(typed, typed < word_len || (elapsed / (TK_T_BLINK / 2)) % 2 == 0);

            if (home_up)
            {
                set_log(2);   /* the UI is built: a real milestone, not a fake bar */
            }

            if (home_up && typed == word_len && elapsed >= SPLASH_MIN_MS)
            {
                set_log(3);
                set_typed(word_len, false);
                lv_obj_set_hidden(s_splash.ghost, false);
                s_splash.phase = SPLASH_GLITCH;
            }
            break;
        }

        case SPLASH_GLITCH:
            /* Two frames of chromatic offset, then the logo collapses into the line. */
            if (++s_splash.glitch_ticks >= SPLASH_GLITCH_TICKS)
            {
                lv_obj_set_hidden(s_splash.ghost, true);
                lv_obj_set_hidden(s_splash.word, true);
                lv_obj_set_hidden(lv_obj_get_parent(s_splash.names), true);
                s_splash.phase = SPLASH_OUTRO;
                s_splash.timer = NULL;
                lv_timer_delete(timer);
                line_anim(s_splash.line_width, 0, TK_T_PAGE, lv_anim_path_ease_in, splash_deleted);
            }
            break;

        case SPLASH_OUTRO:
            break;
    }
}

void ui_splash_show(void)
{
    if (recovery_active() || lv_port_display() == NULL || s_splash.cover != NULL)
    {
        return;
    }

    lv_port_lock();

    const board_info_t *board = ui_board_info();
    s_splash.boot_screen = lv_screen_active();
    s_splash.start = lv_tick_get();
    s_splash.line_width = board->width * 3 / 5;
    s_splash.typed = -1;

    /* Opaque black cover; clickable so early taps do not reach the home below. */
    lv_obj_t *cover = lv_obj_create(lv_layer_top());
    s_splash.cover = cover;
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cover, lv_color_hex(TK_VOID), 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_clickable(cover, true);
    lv_obj_set_scrollable(cover, false);

    s_splash.line = lv_obj_create(cover);
    lv_obj_remove_style_all(s_splash.line);
    lv_obj_set_size(s_splash.line, 1, TK_LW);
    lv_obj_set_style_bg_color(s_splash.line, lv_color_hex(TK_CYAN), 0);
    lv_obj_set_style_bg_opa(s_splash.line, LV_OPA_COVER, 0);
    lv_obj_align(s_splash.line, LV_ALIGN_CENTER, 0, TK_SP_6);

    /* Glitch copy first, so the real wordmark draws over it. */
    s_splash.ghost = lv_label_create(cover);
    lv_label_set_text_static(s_splash.ghost, SPLASH_WORD " ");
    lv_obj_set_style_text_font(s_splash.ghost, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_letter_space(s_splash.ghost, 6, 0);
    lv_obj_set_style_text_color(s_splash.ghost, lv_color_hex(TK_MAGENTA), 0);
    lv_obj_align(s_splash.ghost, LV_ALIGN_CENTER, TK_LW + 1, -TK_SP_3 + 1);
    lv_obj_set_hidden(s_splash.ghost, true);

    s_splash.word = lv_label_create(cover);
    lv_obj_set_style_text_font(s_splash.word, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_letter_space(s_splash.word, 6, 0);
    lv_obj_set_style_text_color(s_splash.word, lv_color_hex(TK_TEXT), 0);
    lv_obj_set_width(s_splash.word, LV_SIZE_CONTENT);
    lv_obj_align(s_splash.word, LV_ALIGN_CENTER, 0, -TK_SP_3);
    set_typed(0, true);

    /* Boot log: names on the left (dim), status on the right (lime). */
    lv_obj_t *log = lv_obj_create(cover);
    lv_obj_remove_style_all(log);
    lv_obj_set_size(log, board->width * 2 / 5, LV_SIZE_CONTENT);
    lv_obj_align(log, LV_ALIGN_CENTER, 0, board->height / 5);
    lv_obj_set_scrollable(log, false);

    s_splash.names = lv_label_create(log);
    lv_obj_set_style_text_font(s_splash.names, UI_FONT_MICRO, 0);
    lv_obj_set_style_text_letter_space(s_splash.names, UI_MICRO_LETTER_SPACE, 0);
    lv_obj_set_style_text_color(s_splash.names, lv_color_hex(TK_TEXT_3), 0);
    lv_obj_align(s_splash.names, LV_ALIGN_TOP_LEFT, 0, 0);

    s_splash.status = lv_label_create(log);
    lv_obj_set_style_text_font(s_splash.status, UI_FONT_MICRO, 0);
    lv_obj_set_style_text_letter_space(s_splash.status, UI_MICRO_LETTER_SPACE, 0);
    lv_obj_set_style_text_color(s_splash.status, lv_color_hex(TK_LIME), 0);
    lv_obj_set_style_text_align(s_splash.status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_splash.status, LV_ALIGN_TOP_RIGHT, 0, 0);
    set_log(1);

    line_anim(1, s_splash.line_width, SPLASH_LINE_MS, lv_anim_path_ease_out, NULL);
    s_splash.timer = lv_timer_create(splash_tick, TK_T_LIVE_TICK, NULL);

    lv_port_unlock();
}
