/* The pet page supplies only the selected actor and the reward policy.
 * The common minigame host owns the complete game screen and its lifecycle. */
#include "pet_game_catalog.h"
#include "../minigames/minigame_screen.h"
#include "core/sys.h"

static lv_obj_t *s_game;
static EXT_RAM_BSS_ATTR char s_game_seed[17];
typedef struct
{
    char seed[17], game[32], error[80];
    int score;
} game_reward_job_t;

static void reward_error(void *arg)
{
    game_reward_job_t *job = arg;
    ui_toast(job->error);
    free(job);
}
static void reward_worker(void *arg)
{
    game_reward_job_t *job = arg;
    cJSON *j = cJSON_CreateObject();
    if (j)
    {
        cJSON_AddStringToObject(j, "action", "game");
        cJSON_AddStringToObject(j, "seed", job->seed);
        cJSON_AddStringToObject(j, "game", job->game);
        cJSON_AddNumberToObject(j, "score", job->score);
    }
    const char *error = NULL;
    cJSON *out = j ? pet_command(j, &error) : NULL;
    if (!out) snprintf(job->error, sizeof(job->error), "Premio non salvato: %s", error ? error : "memoria insufficiente");
    cJSON_Delete(out);
    cJSON_Delete(j);
    if (job->error[0])
    {
        ESP_LOGW(TAG, "%s", job->error);
        if (ui_async(reward_error, job)) return;
    }
    else ESP_LOGI(TAG, "game reward saved: %s score %d", job->game, job->score);
    free(job);
}
static bool game_reward(const char *id, int score, void *user)
{
    game_reward_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) return false;
    snprintf(job->seed, sizeof(job->seed), "%s", (const char *)user);
    snprintf(job->game, sizeof(job->game), "%s", id);
    job->score = score;
    if (sys_post(reward_worker, job)) return true;
    free(job);
    return false;
}
static void game_close(void *user)
{
    (void)user;
    menu_close(NULL);
    sync_pets(false);
    hud_update();
}
static bool game_button(button_id_t button, button_event_t event)
{
    return board_info()->has_knob && button == BUTTON_BOOT && event == BUTTON_CLICK && mg_screen_click(s_game);
}
static bool game_back(void)
{
    if (!S.menu) return false;
    game_close(NULL);
    return true;
}
static void game_open(lv_event_t *e)
{
    const mg_game_t *game = lv_event_get_user_data(e);
    actor_t *a = selected_actor();
    if (!a || a->rec.asleep || pet_travel_busy())
    {
        ui_toast("Sveglia il pet con la luna e attendi la fine dei viaggi");
        return;
    }
    pet_record_t pet = a->rec;
    menu_close(NULL);
    snprintf(s_game_seed, sizeof(s_game_seed), "%s", pet.seed);
    mg_screen_config_t config = {.owner = &pet_app, .actor = look_of(&pet), .exit = game_close,
                                 .reward = game_reward, .user = s_game_seed};
    s_game = mg_screen_open(S.screen, game, &config);
    S.menu = s_game;
    if (!s_game) ui_toast("Memoria insufficiente per il gioco");
}
static void game_menu(lv_event_t *e)
{
    menu_close(e);
    S.menu = box(S.screen, 0x0C1523, LV_OPA_COVER);
    lv_obj_set_clickable(S.menu, true);
    lv_obj_set_size(S.menu, S.W, S.H);
    lv_obj_t *page = ui_page(S.menu, "Minigiochi");
    for (size_t i = 0; i < pet_game_count(); i++)
    {
        const mg_game_t *game = pet_game_at(i);
        ui_button(page, game->title, 0x1F4E5F, game_open, (void *)game);
        ui_text(page, game->instructions, true);
    }
    ui_button(page, "Chiudi", 0x24415C, menu_close, NULL);
}
