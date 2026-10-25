#include "orbi/pet_pokemon.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <string.h>

#ifdef WATCH_PET_POKEMON
#include "pokemon.h"
extern const pokemon_sprite_t pokemon_sprites[12][3][2];
typedef struct { uint16_t dex; uint8_t family, form; const char *name; } species_t;
static const species_t SPECIES[] = {
    {1,6,0,"Bulbasaur"}, {2,6,1,"Ivysaur"}, {3,6,2,"Venusaur"},
    {4,0,0,"Charmander"}, {5,0,1,"Charmeleon"}, {6,0,2,"Charizard"},
    {7,3,0,"Squirtle"}, {8,3,1,"Wartortle"}, {9,3,2,"Blastoise"},
    {25,10,0,"Pikachu"}, {26,10,1,"Raichu"}, {37,1,0,"Vulpix"}, {38,1,1,"Ninetales"},
    {43,7,0,"Oddish"}, {44,7,1,"Gloom"}, {45,7,2,"Vileplume"},
    {54,5,0,"Psyduck"}, {55,5,1,"Golduck"}, {58,2,0,"Growlithe"}, {59,2,1,"Arcanine"},
    {60,4,0,"Poliwag"}, {61,4,1,"Poliwhirl"}, {62,4,2,"Poliwrath"},
    {69,8,0,"Bellsprout"}, {70,8,1,"Weepinbell"}, {71,8,2,"Victreebel"},
    {74,9,0,"Geodude"}, {75,9,1,"Graveler"}, {76,9,2,"Golem"},
    {111,11,0,"Rhyhorn"}, {112,11,1,"Rhydon"}
};
static const species_t *find(uint16_t dex)
{
    for (unsigned i = 0; i < sizeof(SPECIES) / sizeof(SPECIES[0]); i++)
        if (SPECIES[i].dex == dex) return &SPECIES[i];
    return NULL;
}
static const pokemon_sprite_t *sprite(uint16_t dex, bool shiny)
{
    const species_t *s = find(dex);
    return s ? &pokemon_sprites[s->family][s->form][shiny ? 1 : 0] : NULL;
}
#endif

typedef struct { uint64_t seed; uint16_t dex; uint8_t shiny, reserved[5]; } appearance_t;
typedef struct { uint32_t magic, reserved; appearance_t records[8]; } store_t;
static store_t s_store;
static StaticSemaphore_t s_mutex;
static StaticSemaphore_t s_writer_mutex;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_writer;
static bool s_ready;

int pet_pokemon_count(void)
{
#ifdef WATCH_PET_POKEMON
    return sizeof(SPECIES) / sizeof(SPECIES[0]);
#else
    return 0;
#endif
}
uint16_t pet_pokemon_dex(int index)
{
#ifdef WATCH_PET_POKEMON
    if (index >= 0 && index < pet_pokemon_count()) return SPECIES[index].dex;
#else
    (void)index;
#endif
    return 0;
}
const char *pet_pokemon_name(uint16_t dex)
{
#ifdef WATCH_PET_POKEMON
    const species_t *s = find(dex);
    if (s) return s->name;
#else
    (void)dex;
#endif
    return "Pokemon";
}
void pet_pokemon_init(void)
{
    if (s_ready) return;
    if (!s_lock)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_mutex);
        s_writer = xSemaphoreCreateMutexStatic(&s_writer_mutex);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ready)
    {
        store_t loaded = {0};
        nvs_handle_t h;
        size_t size = sizeof(loaded);
        if (nvs_open("pet_looks", NVS_READONLY, &h) == ESP_OK)
        {
            if (nvs_get_blob(h, "v1", &loaded, &size) == ESP_OK && size == sizeof(loaded) &&
                loaded.magic == 0x31504B50 && !loaded.reserved) s_store = loaded;
            nvs_close(h);
        }
        s_store.magic = 0x31504B50;
        s_ready = true;
    }
    xSemaphoreGive(s_lock);
}
void pet_pokemon_get(uint64_t seed, uint16_t *dex, bool *shiny)
{
    *dex = 0;
    *shiny = false;
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < 8; i++) if (s_store.records[i].seed == seed)
    {
        uint16_t candidate = s_store.records[i].dex;
        for (int j = 0; j < pet_pokemon_count(); j++) if (pet_pokemon_dex(j) == candidate)
        {
            *dex = candidate;
            *shiny = s_store.records[i].shiny == 1;
        }
        break;
    }
    xSemaphoreGive(s_lock);
}
bool pet_pokemon_set(uint64_t seed, uint16_t dex, bool shiny)
{
    bool valid = dex == 0;
    for (int j = 0; j < pet_pokemon_count(); j++) valid |= pet_pokemon_dex(j) == dex;
    if (!seed || !valid || !s_ready) return false;
    xSemaphoreTake(s_writer, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    store_t next = s_store;
    xSemaphoreGive(s_lock);
    int slot = -1;
    for (int i = 0; i < 8; i++) if (next.records[i].seed == seed) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < 8; i++) if (!next.records[i].seed) { slot = i; break; }
    bool ok = false;
    if (slot >= 0)
    {
        memset(&next.records[slot], 0, sizeof(next.records[slot]));
        if (dex) { next.records[slot].seed = seed; next.records[slot].dex = dex; next.records[slot].shiny = shiny; }
        nvs_handle_t h;
        if (nvs_open("pet_looks", NVS_READWRITE, &h) == ESP_OK)
        {
            ok = nvs_set_blob(h, "v1", &next, sizeof(next)) == ESP_OK && nvs_commit(h) == ESP_OK;
            nvs_close(h);
        }
        if (ok)
        {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_store = next;
            xSemaphoreGive(s_lock);
        }
    }
    xSemaphoreGive(s_writer);
    return ok;
}
bool pet_pokemon_palette(uint16_t dex, bool shiny, uint32_t out[256])
{
#ifdef WATCH_PET_POKEMON
    const pokemon_sprite_t *s = sprite(dex, shiny);
    if (!s || !s->frames) return false;
    /* Palette length isn't stored by the old Petland descriptor. Collect only
     * indices actually used by its frames, never read beyond its palette. */
    memset(out, 0, 256 * sizeof(*out));
    for (unsigned i = 0; i < (unsigned)s->frames * 1600; i++)
    {
        unsigned index = s->pixels[i];
        if (index == 255 || out[index + 1]) continue;
        unsigned rgb = s->palette[index];
        out[index + 1] = 0xFF000000u | ((rgb >> 11) * 255 / 31) << 16 |
            (((rgb >> 5) & 63) * 255 / 63) << 8 | (rgb & 31) * 255 / 31;
    }
    return true;
#else
    (void)dex; (void)shiny; (void)out;
    return false;
#endif
}
bool pet_pokemon_draw(uint16_t dex, bool shiny, uint32_t frame, uint8_t out[1024])
{
#ifdef WATCH_PET_POKEMON
    const pokemon_sprite_t *s = sprite(dex, shiny);
    if (!s || !s->frames) return false;
    const uint8_t *pixels = s->pixels + (frame % s->frames) * 1600;
    for (unsigned y = 0; y < 32; y++) for (unsigned x = 0; x < 32; x++)
    {
        unsigned index = pixels[(y * 40 / 32) * 40 + x * 40 / 32];
        out[y * 32 + x] = index == 255 ? 0 : (uint8_t)(index + 1);
    }
    return true;
#else
    (void)dex; (void)shiny; (void)frame; (void)out;
    return false;
#endif
}
