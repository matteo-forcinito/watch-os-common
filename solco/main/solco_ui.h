#ifndef SOLCO_UI_H
#define SOLCO_UI_H
/*
 * Solco UI: the live views, the MASTER sheet and Studio.
 *
 *   DECKS  (home)   both decks mirrored around the crossfader
 *   FOCUS  (deck)   one deck: big waveform, pitch, loop and transport rows
 *   JOG             vinyl platter for scratching (created on first open)
 *   LIBRARY         track browser for one deck (created on first open)
 *   MASTER  sheet   volume, output routing, Studio, exit (BOOT click)
 *   STUDIO          drum pads / instruments / motion
 *
 * Everything runs in the lvgl task.
 */
#include "hardware/button.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

enum
{
    SOLCO_VIEW_DECKS,
    SOLCO_VIEW_FOCUS,
    SOLCO_VIEW_JOG,
    SOLCO_VIEW_LIBRARY,
    SOLCO_VIEW_STUDIO,
};

/* Output routing chosen in MASTER (effective once the kit link exists). */
enum
{
    SOLCO_OUT_ALL,    /* every device of the kit plays */
    SOLCO_OUT_HUB,    /* only the hub (default for a DJ set) */
    SOLCO_OUT_FOLLOW, /* the device that mixes last */
};

typedef struct
{
    int width, height;
    bool landscape;
    int view, deck, studio;
    bool master, studio_tools;
} solco_ui_info_t;

void solco_ui_create(void);
void solco_ui_show(int view, int deck);
void solco_ui_info(solco_ui_info_t *info);
/* BOOT/PWR keys while Solco runs (installed over the app manager handler). */
void solco_ui_button(button_id_t button, button_event_t event);
bool solco_ui_home_back(void);
/* Master volume with the source of the last change (diagnostics). */
void solco_ui_set_volume(int value, const char *source);
const char *solco_ui_volume_source(uint32_t *changes);
bool solco_ui_wheel_vinyl(void);

/* ---- shared between the view files */
lv_obj_t *solco_ui_root(void);
void solco_master_create(lv_obj_t *root);
void solco_master_toggle(void);
bool solco_master_open(void);
void solco_master_close(void);
void solco_master_refresh(void);
void solco_master_knob(int delta);
int solco_output_mode(void);
/* Kit link (MASTER > KIT): short state tag ("" offline) and the live-tick hook. */
const char *solco_kit_tag(void);
void solco_kit_tick(void);

void solco_studio_create(lv_obj_t *root);
void solco_studio_open(int view); /* -1 closes */
int solco_studio_view(void);
bool solco_studio_tools_open(void);
void solco_studio_refresh(void);
/* Studio tells the live views when it takes or leaves the screen. */
void solco_ui_studio_opened(void);
void solco_ui_studio_closed(void);

#endif
