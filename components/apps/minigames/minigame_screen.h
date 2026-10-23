#ifndef MINIGAME_SCREEN_H
#define MINIGAME_SCREEN_H
#include "minigame.h"
#include "core/app.h"
#include "orbi/pet_sprite.h"
#include "lvgl.h"
/* Open on the LVGL task. Deleting the root also frees state, framebuffer,
 * timer and display/input callbacks. Exit asks the owning app to close it. */
typedef struct
{
    const app_t *owner;
    pet_look_t actor;
    void (*exit)(void *user);
    bool (*reward)(const char *game_id, int score, void *user);
    void *user;
} mg_screen_config_t;
lv_obj_t *mg_screen_open(lv_obj_t *parent, const mg_game_t *game, const mg_screen_config_t *config);
bool mg_screen_click(lv_obj_t *root);
bool mg_screen_checkpoint(lv_obj_t *root);
#endif
