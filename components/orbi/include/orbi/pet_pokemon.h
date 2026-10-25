#ifndef ORBI_PET_POKEMON_H
#define ORBI_PET_POKEMON_H
#include <stdbool.h>
#include <stdint.h>

void pet_pokemon_init(void);
int pet_pokemon_count(void);
uint16_t pet_pokemon_dex(int index);
const char *pet_pokemon_name(uint16_t dex);
/* Separate versioned NVS sidecar. Set from a worker, get only reads RAM.
 * dex=0 restores original art. No change to pet identities or care records. */
bool pet_pokemon_set(uint64_t seed, uint16_t dex, bool shiny);
void pet_pokemon_get(uint64_t seed, uint16_t *dex, bool *shiny);
bool pet_pokemon_palette(uint16_t dex, bool shiny, uint32_t out[256]);
bool pet_pokemon_draw(uint16_t dex, bool shiny, uint32_t frame, uint8_t out[1024]);
#endif
