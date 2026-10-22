/* One tap from the menu. No new shell tile or swipe gesture. */
#include "apps_internal.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "hardware/board.h"
#include "link/link_internal.h"
#include "orbi/orbi_view.h"
#include "pet.h"
#include "ui/ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static lv_obj_t *s_page, *s_hero, *s_name, *s_status, *s_needs;
static orbi_view_t *s_view;
static lv_timer_t *s_timer;
static char s_seed[17];
static pet_record_t s_pet;
static lv_obj_t *s_ambient;
static orbi_view_t *s_small;
static char s_small_seed[17];
static lv_obj_t *s_controls[5];

static orbi_mood_t mood(const pet_record_t *p)
{
    return p->asleep                       ? ORBI_SLEEP
           : p->energy < 20                ? ORBI_SLEEPY
           : p->joy < 25 || p->hunger > 80 ? ORBI_SAD
           : p->joy > 65                   ? ORBI_HAPPY
                                           : ORBI_CALM;
}
static void open(lv_event_t *e)
{
    (void)e;
    app_open("orbi", NULL);
}
static void command(lv_event_t *e)
{
    const char *action = lv_event_get_user_data(e);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "action", action);
    cJSON_AddStringToObject(j, "seed", s_seed);
    if (!strcmp(action, "company"))
        cJSON_AddBoolToObject(j, "on", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
    const char *error;
    cJSON *out = pet_command(j, &error);
    if (!out)
        ui_toast(error);
    else if (s_view)
        orbi_view_poke(s_view);
    cJSON_Delete(j);
    cJSON_Delete(out);
    if (s_timer)
        lv_timer_ready(s_timer);
}
static void next(lv_event_t *e)
{
    (void)e;
    cJSON *out = pet_snapshot();
    cJSON *list = cJSON_GetObjectItemCaseSensitive(out, "pets");
    cJSON *p;
    char first[17] = "", choice[17] = "";
    bool seen = false;
    cJSON_ArrayForEach(p, list)
    {
        if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(p, "stage")) != PET_ACTIVE)
            continue;
        const char *seed = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "seed"));
        if (!first[0])
            snprintf(first, sizeof(first), "%s", seed);
        if (seen && !choice[0])
            snprintf(choice, sizeof(choice), "%s", seed);
        if (!strcmp(seed, s_seed))
            seen = true;
    }
    if (choice[0] || first[0])
        pet_select(choice[0] ? choice : first);
    cJSON_Delete(out);
    if (s_timer)
        lv_timer_ready(s_timer);
}
static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if (!pet_current(&s_pet))
    {
        for (int i = 0; i < 5; i++)
            lv_obj_set_state(s_controls[i], LV_STATE_DISABLED, true);
        lv_label_set_text(s_name, "Il tuo child è altrove");
        lv_label_set_text(s_status, "Apri Orbit per ritrovarlo o completare un viaggio.");
        lv_label_set_text(s_needs, "");
        if (s_view)
        {
            orbi_view_delete(s_view);
            s_view = NULL;
            s_seed[0] = 0;
        }
        return;
    }
    uint64_t seed;
    orbi_parse(s_pet.seed, &seed);
    orbi_genome_t g;
    orbi_genome(seed, &g);
    if (!s_view || strcmp(s_seed, s_pet.seed))
    {
        if (s_view)
            orbi_view_delete(s_view);
        snprintf(s_seed, sizeof(s_seed), "%s", s_pet.seed);
        int size = board_info()->width * 28 / 100;
        if (size > 140)
            size = 140;
        s_view = orbi_view_create(s_hero, &g, size);
        if (s_view)
            lv_obj_center(orbi_view_root(s_view));
    }
    if (s_view)
        orbi_view_mood(s_view, mood(&s_pet));
    for (int i = 0; i < 5; i++)
        lv_obj_set_state(s_controls[i], LV_STATE_DISABLED, false);
    lv_obj_set_state(s_controls[2], LV_STATE_DISABLED, s_pet.asleep != 0);
    lv_obj_set_state(s_controls[3], LV_STATE_DISABLED, s_pet.asleep != 0 || s_pet.energy < 10);
    lv_label_set_text_fmt(s_name, "%s  /  LV %d", g.name, 1 + (int)sqrt((double)s_pet.xp / 40));
    int days = s_pet.born > 0 ? (int)(((int64_t)time(NULL) - s_pet.born) / 86400) : 0;
    if (days < 0)
        days = 0;
    const char *stage = days < 1 ? "Cucciolo" : days < 7 ? "Giovane" : "Adulto";
    lv_label_set_text_fmt(s_status, "%s - nato su %s\n%s", stage, s_pet.origin,
                          s_pet.asleep        ? "Sta riposando"
                          : s_pet.hunger > 80 ? "Ha fame"
                          : s_pet.energy < 20 ? "Ha bisogno di riposo"
                                              : "Toccalo per una coccola");
    lv_label_set_text_fmt(s_needs, "Sazieta %d%%   Energia %d%%\nGioia %d%%", (int)(100 - s_pet.hunger),
                          (int)s_pet.energy, (int)s_pet.joy);
}
static lv_obj_t *label(lv_obj_t *p, uint32_t color)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, ui_color(color), 0);
    lv_obj_set_style_text_font(l, UI_FONT_SMALL, 0);
    return l;
}
static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    if (!pet_init())
        ui_toast("Archivio child non disponibile: riprova");
    lv_obj_set_style_bg_color(screen, ui_color(0x060B12), 0);
    s_page = ui_page(screen, "Orbi / il tuo child");
    s_name = label(s_page, 0xEEF4FB);
    lv_obj_set_style_text_font(s_name, ui_font_title, 0);
    s_hero = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_hero);
    int w = board_info()->width;
    lv_obj_set_size(s_hero, LV_PCT(100), w * 65 / 100);
    lv_obj_set_scrollable(s_hero, false);
    lv_obj_set_style_bg_color(s_hero, ui_color(0x111A26), 0);
    lv_obj_set_style_bg_opa(s_hero, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_hero, 24, 0);
    lv_obj_set_style_border_width(s_hero, 1, 0);
    lv_obj_set_style_border_color(s_hero, ui_color(0x34C6F4), 0);
    lv_obj_set_clickable(s_hero, true);
    lv_obj_add_event_cb(s_hero, command, LV_EVENT_SHORT_CLICKED, "pet");
    lv_port_set_focusable(s_hero);
    s_status = label(s_page, 0x8B9AB0);
    s_needs = label(s_page, 0x34C6F4);
    s_controls[0] = s_hero;
    s_controls[1] = ui_button(s_page, "Coccola", 0x24415C, command, "pet");
    s_controls[2] = ui_button(s_page, "Dai da mangiare", 0x245544, command, "feed");
    s_controls[3] = ui_button(s_page, "Gioca", 0x493663, command, "play");
    s_controls[4] = ui_button(s_page, "Nanna / sveglia", 0x26364D, command, "sleep");
    ui_button(s_page, "Altri child sul device", 0x26364D, next, NULL);
    ui_switch_row(s_page, LV_SYMBOL_EYE_OPEN, UI_COLOR_BLUE, "Piccola compagnia", pet_company(), command, "company");
    ui_text(s_page,
            "Per farlo viaggiare: Orbit > Ecosistema > Children. Il suo aspetto e i suoi progressi viaggiano con lui.",
            true);
    s_seed[0] = 0;
    s_timer = lv_timer_create(refresh, 2000, NULL);
    refresh(NULL);
}
static void destroy(void)
{
    if (s_timer)
    {
        lv_timer_delete(s_timer);
    }
    s_timer = NULL;
    s_view = NULL;
    s_page = NULL;
    s_seed[0] = 0;
}
const app_t pet_app = {.id = "orbi",
                       .name = "Orbi / Children",
                       .icon = LV_SYMBOL_IMAGE,
                       .color = 0x34C6F4,
                       .flags = APP_FLAG_HIDDEN,
                       .create = create,
                       .destroy = destroy,
                       .about = "Il child nato su questo device. Coccole, cibo, gioco e viaggi con Orbit."};

void pet_cta(lv_obj_t *parent)
{
    pet_record_t p;
    lv_obj_t *card = lv_obj_create(parent);
    ui_make_card(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, 94);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, true);
    lv_port_set_focusable(card);
    lv_obj_add_event_cb(card, open, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_style_text_font(title, UI_FONT_BODY, 0);
    lv_obj_set_width(title, LV_PCT(65));
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(title, LV_ALIGN_RIGHT_MID, 0, 0);
    if (pet_current(&p))
    {
        uint64_t seed;
        orbi_parse(p.seed, &seed);
        orbi_genome_t g;
        orbi_genome(seed, &g);
        lv_label_set_text_fmt(title, "%s\nApri il tuo child  >", g.name);
        orbi_view_t *v = orbi_view_create(card, &g, 34);
        if (v)
        {
            orbi_view_static(v);
            lv_obj_align(orbi_view_root(v), LV_ALIGN_LEFT_MID, 2, 0);
        }
    }
    else
        lv_label_set_text(title, "Children\nApri  >");
}
void pet_thumbnail(lv_obj_t *parent, int width)
{
    pet_record_t p;
    if (!pet_current(&p))
        return;
    uint64_t seed;
    orbi_parse(p.seed, &seed);
    orbi_genome_t g;
    orbi_genome(seed, &g);
    lv_obj_clean(parent);
    orbi_view_t *v = orbi_view_create(parent, &g, width);
    if (v)
    {
        orbi_view_static(v);
        lv_obj_center(orbi_view_root(v));
    }
}
static void ambient(lv_timer_t *timer)
{
    (void)timer;
    pet_record_t p;
    const app_t *app = app_current();
    bool show = pet_company() && power_screen() == POWER_SCREEN_ON &&
                (!app || (app != &pet_app && app != &ring_app && app != &call_app)) && !link_scene_busy() &&
                pet_current(&p);
    if (!show)
    {
        if (s_ambient)
            lv_obj_set_hidden(s_ambient, true);
        return;
    }
    if (!s_ambient || strcmp(s_small_seed, p.seed))
    {
        if (s_ambient)
            lv_obj_delete(s_ambient);
        s_ambient = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_ambient);
        int size = board_info()->width * 13 / 100;
        if (size > 60)
            size = 60;
        if (size < 28)
            size = 28;
        lv_obj_set_size(s_ambient, size * 3 / 2, size * 2);
        lv_obj_set_scrollable(s_ambient, false);
        /* Decorative only: cannot intercept a swipe or a button underneath. */
        lv_obj_set_clickable(s_ambient, false);
        uint64_t seed;
        orbi_parse(p.seed, &seed);
        orbi_genome_t g;
        orbi_genome(seed, &g);
        s_small = orbi_view_create(s_ambient, &g, size);
        if (s_small)
            lv_obj_center(orbi_view_root(s_small));
        snprintf(s_small_seed, sizeof(s_small_seed), "%s", s_small ? p.seed : "");
        int inset = board_info()->round ? board_info()->width / 7 : 6;
        lv_obj_align(s_ambient, LV_ALIGN_BOTTOM_RIGHT, -inset, -inset);
    }
    lv_obj_set_hidden(s_ambient, false);
    if (s_small)
        orbi_view_mood(s_small, mood(&p));
}
void pet_companion_start(void)
{
    lv_timer_create(ambient, 1500, NULL);
}
