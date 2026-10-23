/* Petland: a standalone, turn-based RPG. A 96x96 seeded world, fog of war,
 * four shrines, villages, shops, equipment, relics and a final tower. All
 * rules and drawing are portable C; the external host owns flash and LVGL. */
#include "../../components/apps/minigames/minigame_canvas.h"
#include "pokemon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WORLD 96
typedef struct
{
    uint32_t seed, random;
    int x, y, level, xp, hp, gold, weapon, armour, potions, chapter;
    int seals, relics, boat, quest, steps, mode, enemy, enemy_hp, enemy_max, focus;
    int boss, choice, direction, held, move_at, clock;
    int appearance, role, mana, turn, intent, rare, active_pet, equipped, capsules, drop, evolved;
    uint16_t caught, shiny, beast_xp[12], gear[3];
    uint32_t opened[2];
    int edition;
    uint8_t party[3], active_slot;
    uint16_t creature_hp[12];
    uint8_t visited[WORLD * WORLD / 8];
} rpg_model_t;
typedef struct { rpg_model_t m; bool redraw; char message[96]; } rpg_t;
static const int s_shrines[4][2] = {{38,14},{70,40},{38,70},{14,40}};
static const int s_villages[4][2] = {{12,12},{68,14},{14,70},{70,70}};
static const char *const s_beasts[12] = {"Lumos","Volpice","Brillo","Coralio","Ondino","Nebulina",
    "Frondel","Radin","Musgel","Quarzo","Granok","Ossidion"};
static const char *const s_elements[5] = {"Fuoco","Acqua","Erba","Roccia","Elettro"};
static const uint32_t s_colors[5] = {0xF5A36A,0x7DCEF4,0xA3DEA0,0xBCABD7,0xFFE075};
static const char *const s_intents[4] = {"attacco rapido", "si difende", "carica: interrompilo!", "colpo pesante: para!"};
_Static_assert(sizeof(rpg_model_t) <= MG_CHECKPOINT_BYTES, "Petland checkpoint budget");
static int clamp(int v, int a, int b) { return v < a ? a : v > b ? b : v; }
static int beast_level(const rpg_model_t *m, int id) { return clamp(1 + m->beast_xp[id] / 12, 1, 30); }
static int beast_form(const rpg_model_t *m, int id)
{
    int level = beast_level(m, id);
    int form = level >= 12 && (m->chapter > 1 || m->seals == 15) ? 2 : level >= 5 && (m->chapter > 1 || m->seals) ? 1 : 0;
    return m->edition && form > pokemon_last_form(id) ? pokemon_last_form(id) : form;
}
static const char *beast_name(const rpg_model_t *m, int id) { return m->edition ? pokemon_name(id, beast_form(m, id)) : s_beasts[id]; }
static int element_of(const rpg_model_t *m, int id) { return m->edition && id == 10 ? 4 : id / 3; }
static int enemy_form(const rpg_model_t *m)
{
    int form = m->boss ? m->boss > 3 ? 2 : 1 : 0;
    return m->edition && form > pokemon_last_form(m->enemy - 1) ? pokemon_last_form(m->enemy - 1) : form;
}
static const char *enemy_name(const rpg_model_t *m) { return m->edition ? pokemon_name(m->enemy - 1, enemy_form(m)) : s_beasts[m->enemy - 1]; }
static int hp_for(const rpg_model_t *m, int id) { return 20 + m->level * 5 + m->armour * 4 + beast_level(m, id); }
static int hp_max(const rpg_model_t *m) { return hp_for(m, m->active_pet); }
static void heal_party(rpg_model_t *m)
{
    for (int i = 0; i < 12; i++) if (m->caught & (1 << i)) m->creature_hp[i] = (uint16_t)hp_for(m, i);
    m->hp = hp_max(m);
}
static bool switch_party(rpg_model_t *m)
{
    m->creature_hp[m->active_pet] = (uint16_t)clamp(m->hp, 0, hp_max(m));
    for (int i = 1; i < 3; i++)
    {
        int slot = (m->active_slot + i) % 3, id = m->party[slot];
        if (id < 12 && m->creature_hp[id])
        {
            m->active_slot = (uint8_t)slot; m->active_pet = id;
            m->hp = m->creature_hp[id];
            return true;
        }
    }
    return false;
}
static int effectiveness(int attack, int defence)
{
    static const int beats[4] = {2,0,3,1};
    return beats[attack] == defence ? 200 : beats[defence] == attack ? 50 : 100;
}
static int matchup(const rpg_model_t *m, int attack, int defence)
{
    static const int chart[5][5] = {
        {50,50,200,50,100}, {200,50,50,200,100}, {50,200,50,200,100},
        {200,100,100,100,100}, {100,200,50,0,50}
    };
    return m->edition ? chart[attack][defence] : effectiveness(attack, defence);
}
static int action_count(const rpg_model_t *m) { return m->mode == 4 || m->mode == 6 ? 4 : m->mode == 5 ? 3 : m->mode == 1 ? 6 : 5; }
static uint16_t make_gear(int power, int element, int rarity) { return (uint16_t)(power | (element << 6) | (rarity << 8)); }
static uint32_t tile_hash(const rpg_model_t *m, int x, int y)
{
    uint32_t h = m->seed ^ (uint32_t)x * 374761393u ^ (uint32_t)y * 668265263u;
    return (h ^ (h >> 13)) * 1274126177u;
}
static bool road(int x, int y)
{
    return x == 12 || x == 14 || x == 38 || x == 46 || x == 68 || x == 70 ||
           y == 12 || y == 14 || y == 40 || y == 46 || y == 70;
}
static int landmark(int x, int y)
{
    for (int i = 0; i < 4; i++)
    {
        if (x == s_villages[i][0] && y == s_villages[i][1]) return 1;
        if (x == s_shrines[i][0] && y == s_shrines[i][1]) return 2 + i;
    }
    return x == 46 && y == 46 ? 6 : 0;
}
static int terrain(const rpg_model_t *m, int x, int y)
{
    if (x < 1 || y < 1 || x >= WORLD - 1 || y >= WORLD - 1) return 3;
    if (road(x, y) || landmark(x, y)) return 0;
    unsigned h = (tile_hash(m, x / 6, y / 6) % 100 * 2 + tile_hash(m, x, y) % 100) / 3;
    return h < 22 ? 1 : h > 88 ? 3 : h > 52 ? 2 : 0;
}
static int site_at(const rpg_model_t *m, int x, int y)
{
    int site = landmark(x, y);
    if (site || x < 1 || y < 1 || x >= WORLD - 1 || y >= WORLD - 1) return site;
    int rx = x / 12, ry = y / 12;
    uint32_t h = tile_hash(m, rx + 113, ry + 317);
    if (x == rx * 12 + 3 + (int)(h % 6) && y == ry * 12 + 3 + (int)((h >> 8) % 6) &&
        !road(x, y) && terrain(m, x, y) != 3) return 7;
    return 0;
}
static bool seen(const rpg_model_t *m, int x, int y)
{
    if (x < 0 || y < 0 || x >= WORLD || y >= WORLD) return false;
    int bit = y * WORLD + x;
    return (m->visited[bit / 8] & (1u << (bit % 8))) != 0;
}
static void reveal(rpg_model_t *m)
{
    for (int dy = -4; dy <= 4; dy++) for (int dx = -4; dx <= 4; dx++)
    {
        int x = m->x + dx, y = m->y + dy;
        if (x >= 0 && x < WORLD && y >= 0 && y < WORLD)
        {
            int bit = y * WORLD + x;
            m->visited[bit / 8] |= (uint8_t)(1u << (bit % 8));
        }
    }
}
static void say(rpg_t *s, const char *message)
{
    snprintf(s->message, sizeof(s->message), "%s", message);
    s->redraw = true;
}
static void start(void *state, uint32_t seed)
{
    rpg_t *s = state;
    memset(s, 0, sizeof(*s));
    rpg_model_t *m = &s->m;
    m->seed = m->random = seed ? seed : 1;
    m->x = m->y = 12;
    m->level = m->chapter = 1;
    m->potions = 3;
    m->capsules = 8;
    m->active_pet = 6;
    m->caught = 1 << 6;
    m->party[0] = 6; m->party[1] = m->party[2] = 255;
    m->mana = 6;
    m->gear[0] = make_gear(1, 2, 1);
    heal_party(m);
    reveal(m);
    say(s, "Benvenuto! Tocca il centro al villaggio");
}
static void wild_start(void *state, uint32_t seed)
{
    start(state, seed);
    rpg_t *s = state;
    s->m.edition = 1;
    s->m.mode = 5;
    say(s, "Scegli il tuo primo compagno");
}
static void battle(rpg_t *s, int boss)
{
    rpg_model_t *m = &s->m;
    m->mode = 1;
    m->held = m->focus = m->choice = 0;
    m->boss = boss;
    int biome = m->x < 48 ? m->y < 48 ? 2 : 1 : m->y < 48 ? 0 : 3;
    m->enemy = boss ? 1 + ((boss - 1) % 4) * 3 : 1 + biome * 3 + (int)(mg_random(&m->random) % 3);
    m->enemy_hp = m->enemy_max = 10 + clamp(m->chapter, 1, 30) * 7 + m->level * 3 + (boss ? 24 + boss * 6 : 0);
    m->turn = 0;
    m->intent = m->enemy % 4;
    m->rare = !boss && mg_random(&m->random) % 32 == 0;
    m->mana = 6;
    snprintf(s->message, sizeof(s->message), "%s %s: %s", enemy_name(m), s_elements[element_of(m, m->enemy - 1)], s_intents[m->intent]);
    s->redraw = true;
}
static void preview_drop(rpg_t *s)
{
    const rpg_model_t *m = &s->m;
    unsigned gear = m->choice < 3 ? m->gear[m->choice] : 0;
    if (m->choice == 3) snprintf(s->message, sizeof(s->message), "Vendi: +%d oro", (m->drop & 63) * 3 + ((m->drop >> 8) & 3) * 5);
    else if (!gear) snprintf(s->message, sizeof(s->message), "Slot %d: vuoto", m->choice + 1);
    else snprintf(s->message, sizeof(s->message), "S%d: %s P%d R%d", m->choice + 1, s_elements[(gear >> 6) & 3], (int)(gear & 63), (int)((gear >> 8) & 3));
    s->redraw = true;
}
static void victory(rpg_t *s)
{
    rpg_model_t *m = &s->m;
    m->gold = clamp(m->gold + 8 + m->chapter * 3 + (m->boss ? 25 : 0), 0, 10000000);
    m->xp = clamp(m->xp + 8 + (m->boss ? 24 : 0), 0, 100000000);
    if (m->xp >= m->level * 20 && m->level < 99)
    {
        m->xp -= m->level * 20;
        m->level++;
        m->hp = hp_max(m);
    }
    if (m->boss || m->rare || mg_random(&m->random) % 5 == 0) m->relics |= 1 << (mg_random(&m->random) % 12);
    int before = beast_form(m, m->active_pet);
    m->beast_xp[m->active_pet] = (uint16_t)clamp(m->beast_xp[m->active_pet] + (m->boss ? 12 : 5), 0, 348);
    if (m->boss >= 1 && m->boss <= 4)
    {
        m->seals |= 1 << (m->boss - 1);
        m->boat = 1;
        say(s, "Sigillo trovato! Barca sbloccata");
    }
    else if (m->boss == 5)
    {
        m->chapter = clamp(m->chapter + 1, 1, 1000000);
        m->seals = 0;
        m->seed = mg_random(&m->random);
        memset(m->visited, 0, sizeof(m->visited));
        memset(m->opened, 0, sizeof(m->opened));
        reveal(m);
        say(s, "Torre liberata! Nuovo capitolo");
    }
    else say(s, "Vittoria! Oro, esperienza e reliquia");
    m->evolved = beast_form(m, m->active_pet) > before;
    unsigned roll = mg_random(&m->random) % 100;
    int rarity = m->rare || roll < 6 ? 3 : roll < 28 ? 2 : 1;
    m->drop = make_gear(clamp(m->level + m->chapter + (int)(roll % 4), 1, 40), (int)(mg_random(&m->random) % 4), rarity);
    m->mode = 4;
    m->choice = 0;
    preview_drop(s);
    m->boss = 0;
}
static void combat(rpg_t *s, int action)
{
    rpg_model_t *m = &s->m;
    if (action == 6)
    {
        if (m->boss) { say(s, "Il guardiano blocca la fuga"); return; }
        m->mode = 0;
        m->gold -= m->gold / 20;
        say(s, "Fuga riuscita");
        return;
    }
    int guard = 0, interrupted = 0;
    int element = element_of(m, m->active_pet), enemy_element = element_of(m, m->enemy - 1);
    if (action == 5)
    {
        if (!switch_party(m)) { say(s, "Nessun altro compagno pronto"); return; }
        m->focus = 0;
    }
    if (action == 0)
    {
        unsigned gear = m->gear[m->equipped];
        int power = (int)(gear & 63) * (int)(((gear >> 8) & 3) + 1) / 2;
        int damage = 3 + m->level + m->weapon * 2 + power + beast_level(m, m->active_pet) / 3 + (m->role == 0 ? 2 : 0);
        bool focused = m->focus >= 2;
        if (focused) damage *= 3, m->focus = 0;
        damage = damage * matchup(m, (gear >> 6) & 3, enemy_element) / 100;
        if (m->intent == 1) damage = (damage + 2) / 3;
        interrupted = m->intent == 2 && focused && (!m->boss || m->enemy_hp > m->enemy_max / 2);
        m->enemy_hp -= damage;
    }
    else if (action == 1)
    {
        if (m->mana < 3) { say(s, "Energia bassa: para per ricaricare"); return; }
        m->mana -= 3;
        int damage = (7 + m->level + beast_level(m, m->active_pet) * 2 + beast_form(m, m->active_pet) * 5) * matchup(m, element, enemy_element) / 100;
        if (element == 0) damage += 3;
        if (element == 2) m->hp = clamp(m->hp + 3, 1, hp_max(m));
        if (m->intent == 1 && matchup(m, element, enemy_element) < 200) damage /= 2;
        m->enemy_hp -= damage;
        interrupted = m->intent == 2 && damage > 0;
    }
    else if (action == 2)
    {
        guard = 1;
        m->focus = clamp(m->focus + 1, 0, 2);
        m->mana = clamp(m->mana + (m->role == 2 ? 2 : 1), 0, 6);
    }
    else if (action == 3 && m->potions > 0)
    {
        m->potions--;
        m->hp = clamp(m->hp + hp_max(m) / 2 + (m->role == 2 ? 5 : 0), 1, hp_max(m));
    }
    else if (action == 3) { say(s, "Pozioni finite: visita un emporio"); return; }
    else if (action == 4)
    {
        if (m->boss) { say(s, "I guardiani non sono catturabili"); return; }
        if (!m->capsules) { say(s, "Sfere finite: compra un kit"); return; }
        m->capsules--;
        int chance = 15 + (m->enemy_max - m->enemy_hp) * 70 / m->enemy_max + m->focus * 5 + (m->role == 3 ? 10 : 0);
        if (m->rare) chance /= 2;
        if ((int)(mg_random(&m->random) % 100) < clamp(chance, 5, 95))
        {
            int id = m->enemy - 1;
            m->caught |= (uint16_t)(1 << id);
            if (m->rare) m->shiny |= (uint16_t)(1 << id);
            m->beast_xp[id] = (uint16_t)clamp(m->beast_xp[id] + 8, 0, 348);
            m->creature_hp[id] = (uint16_t)hp_for(m, id);
            bool present = false;
            for (int i = 0; i < 3; i++) if (m->party[i] == id) present = true;
            if (!present) for (int i = 0; i < 3; i++) if (m->party[i] == 255) { m->party[i] = (uint8_t)id; break; }
            victory(s);
            say(s, "Cattura riuscita! Nuovo compagno");
            return;
        }
    }
    if (m->enemy_hp <= 0) { victory(s); return; }
    int incoming = 2 + clamp(m->chapter, 1, 30) * 2 + (m->boss ? 2 : 0) + (int)(mg_random(&m->random) % 3) - m->armour - (m->role == 1 ? 2 : 0);
    incoming = clamp(incoming, 1, 10000);
    if (m->intent == 1 || m->intent == 2) incoming = 0;
    else if (m->intent == 3) incoming *= m->boss && m->enemy_hp < m->enemy_max / 2 ? 3 : 2;
    if (m->edition) incoming = incoming * matchup(m, enemy_element, element_of(m, m->active_pet)) / 100;
    if (action == 1 && element == 1) incoming = (incoming + 1) / 2;
    if (action == 1 && element == 3) incoming = clamp(incoming - 3, 0, 10000);
    if (guard) incoming = (incoming + 2) / 3;
    m->hp -= incoming;
    if (m->hp <= 0)
    {
        if (switch_party(m))
        {
            m->mana = clamp(m->mana, 0, 3); m->focus = 0;
            m->intent = (m->intent + 1) % 4;
            snprintf(s->message, sizeof(s->message), "Entra %s; %s", beast_name(m, m->active_pet), s_intents[m->intent]);
        }
        else
        {
            m->mode = m->boss = 0;
            m->x = m->y = 12;
            heal_party(m);
            m->gold -= m->gold / 10;
            reveal(m);
            say(s, "Soccorso al villaggio: progressi salvi");
        }
    }
    else
    {
        m->turn = (m->turn + 1) % 1000000;
        m->intent = interrupted ? 0 : (m->intent + 1) % 4;
        snprintf(s->message, sizeof(s->message), "%s: %s", enemy_name(m), s_intents[m->intent]);
    }
    s->redraw = true;
}
static void interact(rpg_t *s)
{
    rpg_model_t *m = &s->m;
    int found = 0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
    {
        int site = site_at(m, m->x + dx, m->y + dy);
        if (site && (!found || site < found)) found = site;
    }
    if (found == 1)
    {
        heal_party(m);
        if (!m->quest)
        {
            m->quest = 1;
            say(s, m->edition ? "12 famiglie: esplora, cattura, evolvi" : "Il faro e spento: cerca 4 sigilli");
            return;
        }
        m->quest = 1;
        m->mode = 2;
        m->choice = 0;
        say(s, "Emporio: kit, attrezzatura e campo");
    }
    else if (found >= 2 && found <= 5)
    {
        int shrine = found - 2;
        if (!(m->seals & (1 << shrine))) battle(s, shrine + 1);
        else say(s, "Santuario gia liberato");
    }
    else if (found == 6)
    {
        if (m->seals == 15) battle(s, 5);
        else say(s, "La torre richiede i quattro sigilli");
    }
    else if (found == 7)
    {
        int region = m->x / 12 + m->y / 12 * 8;
        uint32_t mask = 1u << (region % 32);
        if (m->opened[region / 32] & mask) { say(s, "Rovina gia esplorata"); return; }
        m->opened[region / 32] |= mask;
        m->capsules = clamp(m->capsules + 1, 0, 99);
        if (m->role != 3 && mg_random(&m->random) % 4 == 0) { battle(s, 0); say(s, "Trappola! La rovina era sorvegliata"); return; }
        m->drop = make_gear(clamp(m->level + m->chapter + 3, 1, 40), (int)(mg_random(&m->random) % 4), mg_random(&m->random) % 5 == 0 ? 3 : 2);
        m->mode = 4; m->choice = 0;
        say(s, "Rovina segreta: scegli un bottino");
    }
    else
    {
        static const char *const names[4] = {"Aurora", "Marea", "Radice", "Ombra"};
        int next = 0;
        while (next < 4 && (m->seals & (1 << next))) next++;
        m->choice = (m->choice + 1) % 3;
        if (m->choice == 1)
            snprintf(s->message, sizeof(s->message), "Pozioni %d  Sfere %d  Arma %d", m->potions, m->capsules, m->weapon);
        else if (m->choice == 2)
        {
            int relics = 0;
            for (int i = 0; i < 12; i++) if (m->relics & (1 << i)) relics++;
            int caught = 0;
            for (int i = 0; i < 12; i++) if (m->caught & (1 << i)) caught++;
            snprintf(s->message, sizeof(s->message), "Specie %d/12  Reliquie %d/12", caught, relics);
        }
        else if (next < 4)
            snprintf(s->message, sizeof(s->message), "%s: raggiungi %d,%d", names[next], s_shrines[next][0], s_shrines[next][1]);
        else snprintf(s->message, sizeof(s->message), "Quattro sigilli: torna alla torre 46,46");
        s->redraw = true;
    }
}
static void move(rpg_t *s, int direction)
{
    static const int dx[4] = {1,0,-1,0}, dy[4] = {0,1,0,-1};
    rpg_model_t *m = &s->m;
    int x = m->x + dx[direction], y = m->y + dy[direction];
    int type = terrain(m, x, y);
    if (type == 3 || (type == 1 && !m->boat)) { say(s, type == 1 ? "Acqua: serve la barca dei santuari" : "Montagna: cerca un sentiero"); return; }
    m->x = x; m->y = y;
    m->steps = (m->steps + 1) % 100000000;
    reveal(m);
    s->redraw = true;
    if (type == 2 && mg_random(&m->random) % 8 == 0) battle(s, 0);
    else if (site_at(m, x, y)) say(s, "Luogo speciale: tocca il centro");
    else if (m->steps % 25 == 0) { m->potions = clamp(m->potions + 1, 0, 30); say(s, "Erbe raccolte: una pozione in piu"); }
}
static void shop(rpg_t *s, int choice)
{
    rpg_model_t *m = &s->m;
    if (choice == 3) { m->mode = 3; m->choice = 0; say(s, "Campo: cambia la tua strategia"); return; }
    if (choice == 4) { m->mode = 0; m->choice = 0; say(s, "Buon viaggio!"); return; }
    int cost = choice == 0 ? 20 + m->weapon * 20 : choice == 1 ? 15 + m->armour * 20 : 12;
    if (m->gold < cost) { say(s, "Oro insufficiente. Trascina per uscire"); return; }
    m->gold -= cost;
    if (choice == 0) m->weapon = clamp(m->weapon + 1, 0, 50);
    else if (choice == 1) m->armour = clamp(m->armour + 1, 0, 50);
    else m->potions = clamp(m->potions + 1, 0, 30), m->capsules = clamp(m->capsules + 2, 0, 99);
    m->mode = 0;
    say(s, "Equipaggiamento pronto. Buon viaggio!");
}
static void camp(rpg_t *s, int choice)
{
    rpg_model_t *m = &s->m;
    if (choice == 0) m->appearance = (m->appearance + 1) % 8, say(s, "Nuovo aspetto: nessun bonus nascosto");
    else if (choice == 1)
    {
        static const char *const roles[4] = {"Assaltatore: colpi forti", "Custode: subisce meno danni", "Alchimista: energia e cure", "Esploratore: cattura migliore"};
        m->role = (m->role + 1) % 4;
        say(s, roles[m->role]);
    }
    else if (choice == 2)
    {
        m->mode = 6; m->choice = 0;
        say(s, "Scegli uno slot: cambia il compagno");
    }
    else if (choice == 3)
    {
        for (int i = 1; i <= 3; i++) if (m->gear[(m->equipped + i) % 3]) { m->equipped = (m->equipped + i) % 3; break; }
        unsigned gear = m->gear[m->equipped];
        snprintf(s->message, sizeof(s->message), "Arma %d: %s P%d rarita %d", m->equipped + 1, s_elements[(gear >> 6) & 3], (int)(gear & 63), (int)((gear >> 8) & 3));
        s->redraw = true;
    }
    else m->mode = m->choice = 0, say(s, "Parti preparato: ogni elemento conta");
}
static void team(rpg_t *s, int slot)
{
    rpg_model_t *m = &s->m;
    if (slot == 3) { m->mode = 3; m->choice = 0; say(s, "Squadra pronta per la prossima sfida"); return; }
    int current = m->party[slot] < 12 ? m->party[slot] : 11;
    for (int step = 1; step <= 12; step++)
    {
        int id = (current + step) % 12;
        bool used = false;
        for (int i = 0; i < 3; i++) if (i != slot && m->party[i] == id) used = true;
        if (!(m->caught & (1 << id)) || used) continue;
        m->creature_hp[m->active_pet] = (uint16_t)m->hp;
        m->party[slot] = (uint8_t)id;
        m->active_slot = (uint8_t)slot;
        m->active_pet = id;
        m->hp = m->creature_hp[id] ? m->creature_hp[id] : hp_for(m, id);
        snprintf(s->message, sizeof(s->message), "Slot%d: %s LV%d %s", slot + 1, beast_name(m, id), beast_level(m, id), s_elements[element_of(m, id)]);
        s->redraw = true;
        return;
    }
    say(s, "Cattura altri compagni per questo slot");
}
static void choose_starter(rpg_t *s, int choice)
{
    rpg_model_t *m = &s->m;
    int id = clamp(choice, 0, 2) * 3;
    m->active_pet = id; m->party[0] = (uint8_t)id;
    m->caught = (uint16_t)(1 << id);
    heal_party(m);
    m->mode = m->choice = 0;
    say(s, "Il viaggio comincia: cerca altre specie");
}
static void take_drop(rpg_t *s, int choice)
{
    rpg_model_t *m = &s->m;
    if (choice < 3) m->gear[choice] = (uint16_t)m->drop;
    else m->gold = clamp(m->gold + (m->drop & 63) * 3 + ((m->drop >> 8) & 3) * 5, 0, 10000000);
    m->drop = m->mode = m->choice = 0;
    if (m->evolved) snprintf(s->message, sizeof(s->message), "Evoluzione: %s!", beast_name(m, m->active_pet));
    else snprintf(s->message, sizeof(s->message), "%s. Equipaggia le armi al campo", choice < 3 ? "Oggetto conservato" : "Oggetto venduto");
    s->redraw = true;
}
static void input(void *state, const mg_input_t *event)
{
    rpg_t *s = state;
    rpg_model_t *m = &s->m;
    if (event->kind == MG_RELEASE)
    {
        bool armed = m->held != 0;
        m->held = 0;
        if (m->mode == 4 && armed && !event->cancelled) take_drop(s, m->choice);
        return;
    }
    if (event->kind == MG_TURN)
    {
        m->held = 0;
        if (m->mode) m->choice = clamp(m->choice + event->direction, 0, action_count(m) - 1);
        else m->direction = (m->direction + event->direction + 5) % 5;
        s->redraw = true;
        if (m->mode == 4) preview_drop(s);
    }
    else if (event->kind == MG_CLICK)
    {
        if (m->mode == 1) combat(s, m->choice);
        else if (m->mode == 2) shop(s, m->choice);
        else if (m->mode == 3) camp(s, m->choice);
        else if (m->mode == 4) take_drop(s, m->choice);
        else if (m->mode == 5) choose_starter(s, m->choice);
        else if (m->mode == 6) team(s, m->choice);
        else if (m->direction == 4 || site_at(m, m->x, m->y)) interact(s);
        else move(s, m->direction);
    }
    else if (event->kind == MG_PRESS || event->kind == MG_MOVE)
    {
        if (m->mode)
        {
            if (m->mode == 4)
            {
                m->choice = clamp(event->x * 4 / 1000, 0, 3);
                m->held = 1;
                preview_drop(s);
                return;
            }
            if (event->kind != MG_PRESS) return;
            int choice = clamp(event->x * action_count(m) / 1000, 0, action_count(m) - 1);
            if (m->mode == 1) combat(s, event->y > 850 ? 6 : event->y < 180 ? 5 : choice);
            else if (m->mode == 5) choose_starter(s, choice);
            else if (m->mode == 6) team(s, choice);
            else if (event->y < 220) m->mode = m->choice = 0, say(s, "Cerca i quattro santuari");
            else if (m->mode == 3) camp(s, choice);
            else shop(s, choice);
            return;
        }
        int dx = event->x - 500, dy = event->y - 500;
        if (abs(dx) < 150 && abs(dy) < 180)
        {
            if (event->kind == MG_PRESS) interact(s);
            return;
        }
        m->direction = abs(dx) > abs(dy) ? dx > 0 ? 0 : 2 : dy > 0 ? 1 : 3;
        m->held = 1;
        if (event->kind == MG_PRESS) move(s, m->direction), m->move_at = 180;
    }
}
static void step(void *state, int ms)
{
    rpg_t *s = state;
    rpg_model_t *m = &s->m;
    int animation = m->clock / 240;
    m->clock = (m->clock + ms) % 60000;
    if (m->edition && m->mode && m->clock / 240 != animation) s->redraw = true;
    m->move_at = clamp(m->move_at - ms, 0, 500);
    if (!m->mode && m->held && !m->move_at) move(s, m->direction), m->move_at = 180;
}
static void draw_original(mg_canvas_t *c, int id, int form, int x, int y, bool rare)
{
    int r = 12 + form * 5, element = id / 3;
    uint32_t color = rare ? 0xF5DB91 : s_colors[element];
    mg_box_pixels(c, x - r - 3, y + r, r * 2 + 6, 4, 0x111C2C);
    if (element == 0)
        for (int i = 0; i < 4; i++) mg_box_pixels(c, x - r - 10 + i * 3, y + 3 - i * 4, 5, 8, i & 1 ? 0xFFD286 : 0xD37559);
    if (element == 1)
    {
        mg_box_pixels(c, x - r - 7, y - 5, 10, 15, color);
        mg_box_pixels(c, x + r - 3, y - 5, 10, 15, color);
    }
    if (element == 2)
    {
        mg_box_pixels(c, x - r - 4, y - r + 2, 13, 6, 0x65A77B);
        mg_box_pixels(c, x + r - 9, y - r + 2, 13, 6, 0x65A77B);
    }
    mg_box_pixels(c, x - r, y - r + 3, r * 2, r * 2 - 2, 0x33475D);
    mg_box_pixels(c, x - r + 2, y - r + 2, r * 2 - 4, r * 2 - 4, color);
    mg_box_pixels(c, x - r / 2, y + 2, r, r - 4, 0xE5EFD7);
    if (id % 3 != 0 || form > 0)
    {
        mg_box_pixels(c, x - r + 3, y - r - 5 - form * 2, 5, 10, color);
        mg_box_pixels(c, x + r - 8, y - r - 5 - form * 2, 5, 10, color);
    }
    if (element == 3 || form == 2)
        for (int i = -1; i <= 1; i++) mg_box_pixels(c, x + i * 9 - 2, y - r - 9, 5, 12, i ? color : 0xE9D8FD);
    mg_box_pixels(c, x - 7, y - 3, 4, 5, 0x182D43);
    mg_box_pixels(c, x + 3, y - 3, 4, 5, 0x182D43);
    mg_box_pixels(c, x - 6, y - 3, 2, 2, 0xFFFFFF);
    mg_box_pixels(c, x + 4, y - 3, 2, 2, 0xFFFFFF);
    if (rare) mg_box_pixels(c, x + r + 5, y - r - 4, 3, 9, 0xFFE89F), mg_box_pixels(c, x + r + 2, y - r - 1, 9, 3, 0xFFE89F);
}
static void draw_beast(mg_canvas_t *c, const rpg_model_t *m, int id, int form, int x, int y, bool rare)
{
    if (m->edition && pokemon_draw(c, id, form, x, y + 34, 2, m->clock / 240, rare)) return;
    draw_original(c, id, form, x, y, rare);
}
static void draw(void *state, mg_canvas_t *c)
{
    rpg_t *s = state;
    rpg_model_t *m = &s->m;
    if (!s->redraw) return;
    s->redraw = false;
    mg_clear(c, 0x0C1825);
    if (!m->mode)
    {
        int tile = c->width / 11, cx = c->width / 2, cy = c->height / 2;
        for (int dy = -4; dy <= 4; dy++) for (int dx = -6; dx <= 6; dx++)
        {
            int x = m->x + dx, y = m->y + dy, px = cx + dx * tile - tile / 2, py = cy + dy * tile - tile / 2;
            if (!seen(m, x, y)) { mg_box_pixels(c, px, py, tile, tile, 0x0E1C2D); continue; }
            int t = terrain(m, x, y), site = site_at(m, x, y);
            uint32_t grass = x < 48 ? y < 48 ? 0x37664B : 0x72805D : y < 48 ? 0xA98E63 : 0x4C5B70;
            mg_box_pixels(c, px, py, tile - 1, tile - 1, t == 1 ? 0x286B9B : t == 3 ? 0x69778A : grass);
            if (road(x, y))
            {
                bool vertical = x == 12 || x == 14 || x == 38 || x == 46 || x == 68 || x == 70;
                bool horizontal = y == 12 || y == 14 || y == 40 || y == 46 || y == 70;
                if (vertical) mg_box_pixels(c, px + tile / 3, py, tile / 3, tile, 0xB09C7F);
                if (horizontal) mg_box_pixels(c, px, py + tile / 3, tile, tile / 3, 0xB09C7F);
            }
            else if (t == 1)
            {
                mg_box_pixels(c, px + 4, py + tile / 3, tile / 2, 2, 0x5392B8);
                mg_box_pixels(c, px + tile / 3, py + tile * 2 / 3, tile / 2, 2, 0x327CA7);
            }
            else if (t == 3)
            {
                for (int step = 0; step < 4; step++) mg_box_pixels(c, px + 3 + step * 3, py + tile - 7 - step * 5, tile - 6 - step * 6, 5, step == 3 ? 0xC8D9DB : 0x7A8795);
            }
            if (t == 2)
            {
                mg_box_pixels(c, px + tile / 2 - 2, py + tile / 3, 5, tile / 2, 0x624B3B);
                mg_box_pixels(c, px + 3, py + 7, tile - 7, tile / 2, 0x1A3E35);
                mg_box_pixels(c, px + 6, py + 2, tile - 13, tile / 2, 0x315B43);
                mg_box_pixels(c, px + 8, py + 4, tile / 3, 3, 0x53815B);
            }
            if (site)
            {
                uint32_t color = site == 1 || site == 7 ? 0xFFD184 : site == 6 ? 0xC691F3 : (m->seals & (1 << (site - 2))) ? 0x9CF1CB : 0x78CDE3;
                mg_box_pixels(c, px + 4, py + 4, tile - 8, tile - 8, 0x29394C);
                mg_box_pixels(c, px + 7, py + 5, tile - 14, tile - 14, color);
                if (site == 1)
                {
                    mg_box_pixels(c, px + 4, py + 3, tile - 8, 8, 0xB96957);
                    mg_box_pixels(c, px + tile / 2 - 3, py + tile / 2, 7, tile / 2 - 4, 0x374758);
                    mg_box_pixels(c, px + 9, py + 14, 5, 5, 0xFFF1B4);
                }
            }
        }
        mg_pet_pixels(c, cx, cy + c->actor_size / 2);
        if (m->edition) pokemon_draw(c, m->active_pet, beast_form(m, m->active_pet), cx + tile, cy + 20, 1, m->clock / 240, !!(m->shiny & (1 << m->active_pet)));
        static const int dx[5] = {1,0,-1,0,0}, dy[5] = {0,1,0,-1,0};
        mg_box_pixels(c, cx + dx[m->direction] * tile - 3, cy + dy[m->direction] * tile - 3, 7, 7, 0xBDFBE6);
        int mx = c->width - 66, my = 8;
        mg_box_pixels(c, mx - 2, my - 2, 60, 60, 0x102034);
        for (int y = 0; y < 24; y++) for (int x = 0; x < 24; x++)
            if (seen(m, x * 4, y * 4)) mg_box_pixels(c, mx + x * 2, my + y * 2, 2, 2, 0x527565);
        mg_box_pixels(c, mx + m->x / 2, my + m->y / 2, 3, 3, 0xFFE4A2);
    }
    else
    {
        int w = c->width, h = c->height;
        mg_box_pixels(c, w / 9, h / 8, w * 7 / 9, h * 3 / 5, 0x25354D);
        if (m->mode != 5 && m->mode != 6)
        {
            mg_pet_pixels(c, w / 5, h * 3 / 5);
            draw_beast(c, m, m->active_pet, beast_form(m, m->active_pet), w * 37 / 100, h * 2 / 5, !!(m->shiny & (1 << m->active_pet)));
        }
        else for (int i = 0; i < 3; i++)
        {
            int id = m->mode == 5 ? i * 3 : m->party[i];
            if (id < 12) draw_beast(c, m, id, beast_form(m, id), w * (180 + i * 320) / 1000, h * 2 / 5, !!(m->shiny & (1 << id)));
        }
        if (m->mode == 1)
        {
            int x = w * 2 / 3, y = h / 3, r = m->boss ? 25 : 17;
            draw_beast(c, m, m->enemy - 1, enemy_form(m), x, y, !!m->rare);
            mg_box_pixels(c, x - 30, y + r + 7, 60 * m->enemy_hp / m->enemy_max, 4, 0xECA4AD);
            mg_box_pixels(c, x - 30, y + r + 14, 60, 3, s_colors[element_of(m, m->enemy - 1)]);
        }
        if (m->mode == 4)
        {
            int x = w * 2 / 3;
            mg_box_pixels(c, x - 20, h / 4, 40, h / 3, ((m->drop >> 8) & 3) == 3 ? 0xF4D188 : 0xA99ED4);
            mg_box_pixels(c, x - 4, h / 4 + 5, 8, h / 4, s_colors[(m->drop >> 6) & 3]);
        }
        int count = action_count(m);
        for (int i = 0; i < count; i++)
        {
            int x = w * (100 + i * 800 / (count - 1)) / 1000, card = w / (count + 3);
            mg_box_pixels(c, x - card / 2, h * 3 / 4, card, h / 5, m->choice == i ? 0xA5D7B1 : 0x5F718A);
            mg_box_pixels(c, x - 3, h * 3 / 4 + 5, 6, h / 7, 0x20344B);
            if (i) mg_box_pixels(c, x - 12, h * 3 / 4 + 12, 24, 6, 0x20344B);
        }
    }
}
static void hud(const void *state, char text[MG_HUD_BYTES])
{
    const rpg_model_t *m = &((const rpg_t *)state)->m;
    if (m->mode == 4) snprintf(text, MG_HUD_BYTES, "Drop %s P%d R%d", s_elements[(m->drop >> 6) & 3], m->drop & 63, (m->drop >> 8) & 3);
    else if (m->mode == 2) snprintf(text, MG_HUD_BYTES, "Oro%d: arma%d dif%d kit12", m->gold, 20 + m->weapon * 20, 15 + m->armour * 20);
    else if (m->mode == 1) snprintf(text, MG_HUD_BYTES, "%s L%d  V%d E%d Sf%d", beast_name(m, m->active_pet), beast_level(m, m->active_pet), m->hp, m->mana, m->capsules);
    else snprintf(text, MG_HUD_BYTES, "LV %d  V%d/%d Oro %d [%d,%d]", m->level, m->hp, hp_max(m), m->gold, m->x, m->y);
}
static const char *help(const void *state)
{
    const rpg_model_t *m = &((const rpg_t *)state)->m;
    if (m->mode == 1) return m->edition ? "Colpo / mossa / scudo / cura / ball / pet" : "Colpo / magia / scudo / cura / sfera / pet";
    if (m->mode == 2) return "Arma / difesa / kit / campo / esci";
    if (m->mode == 3) return "Aspetto / ruolo / pet / arma / esci";
    if (m->mode == 4) return "Slot 1 / slot 2 / slot 3 / vendi";
    if (m->mode == 5) return "Charmander / Squirtle / Bulbasaur";
    if (m->mode == 6) return "Slot 1 / slot 2 / slot 3 / indietro";
    return "Trascina; centro: diario e azioni";
}
static int actor_variant(const void *state) { return ((const rpg_t *)state)->m.appearance; }
static const char *feedback(const void *state) { return ((const rpg_t *)state)->message; }
static bool result(const void *state, mg_result_t *out) { (void)state; *out = (mg_result_t){0}; return false; }
static bool save(const void *state, void *data, size_t size)
{
    if (size != sizeof(rpg_model_t)) return false;
    memcpy(data, &((const rpg_t *)state)->m, size);
    rpg_model_t *m = data;
    m->held = 0;
    m->creature_hp[m->active_pet] = (uint16_t)m->hp;
    return true;
}
static bool load(void *state, const void *data, size_t size)
{
    if (size != sizeof(rpg_model_t)) return false;
    const rpg_model_t *m = data;
    rpg_t *s = state;
    if (m->edition != s->m.edition || m->active_slot >= 3 || m->active_pet >= 12 || m->active_pet < 0 ||
        m->party[m->active_slot] != m->active_pet) return false;
    if (m->active_pet < 0 || m->active_pet >= 12 || !(m->caught & (1 << m->active_pet)) || m->caught > 4095 ||
        m->shiny > 4095 || m->equipped < 0 || m->equipped > 2 || !m->gear[m->equipped] ||
        m->mana < 0 || m->mana > 6 || m->role < 0 || m->role > 3 || m->appearance < 0 || m->appearance > 7 ||
        m->capsules < 0 || m->capsules > 99 || m->intent < 0 || m->intent > 3 || m->drop < 0 || m->drop > 1023) return false;
    if (m->clock < 0 || m->clock >= 60000 || m->steps < 0 || m->steps >= 100000000 ||
        m->xp < 0 || m->xp > 100000000 || m->move_at < 0 || m->move_at > 500 ||
        m->turn < 0 || m->turn >= 1000000 || m->rare < 0 || m->rare > 1 ||
        (m->mode == 1 && (m->enemy < 1 || m->enemy > 12)) || (m->mode == 4 && !m->drop)) return false;
    for (int i = 0; i < 12; i++) if (m->beast_xp[i] > 348) return false;
    for (int i = 0; i < 12; i++) if (m->creature_hp[i] > 1000) return false;
    for (int i = 0; i < 3; i++) if (m->party[i] != 255 && (m->party[i] >= 12 || !(m->caught & (1 << m->party[i])))) return false;
    for (int i = 0; i < 3; i++) if (m->gear[i] > 1023 || (m->gear[i] && (!(m->gear[i] & 63) || !((m->gear[i] >> 8) & 3)))) return false;
    if (m->x < 1 || m->x >= WORLD - 1 || m->y < 1 || m->y >= WORLD - 1 || m->level < 1 || m->level > 99 ||
        m->chapter < 1 || m->chapter > 1000000 || m->weapon < 0 || m->weapon > 50 || m->armour < 0 || m->armour > 50 ||
        m->hp < 1 || m->hp > hp_max(m) || m->gold < 0 || m->gold > 10000000 || m->mode < 0 || m->mode > 6 ||
        m->seals < 0 || m->seals > 15 || m->potions < 0 || m->potions > 30 || m->direction < 0 || m->direction > 4 ||
        m->boss < 0 || m->boss > 5 || m->choice < 0 || m->choice >= action_count(m) || m->focus < 0 || m->focus > 2 ||
        (m->mode == 1 && (m->enemy_hp < 1 || m->enemy_max < 1 || m->enemy_hp > m->enemy_max || m->enemy_max > 10000))) return false;
    for (int i = 0; i < 12; i++) if (m->creature_hp[i] > hp_for(m, i)) return false;
    for (int i = 0; i < 3; i++) for (int j = 0; j < i; j++)
        if (m->party[i] != 255 && m->party[i] == m->party[j]) return false;
    s->m = *m;
    s->m.held = 0;
    say(s, m->mode == 1 ? "Riprendi: attacco / scudo / cura" : "Bentornato! La tua avventura continua");
    if (m->mode == 4) preview_drop(s);
    return true;
}
#define RPG_COMMON \
    .instructions = "Trascina; centro: azione", .sky = 0x122034, .ground = 0x122034, \
    .state_size = sizeof(rpg_t), .actor_walk = true, .step = step, .input = input, .draw = draw, \
    .hud = hud, .feedback = feedback, .result = result, .continuous = true, \
    .checkpoint_version = 3, .checkpoint_bytes = sizeof(rpg_model_t), .save = save, .load = load, \
    .help = help, .actor_variant = actor_variant
const mg_game_t mg_petland = {.id = "petland", .title = "Cronache del Faro", .start = start, RPG_COMMON};
const mg_game_t mg_wildbond = {.id = "wildbond", .title = "Legami selvatici", .start = wild_start, RPG_COMMON};
