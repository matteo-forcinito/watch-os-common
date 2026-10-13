/* Dedicated input: no static plaintext result or truncated 96-byte buffers. */
#include "vault.h"
#include "core/app.h"
#include "ui/ui.h"
#include "hardware/board.h"
#include <stdio.h>
#include <string.h>

bool ui_round_keyboard_create(lv_obj_t *screen, lv_obj_t *textarea, const char *title);
typedef struct
{
    char title[48], initial[257];
    bool password;
    void (*callback)(const char *, void *);
    void *argument;
    lv_obj_t *textarea, *screen;
} input_t;

static lv_obj_t *vault_text(lv_obj_t *page, const char *text)
{
    return ui_text(page, text, true);
}

static input_t *s_input;
static const app_t s_input_app;

static void deleted(lv_event_t *event)
{
    input_t *input = lv_event_get_user_data(event);
    if (s_input == input)
        s_input = NULL;
    vault_secure_free(input);
}

static void input_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL)
        return;
    input_t *input = s_input;
    if (input == NULL)
        return;
    char result[257] = {0};
    const char *text = lv_textarea_get_text(input->textarea);
    if (code == LV_EVENT_READY && !vault_text_valid(text, sizeof(result), true))
    {
        ui_toast("Testo troppo lungo o non valido");
        return;
    }
    if (code == LV_EVENT_READY)
        memcpy(result, text, strlen(text) + 1);
    void (*callback)(const char *, void *) = input->callback;
    void *argument = input->argument;
    input->callback = NULL;
    vault_wipe(input->initial, sizeof(input->initial));
    lv_textarea_set_text(input->textarea, "");
    app_back();
    if (callback != NULL)
        callback(code == LV_EVENT_READY ? result : NULL, argument);
    vault_wipe(result, sizeof(result));
}

static void create(lv_obj_t *screen, void *argument)
{
    input_t *input = argument;
    s_input = input;
    if (input == NULL)
        return;
    input->screen = screen;
    lv_obj_add_event_cb(screen, deleted, LV_EVENT_DELETE, input);
    input->textarea = lv_textarea_create(screen);
    lv_textarea_set_one_line(input->textarea, true);
    lv_textarea_set_max_length(input->textarea, 256);
    lv_textarea_set_password_mode(input->textarea, input->password);
    lv_textarea_set_password_show_time(input->textarea, 0);
    lv_textarea_set_text(input->textarea, input->initial);
    vault_wipe(input->initial, sizeof(input->initial));
    lv_obj_set_style_bg_color(input->textarea, ui_color(UI_COLOR_CARD), 0);
    lv_obj_set_style_border_width(input->textarea, 0, 0);
    lv_obj_add_state(input->textarea, LV_STATE_FOCUSED);
    if (board_info()->round)
    {
        lv_obj_add_event_cb(screen, input_event, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(screen, input_event, LV_EVENT_CANCEL, NULL);
        if (!ui_round_keyboard_create(screen, input->textarea, input->title))
            app_back();
    }
    else
    {
        lv_obj_set_size(input->textarea, LV_PCT(92), 64);
        lv_obj_align(input->textarea, LV_ALIGN_TOP_MID, 0, 56);
        lv_obj_t *label = lv_label_create(screen);
        lv_label_set_text(label, input->title);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 16);
        lv_obj_t *keyboard = lv_keyboard_create(screen);
        lv_obj_set_size(keyboard, LV_PCT(100), LV_PCT(70));
        lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_keyboard_set_textarea(keyboard, input->textarea);
        lv_obj_add_event_cb(keyboard, input_event, LV_EVENT_ALL, NULL);
    }
}

static bool back(void)
{
    if (s_input != NULL)
    {
        void (*callback)(const char *, void *) = s_input->callback;
        s_input->callback = NULL;
        vault_wipe(s_input->initial, sizeof(s_input->initial));
        if (s_input->textarea != NULL)
            lv_textarea_set_text(s_input->textarea, "");
        void *argument = s_input->argument;
        s_input = NULL;
        if (callback != NULL)
            callback(NULL, argument);
    }
    return false;
}

static const app_t s_input_app = {.id = "vault.input",
                                  .name = "Inserimento",
                                  .icon = LV_SYMBOL_KEYBOARD,
                                  .flags = APP_FLAG_HIDDEN | APP_FLAG_NO_BACK_GESTURE,
                                  .create = create,
                                  .back = back};

void vault_input_init(void)
{
    app_register(&s_input_app);
}

void vault_input_cancel(void)
{
    if (s_input == NULL)
        return;
    s_input->callback = NULL;
    vault_wipe(s_input->initial, sizeof(s_input->initial));
    if (s_input->textarea != NULL)
        lv_textarea_set_text(s_input->textarea, "");
    if (app_current() == &s_input_app)
        app_back();
}

static void close_input(lv_event_t *event)
{
    (void)event;
    app_back();
}

void vault_input_scrub(void)
{
    if (s_input == NULL)
        return;
    s_input->callback = NULL;
    vault_wipe(s_input->initial, sizeof(s_input->initial));
    if (s_input->screen != NULL)
    {
        lv_obj_clean(s_input->screen);
        s_input->textarea = NULL;
        lv_obj_t *page = ui_page(s_input->screen, "Archivio bloccato");
        vault_text(page, "Riapri Vault per inserire credenziali.");
        ui_button(page, "Chiudi", UI_COLOR_CARD_HI, close_input, NULL);
    }
}

void vault_input(const char *title, const char *initial, bool password,
                 void (*callback)(const char *, void *), void *argument)
{
    if (s_input != NULL || !vault_text_valid(initial, 257, true))
    {
        if (callback != NULL)
            callback(NULL, argument);
        return;
    }
    input_t *input = vault_secure_alloc(sizeof(*input));
    if (input == NULL)
    {
        if (callback != NULL)
            callback(NULL, argument);
        return;
    }
    memset(input, 0, sizeof(*input));
    snprintf(input->title, sizeof(input->title), "%s", title);
    memcpy(input->initial, initial, strlen(initial) + 1);
    input->password = password;
    input->callback = callback;
    input->argument = argument;
    if (!app_open_app(&s_input_app, input))
    {
        vault_secure_free(input);
        if (callback != NULL)
            callback(NULL, argument);
    }
}
