#ifndef PET_GAME_CATALOG_H
#define PET_GAME_CATALOG_H
#include "pet_game.h"
#include "../minigames/minigame.h"
size_t pet_game_count(void);
const mg_game_t *pet_game_at(size_t index);
const mg_game_t *pet_game_find(const char *id);
#endif
