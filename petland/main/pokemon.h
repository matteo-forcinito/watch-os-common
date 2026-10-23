#ifndef PETLAND_POKEMON_H
#define PETLAND_POKEMON_H
#include "../../components/apps/minigames/minigame_canvas.h"
typedef struct
{
    const uint8_t *pixels;
    const uint16_t *palette;
    uint8_t frames;
} pokemon_sprite_t;
const char *pokemon_name(int family, int form);
int pokemon_last_form(int family);
bool pokemon_draw(mg_canvas_t *canvas, int family, int form, int x, int foot_y, int scale, int frame, bool shiny);
#endif
