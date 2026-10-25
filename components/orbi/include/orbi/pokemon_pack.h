#ifndef ORBI_POKEMON_PACK_H
#define ORBI_POKEMON_PACK_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* File-backed Petland art. Call open/read on a storage worker, never per UI tick.
 * Caller owns the handle and decoded frame cache, preferably in PSRAM.
 * A failed read leaves the output unchanged, so callers can retain old art. */
typedef struct { FILE *file; uint32_t bytes, data_offset; uint16_t count; } pokemon_pack_t;
bool pokemon_pack_open(pokemon_pack_t *pack, const char *path);
void pokemon_pack_close(pokemon_pack_t *pack);
/* National dex ID, not the RPG family index. Output is 32x32 ARGB8888,
 * transparent background, nearest-neighbour fit of Petland's 40x40 frame. */
bool pokemon_pack_read(pokemon_pack_t *pack, uint16_t dex, bool shiny,
                       uint32_t frame, uint32_t argb[32 * 32]);
#endif
