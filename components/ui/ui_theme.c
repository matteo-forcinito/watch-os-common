#include "ui/ui.h"

#include "core/lv_port.h"
#include "core/settings.h"

#include "esp_log.h"

static const char *TAG = "ui";

/* Montserrat TTF subsets embedded in the firmware (components/ui/fonts). */
extern const uint8_t light_ttf_start[] asm("_binary_montserrat_light_ttf_start");
extern const uint8_t light_ttf_end[] asm("_binary_montserrat_light_ttf_end");
extern const uint8_t semibold_ttf_start[] asm("_binary_montserrat_semibold_ttf_start");
extern const uint8_t semibold_ttf_end[] asm("_binary_montserrat_semibold_ttf_end");
extern const uint8_t latin_ttf_start[] asm("_binary_montserrat_medium_latin_ttf_start");
extern const uint8_t latin_ttf_end[] asm("_binary_montserrat_medium_latin_ttf_end");

const lv_font_t *ui_font_small = &lv_font_montserrat_16;
const lv_font_t *ui_font_body = &lv_font_montserrat_20;
const lv_font_t *ui_font_large = &lv_font_montserrat_24;

/* RAM copies of the built-in fonts, so they can get a fallback. */
static lv_font_t s_small;
static lv_font_t s_body;
static lv_font_t s_large;

const lv_font_t *ui_font_clock = &lv_font_montserrat_48;
const lv_font_t *ui_font_big = &lv_font_montserrat_48;
const lv_font_t *ui_font_title = &lv_font_montserrat_28;
const lv_font_t *ui_font_headline = &lv_font_montserrat_32;

void ui_register_keyboard_app(void);

/*
 * Big text is rendered from TTF at run time (Tiny TTF): any size, crisp,
 * and the glyphs are cached in PSRAM after the first use. Small text uses
 * LVGL's pre-rendered Montserrat (fastest).
 */
static const lv_font_t *ttf(const uint8_t *start, const uint8_t *end, int size, const lv_font_t *fallback)
{
    lv_font_t *font = lv_tiny_ttf_create_data(start, (size_t)(end - start), size);

    if (font == NULL)
    {
        ESP_LOGW(TAG, "TTF font %d px failed, using built-in", size);
        return fallback;
    }

    font->fallback = ui_font_body;   /* symbols and missing glyphs */
    return font;
}

uint32_t ui_accent_hex(void)
{
    return settings_accent_hex();
}

lv_color_t ui_accent(void)
{
    return lv_color_hex(ui_accent_hex());
}

bool ui_motion_paused(void)
{
    if (lv_display_get_screen_loading(lv_display_get_default()) != NULL)
    {
        return true;   /* a slide is running: it gets the whole frame time */
    }

    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev != NULL; indev = lv_indev_get_next(indev))
    {
        if (lv_indev_get_scroll_obj(indev) != NULL)
        {
            return true;   /* the user is dragging a page or the tileview */
        }
    }

    return false;
}

/* Built-in font + TTF fallback for the non-ASCII characters. */
static const lv_font_t *with_accents(lv_font_t *copy, const lv_font_t *builtin, int size)
{
    *copy = *builtin;
    lv_font_t *latin = lv_tiny_ttf_create_data(latin_ttf_start, (size_t)(latin_ttf_end - latin_ttf_start), size);

    if (latin == NULL)
    {
        return builtin;
    }

    copy->fallback = latin;
    return copy;
}

void ui_init(void)
{
    ui_font_small = with_accents(&s_small, &lv_font_montserrat_16, 16);
    ui_font_body = with_accents(&s_body, &lv_font_montserrat_20, 20);
    ui_font_large = with_accents(&s_large, &lv_font_montserrat_24, 24);

    ui_font_clock = ttf(light_ttf_start, light_ttf_end, 124, &lv_font_montserrat_48);
    ui_font_big = ttf(light_ttf_start, light_ttf_end, 64, &lv_font_montserrat_48);
    ui_font_title = ttf(semibold_ttf_start, semibold_ttf_end, 30, &lv_font_montserrat_28);
    ui_font_headline = ttf(semibold_ttf_start, semibold_ttf_end, 38, &lv_font_montserrat_32);

    lv_display_t *disp = lv_port_display();
    lv_theme_t *theme = lv_theme_default_init(disp, ui_accent(), lv_color_hex(TK_MAGENTA), true, UI_FONT_BODY);
    lv_display_set_theme(disp, theme);
    lv_port_set_focus_color(ui_accent());

    lv_obj_set_style_bg_color(lv_layer_bottom(), lv_color_hex(TK_VOID), 0);
    lv_obj_set_style_bg_opa(lv_layer_bottom(), LV_OPA_COVER, 0);

    ui_register_keyboard_app();
}

const char *ui_battery_symbol(int percent)
{
    if (percent < 0)
    {
        return LV_SYMBOL_BATTERY_EMPTY;
    }

    if (percent > 85)
    {
        return LV_SYMBOL_BATTERY_FULL;
    }

    if (percent > 60)
    {
        return LV_SYMBOL_BATTERY_3;
    }

    if (percent > 35)
    {
        return LV_SYMBOL_BATTERY_2;
    }

    if (percent > 12)
    {
        return LV_SYMBOL_BATTERY_1;
    }

    return LV_SYMBOL_BATTERY_EMPTY;
}
