/* Radar picture maths (see compose.h). Freestanding: no libc, no libm. */
#include "compose.h"

static uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void radar_palette_init(radar_palette_t *p)
{
    uint8_t *bytes = (uint8_t *)p;
    for (unsigned i = 0; i < sizeof(*p); i++)
        bytes[i] = 0;
    p->count = 1; /* index 0: transparent */
}

uint8_t radar_palette_index(radar_palette_t *p, uint32_t rgba)
{
    uint32_t alpha = rgba & 0xFF;
    if (alpha < 16)
        return 0;
    uint32_t slot = (rgba * 2654435761u) >> 22; /* 10 bits */
    for (int probe = 0; probe < 1024; probe++, slot = (slot + 1) & 1023)
    {
        uint16_t entry = p->slots[slot];
        if (entry == 0)
        {
            if (p->count >= 256)
                return 0;
            int index = p->count++;
            p->rgba[index] = rgba;
            p->rgb565[index] = rgb565(rgba >> 24, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF);
            p->alpha[index] = (uint8_t)alpha;
            p->slots[slot] = (uint16_t)(index + 1);
            return (uint8_t)index;
        }
        if (p->rgba[entry - 1] == rgba)
            return (uint8_t)(entry - 1);
    }
    return 0;
}

/* Overlap of a source tile with the screen along one axis, in screen pixels. */
static bool span(int32_t tile, int shift, int32_t origin, int size, int *from, int *to)
{
    int32_t start = (tile * 256) << shift, end = (tile * 256 + 256) << shift;
    int32_t a = start - origin, b = end - origin;
    *from = a < 0 ? 0 : (int)a;
    *to = b > size ? size : (int)b;
    return *from < *to;
}

/* OSM colours -> a quiet dark map: luminance inverted into a narrow dark band,
 * water kept slightly blue. Same formula as the prototype that chose it. */
void radar_blit_base(uint16_t *view, int w, int h, int32_t ox, int32_t oy, const uint8_t *tile, int32_t tx,
                     int32_t ty)
{
    int x0, x1, y0, y1;
    if (!span(tx, 0, ox, w, &x0, &x1) || !span(ty, 0, oy, h, &y0, &y1))
        return;
    for (int sy = y0; sy < y1; sy++)
    {
        const uint8_t *row = tile + (size_t)((oy + sy) - ty * 256) * 256 * 4;
        uint16_t *out = view + (size_t)sy * w;
        for (int sx = x0; sx < x1; sx++)
        {
            const uint8_t *px = row + ((ox + sx) - tx * 256) * 4;
            uint32_t r = px[0], g = px[1], b = px[2];
            uint32_t lum = (r * 77 + g * 150 + b * 29) >> 8;
            uint32_t v = 12 + (255 - lum) * 60 / 255;
            bool water = b > r + 20 && b > g;
            out[sx] = water ? rgb565(v / 2 + 6, v / 2 + 12, v + 22) : rgb565(v + 4, v + 6, v + 12);
        }
    }
}

void radar_blit_radar(uint8_t *view, int w, int h, int32_t ox, int32_t oy, int shift, const uint8_t *tile,
                      int32_t tx, int32_t ty, radar_palette_t *p)
{
    int x0, x1, y0, y1;
    if (!span(tx, shift, ox, w, &x0, &x1) || !span(ty, shift, oy, h, &y0, &y1))
        return;
    for (int sy = y0; sy < y1; sy++)
    {
        int src_y = ((oy + sy) >> shift) - ty * 256;
        const uint8_t *row = tile + (size_t)src_y * 256 * 4;
        uint8_t *out = view + (size_t)sy * w;
        uint32_t last_rgba = 0;
        uint8_t last_index = 0;
        for (int sx = x0; sx < x1; sx++)
        {
            const uint8_t *px = row + (((ox + sx) >> shift) - tx * 256) * 4;
            uint32_t rgba = (uint32_t)px[0] << 24 | (uint32_t)px[1] << 16 | (uint32_t)px[2] << 8 | px[3];
            if (rgba != last_rgba || sx == x0)
            {
                last_rgba = rgba;
                last_index = radar_palette_index(p, rgba);
            }
            out[sx] = last_index;
        }
    }
}

void radar_compose_frame(const uint16_t *base, const uint8_t *frame, const radar_palette_t *p, uint16_t *out, int n)
{
    for (int i = 0; i < n; i++)
    {
        uint8_t index = frame[i];
        if (index == 0)
        {
            out[i] = base[i];
            continue;
        }
        uint32_t a = p->alpha[index], fg = p->rgb565[index], bg = base[i];
        /* Blend each channel: light rain stays see-through, heavy rain never hides the map completely. */
        a = 70 + a * 140 / 255;
        uint32_t r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (255 - a)) / 255;
        uint32_t g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (255 - a)) / 255;
        uint32_t b = ((fg & 31) * a + (bg & 31) * (255 - a)) / 255;
        out[i] = (uint16_t)(r << 11 | g << 5 | b);
    }
}

float radar_sqrt(float x)
{
    if (x <= 0)
        return 0;
    float y = x > 1 ? x / 2 : 1;
    for (int i = 0; i < 20; i++)
        y = 0.5f * (y + x / y);
    return y;
}

/* atan(z) for |z| <= 1, max error ~0.3 degrees. */
static float atan_unit(float z)
{
    return z * (45.0f + 15.66f * (1.0f - (z < 0 ? -z : z)));
}

float radar_bearing(float dx, float dy)
{
    /* North = up = negative dy on screen; clockwise. */
    float east = dx, north = -dy;
    float ae = east < 0 ? -east : east, an = north < 0 ? -north : north;
    if (ae < 1e-6f && an < 1e-6f)
        return 0;
    float angle = ae <= an ? atan_unit(ae / an) : 90.0f - atan_unit(an / ae); /* from north toward east, 0..90 */
    if (north >= 0)
        return east >= 0 ? angle : 360.0f - angle;
    return east >= 0 ? 180.0f - angle : 180.0f + angle;
}

bool radar_nearest_rain(const uint8_t *frame, const radar_palette_t *p, int w, int h, int cx, int cy, int min_alpha,
                        float *distance_px, float *bearing_deg)
{
    int32_t best = -1, bx = 0, by = 0;
    for (int y = 0; y < h; y++)
    {
        const uint8_t *row = frame + (size_t)y * w;
        int32_t dy = y - cy;
        for (int x = 0; x < w; x++)
        {
            if (row[x] == 0 || p->alpha[row[x]] < min_alpha)
                continue;
            int32_t dx = x - cx, d = dx * dx + dy * dy;
            if (best < 0 || d < best)
            {
                best = d;
                bx = dx;
                by = dy;
            }
        }
    }
    if (best < 0)
        return false;
    *distance_px = radar_sqrt((float)best);
    *bearing_deg = radar_bearing((float)bx, (float)by);
    return true;
}

float radar_intensity_at(const uint8_t *frame, const radar_palette_t *p, int w, int h, int cx, int cy, int radius)
{
    uint32_t sum = 0, count = 0;
    for (int y = cy - radius; y <= cy + radius; y++)
        for (int x = cx - radius; x <= cx + radius; x++)
        {
            if (x < 0 || y < 0 || x >= w || y >= h || (x - cx) * (x - cx) + (y - cy) * (y - cy) > radius * radius)
                continue;
            uint8_t index = frame[(size_t)y * w + x];
            sum += index ? p->alpha[index] : 0;
            count++;
        }
    return count ? (float)sum / (255.0f * count) : 0;
}
