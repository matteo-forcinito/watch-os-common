#ifndef MINIGAME_PROGRESS_H
#define MINIGAME_PROGRESS_H
#include "minigame.h"
/* Initialize after board/storage mount. Boot reads files/NVS once; UI only
 * copies the PSRAM cache. Writes and legacy migration run on sys.
 * Checkpoints belong to this device, independently of the selected actor. */
void mg_progress_init(void);
bool mg_progress_load(const mg_game_t *game, void *state);
bool mg_progress_save(const mg_game_t *game, const void *state);
bool mg_progress_failed(const mg_game_t *game);
#endif
