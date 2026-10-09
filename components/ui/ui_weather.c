#include "ui/ui.h"

/*
 * Weather icons made of simple shapes: circles for the sun, the moon and the
 * clouds, short lines for rain, snow and lightning. They scale to any size
 * and need no image files.
 */

typedef enum
{
    SKY_CLEAR,
    SKY_PARTLY,
    SKY_CLOUDY,
    SKY_FOG,
    SKY_DRIZZLE,
    SKY_RAIN,
    SKY_SNOW,
    SKY_STORM,
} sky_t;

static sky_t sky_from_wmo(int code)
{
    if (code == 0)
    {
        return SKY_CLEAR;
    }

    if (code <= 2)
    {
        return SKY_PARTLY;
    }

    if (code == 3)
    {
        return SKY_CLOUDY;
    }

    if (code == 45 || code == 48)
    {
        return SKY_FOG;
    }

    if (code >= 51 && code <= 57)
    {
        return SKY_DRIZZLE;
    }

    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82))
    {
        return SKY_RAIN;
    }

    if ((code >= 71 && code <= 77) || code == 85 || code == 86)
    {
        return SKY_SNOW;
    }

    if (code >= 95)
    {
        return SKY_STORM;
    }

    return SKY_CLOUDY;
}

const char *ui_weather_text(int code)
{
    switch (sky_from_wmo(code))
    {
        case SKY_CLEAR:   return "Sereno";
        case SKY_PARTLY:  return code == 1 ? "Poco nuvoloso" : "Parz. nuvoloso";
        case SKY_CLOUDY:  return "Nuvoloso";
        case SKY_FOG:     return "Nebbia";
        case SKY_DRIZZLE: return "Pioviggine";
        case SKY_RAIN:    return code >= 80 ? "Rovesci" : "Pioggia";
        case SKY_SNOW:    return "Neve";
        case SKY_STORM:   return "Temporale";
        default:          return "";
    }
}

static lv_obj_t *circle(lv_obj_t *parent, int32_t x, int32_t y, int32_t d, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, d, d);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c, opa, 0);
    lv_obj_set_clickable(c, false);
    return c;
}

static void bar(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    lv_obj_t *b = circle(parent, x, y, w, color, LV_OPA_COVER);
    lv_obj_set_size(b, w, h);
}

/* A cloud: three overlapping circles on a rounded base, in a box of s x s. */
static void cloud(lv_obj_t *parent, int32_t ox, int32_t oy, int32_t s, uint32_t color)
{
    circle(parent, ox + s * 10 / 100, oy + s * 40 / 100, s * 38 / 100, color, LV_OPA_COVER);
    circle(parent, ox + s * 28 / 100, oy + s * 20 / 100, s * 50 / 100, color, LV_OPA_COVER);
    circle(parent, ox + s * 54 / 100, oy + s * 34 / 100, s * 40 / 100, color, LV_OPA_COVER);
    bar(parent, ox + s * 18 / 100, oy + s * 52 / 100, s * 66 / 100, s * 26 / 100, color);
}

static void sun(lv_obj_t *parent, int32_t ox, int32_t oy, int32_t d)
{
    circle(parent, ox - d * 18 / 100, oy - d * 18 / 100, d * 136 / 100, UI_COLOR_YELLOW, LV_OPA_20);   /* glow */
    circle(parent, ox, oy, d, UI_COLOR_YELLOW, LV_OPA_COVER);
}

static void moon(lv_obj_t *parent, int32_t ox, int32_t oy, int32_t d)
{
    circle(parent, ox, oy, d, 0xE5E5EA, LV_OPA_COVER);
    circle(parent, ox + d * 35 / 100, oy - d * 12 / 100, d * 82 / 100, UI_COLOR_BG, LV_OPA_COVER);
}

lv_obj_t *ui_weather_icon(lv_obj_t *parent, int wmo_code, bool night, int32_t s)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, s, s);
    lv_obj_set_clickable(box, false);
    lv_obj_set_scrollable(box, false);

    sky_t sky = sky_from_wmo(wmo_code);
    uint32_t cloud_color = (sky == SKY_STORM || sky == SKY_RAIN) ? 0xAEAEB2 : 0xF2F2F7;

    switch (sky)
    {
        case SKY_CLEAR:
            if (night)
            {
                moon(box, s * 20 / 100, s * 20 / 100, s * 60 / 100);
            }
            else
            {
                sun(box, s * 22 / 100, s * 22 / 100, s * 56 / 100);
            }
            break;

        case SKY_PARTLY:
            if (night)
            {
                moon(box, s * 8 / 100, s * 6 / 100, s * 46 / 100);
            }
            else
            {
                sun(box, s * 12 / 100, s * 8 / 100, s * 44 / 100);
            }

            cloud(box, s * 10 / 100, s * 18 / 100, s * 88 / 100, cloud_color);
            break;

        case SKY_FOG:
            cloud(box, s * 6 / 100, 0, s * 88 / 100, 0xC7C7CC);

            for (int i = 0; i < 3; i++)
            {
                bar(box, s * (12 + i * 6) / 100, s * (70 + i * 10) / 100, s * (72 - i * 12) / 100, s * 6 / 100, 0x8E8E93);
            }
            break;

        case SKY_DRIZZLE:
        case SKY_RAIN:
        case SKY_SNOW:
        case SKY_STORM:
            cloud(box, s * 6 / 100, 0, s * 88 / 100, cloud_color);

            for (int i = 0; i < 3; i++)
            {
                int32_t x = s * (26 + i * 20) / 100;
                int32_t y = s * 76 / 100;

                if (sky == SKY_SNOW)
                {
                    circle(box, x, y, s * 10 / 100, 0xFFFFFF, LV_OPA_COVER);
                }
                else if (sky == SKY_STORM && i == 1)
                {
                    bar(box, x, y - s * 6 / 100, s * 8 / 100, s * 24 / 100, UI_COLOR_YELLOW);
                }
                else
                {
                    int32_t len = sky == SKY_DRIZZLE ? 10 : 18;
                    bar(box, x, y, s * 6 / 100, s * len / 100, UI_COLOR_BLUE);
                }
            }
            break;

        case SKY_CLOUDY:
        default:
            cloud(box, s * 4 / 100, s * 10 / 100, s * 92 / 100, cloud_color);
            break;
    }

    return box;
}
