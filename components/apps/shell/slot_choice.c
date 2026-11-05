/*
 * "Dove installare?": the app-bay slot picker shared by the Market (install
 * after a download) and the launcher (install a card app offline). Automatic
 * first, then every bay slot with its size and the app it holds now; slots too
 * small for the image are disabled. One picker at a time, on the top layer.
 */
#include "apps_internal.h"

#include "services/extapp.h"
#include "ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOT_CHOICES 8

typedef struct
{
    char id[32];
    char label[SLOT_CHOICES][17];
    slot_choice_cb_t done;
    lv_obj_t *overlay;
} slot_choice_t;

static slot_choice_t *s_choice;

bool slot_choice_active(void)
{
    return s_choice != NULL;
}

void slot_choice_close(void)
{
    if (s_choice != NULL)
    {
        lv_obj_delete(s_choice->overlay);
        free(s_choice);
        s_choice = NULL;
    }
}

static void on_choice(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_choice == NULL)
    {
        return;
    }
    char id[32], slot[17];
    snprintf(id, sizeof(id), "%s", s_choice->id);
    snprintf(slot, sizeof(slot), "%s", index >= 0 ? s_choice->label[index] : "");
    slot_choice_cb_t done = s_choice->done;
    slot_choice_close();
    if (done != NULL)
    {
        done(id, index == -2 ? NULL : slot);   /* -2 = Annulla, -1 = automatic ("") */
    }
}

void slot_choice_open(const char *id, size_t size, slot_choice_cb_t done)
{
    slot_choice_close();
    s_choice = calloc(1, sizeof(*s_choice));
    if (s_choice == NULL)
    {
        if (done != NULL) done(id, "");
        return;
    }
    snprintf(s_choice->id, sizeof(s_choice->id), "%s", id);
    s_choice->done = done;

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    s_choice->overlay = overlay;
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(overlay, 8, 0);

    lv_obj_t *title = lv_label_create(overlay);
    lv_obj_set_style_text_font(title, ui_font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Dove installare?");

    char text[64];
    int count = extapp_bay_size() < SLOT_CHOICES ? extapp_bay_size() : SLOT_CHOICES;
    for (int i = -1; i < count; i++)
    {
        bool fits = true;
        if (i < 0)
        {
            snprintf(text, sizeof(text), "Automatica");
        }
        else
        {
            const esp_partition_t *slot = extapp_bay_slot(i);
            extapp_t held;
            snprintf(s_choice->label[i], sizeof(s_choice->label[i]), "%s", slot->label);
            fits = size <= slot->size;
            bool busy = extapp_slot_read(slot, &held);
            snprintf(text, sizeof(text), "%s · %u MB · %s", slot->label, (unsigned)(slot->size >> 20),
                     !fits ? "troppo piccola" : !busy ? "libera" : strcasecmp(held.id, id) == 0 ? "già qui" : held.name);
        }
        lv_obj_t *button = lv_button_create(overlay);
        lv_obj_set_width(button, LV_PCT(78));
        lv_obj_set_style_radius(button, 22, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(i < 0 ? UI_COLOR_GREEN : 0x1E2433), 0);
        lv_obj_t *caption = lv_label_create(button);
        lv_obj_set_style_text_font(caption, UI_FONT_SMALL, 0);
        lv_label_set_text(caption, text);
        lv_obj_center(caption);
        if (fits) lv_obj_add_event_cb(button, on_choice, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        else lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    lv_obj_t *cancel = lv_button_create(overlay);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
    lv_obj_t *caption = lv_label_create(cancel);
    lv_obj_set_style_text_font(caption, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(caption, lv_color_hex(0x9AA3B5), 0);
    lv_label_set_text(caption, "Annulla");
    lv_obj_add_event_cb(cancel, on_choice, LV_EVENT_CLICKED, (void *)(intptr_t)-2);
}
