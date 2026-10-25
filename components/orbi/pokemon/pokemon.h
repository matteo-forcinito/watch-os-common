#ifndef PET_POKEMON_ASSETS_H
#define PET_POKEMON_ASSETS_H
#include <stdint.h>
/* Same generated Petland art, without depending on its RPG/canvas renderer. */
typedef struct
{
    const uint8_t *pixels;
    const uint16_t *palette;
    uint8_t frames;
} pokemon_sprite_t;
#endif
