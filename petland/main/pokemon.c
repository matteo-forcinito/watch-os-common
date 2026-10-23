/* Optional fan sprite pack. The portable game also builds without downloaded art. */
#include "pokemon.h"
static const char *const s_names[12][3] = {
    {"Charmander","Charmeleon","Charizard"}, {"Vulpix","Ninetales","Ninetales"},
    {"Growlithe","Arcanine","Arcanine"}, {"Squirtle","Wartortle","Blastoise"},
    {"Poliwag","Poliwhirl","Poliwrath"}, {"Psyduck","Golduck","Golduck"},
    {"Bulbasaur","Ivysaur","Venusaur"}, {"Oddish","Gloom","Vileplume"},
    {"Bellsprout","Weepinbell","Victreebel"}, {"Geodude","Graveler","Golem"},
    {"Pikachu","Raichu","Raichu"}, {"Rhyhorn","Rhydon","Rhydon"}
};
int pokemon_last_form(int family)
{
    return family == 1 || family == 2 || family == 5 || family == 10 || family == 11 ? 1 : 2;
}
const char *pokemon_name(int family, int form)
{
    if (family < 0 || family >= 12 || form < 0 || form > 2) return "Pokemon";
    return s_names[family][form];
}
#ifdef PETLAND_POKEMON_ASSETS
#include "pokemon_assets.h"
bool pokemon_draw(mg_canvas_t *c, int family, int form, int x, int foot_y, int scale, int frame, bool shiny)
{
    if (family < 0 || family >= 12 || form < 0 || form > 2 || scale < 1 || scale > 2) return false;
    const pokemon_sprite_t *sprite = &pokemon_sprites[family][form][shiny ? 1 : 0];
    if (!sprite->frames) return false;
    const uint8_t *pixels = sprite->pixels + (size_t)(frame >= 0 ? frame : 0) % sprite->frames * 1600;
    for (int y = 0; y < 40; y++) for (int col = 0; col < 40;)
    {
        unsigned index = pixels[y * 40 + col];
        int end = col + 1;
        while (end < 40 && pixels[y * 40 + end] == index) end++;
        if (index != 255)
        {
            uint16_t rgb = sprite->palette[index];
            uint32_t color = (uint32_t)((rgb >> 11) * 255 / 31) << 16 |
                (uint32_t)(((rgb >> 5) & 63) * 255 / 63) << 8 | (rgb & 31) * 255 / 31;
            mg_box_pixels(c, x - 20 * scale + col * scale, foot_y - (40 - y) * scale,
                          (end - col) * scale, scale, color);
        }
        col = end;
    }
    return true;
}
#else
bool pokemon_draw(mg_canvas_t *c, int family, int form, int x, int foot_y, int scale, int frame, bool shiny)
{
    (void)c; (void)family; (void)form; (void)x; (void)foot_y; (void)scale; (void)frame; (void)shiny;
    return false;
}
#endif
