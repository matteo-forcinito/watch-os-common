/* External RPG firmware: the SDK always arms a safe return to the OS.
 * The game uses the same tested timing, input and checkpoint host as minigames. */
#include "appkit_alarms.h"
#include "core/app.h"
#include "core/clock.h"
#include "core/devcon.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/sys.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "services/storage.h"
#include "ui/ui.h"
#include "../../components/apps/minigames/minigame_screen.h"
#include "../../components/apps/minigames/minigame_progress.h"
#include "../../components/apps/pet/pet_game_catalog.h"
#include <string.h>

extern const mg_game_t mg_petland;
extern const mg_game_t mg_wildbond;
static const app_t s_app;
static lv_obj_t *s_root, *s_screen;
static const mg_game_t *s_selected;
static lv_obj_t *s_choices[2];
static int s_menu_choice;
static void open_game(const mg_game_t *game);
size_t pet_game_count(void) { return 2; }
const mg_game_t *pet_game_at(size_t i) { return i == 0 ? &mg_petland : i == 1 ? &mg_wildbond : NULL; }
const mg_game_t *pet_game_find(const char *id)
{
    if (!id) return NULL;
    return !strcmp(id, "petland") ? &mg_petland : !strcmp(id, "wildbond") ? &mg_wildbond : NULL;
}
static void failed(void *arg) { (void)arg; ui_toast("Salvataggio fallito: riprova a uscire"); }
static void return_worker(void *arg)
{
    const mg_game_t *game = arg;
    if (game && mg_progress_failed(game)) { ui_async(failed, NULL); return; }
    extapp_return_to_launcher();
}
static void leave(void *user)
{
    if (!sys_post(return_worker, user)) ui_toast("Attendi e riprova a uscire");
}
static bool back(void)
{
    if (s_root && !mg_screen_checkpoint(s_root))
    {
        ui_toast("Attendi il salvataggio e riprova");
        return true;
    }
    leave((void *)s_selected);
    return true;
}
static void destroy(void) { s_root = s_screen = NULL; s_choices[0] = s_choices[1] = NULL; }
static bool button(button_id_t button, button_event_t event)
{
    if (!board_info()->has_knob || button != BUTTON_BOOT || event != BUTTON_CLICK) return false;
    if (s_root) return mg_screen_click(s_root);
    open_game(pet_game_at((size_t)s_menu_choice));
    return true;
}
static void open_game(const mg_game_t *game)
{
    s_selected = game;
    pet_look_t actor;
    pet_look(0x5045544C414E44ULL, 8, &actor);
    mg_screen_config_t config = {.owner = &s_app, .actor = actor, .exit = leave, .user = (void *)game};
    s_root = mg_screen_open(s_screen, s_selected, &config);
    if (!s_root) ui_toast("Memoria insufficiente");
}
static void choose(lv_event_t *event) { open_game(lv_event_get_user_data(event)); }
static void selector_knob(lv_event_t *event)
{
    if (s_root) return;
    lv_port_knob_t *knob = lv_event_get_param(event);
    s_menu_choice = knob->delta > 0 ? 1 : 0;
    knob->handled = true;
    for (int i = 0; i < 2; i++)
    {
        lv_obj_set_style_border_width(s_choices[i], i == s_menu_choice ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_choices[i], ui_color(0xC9FFE4), 0);
    }
    lv_obj_scroll_to_view(s_choices[s_menu_choice], LV_ANIM_OFF);
}
static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    s_screen = screen;
    s_menu_choice = 0;
    lv_obj_t *page = ui_page(screen, "Petland");
    ui_text(page, "Scegli la tua avventura", true);
    s_choices[0] = ui_button(page, "Cronache del Faro", 0x356652, choose, (void *)&mg_petland);
    ui_text(page, "Santuari, rovine, bottino e il mistero del faro.", true);
    s_choices[1] = ui_button(page, "Legami selvatici", 0x355F83, choose, (void *)&mg_wildbond);
    ui_text(page, "Cattura, allena ed evolvi una squadra di creature.", true);
    lv_obj_add_event_cb(screen, selector_knob, lv_port_knob_event(), NULL);
}
static const app_t s_app = {
    .id = "petland", .name = "Petland", .icon = LV_SYMBOL_HOME, .color = 0x497A66,
    .flags = APP_FLAG_KEEP_SCREEN_ON | APP_FLAG_STAY_ON_WAKE | APP_FLAG_NO_BACK_GESTURE,
    .create = create, .destroy = destroy, .back = back, .button = button};
void app_main(void)
{
    ESP_ERROR_CHECK(extapp_sdk_init());
    sys_worker_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    storage_init();
    mg_progress_init();
    ESP_ERROR_CHECK(lv_port_init());
    clock_init();
    appkit_alarms_prepare();
    lv_port_lock();
    ui_init();
    lv_obj_t *home = lv_obj_create(NULL);
    app_manager_set_home(home, back);
    app_register(&s_app);
    lv_port_unlock();
    ESP_ERROR_CHECK(power_init());
    app_manager_init();
    appkit_alarms_start();
    lv_port_lock();
    app_open_app(&s_app, NULL);
    lv_port_unlock();
    devcon_start();
    sys_heap_log("Petland ready");
}
