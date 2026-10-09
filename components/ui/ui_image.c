#include "ui/ui.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * Image sources from the SD card, including the icons of the old Arduino
 * launcher.
 *
 * Those icon.bin files are in the LVGL 8 format (a 4-byte header, then raw
 * pixels), which LVGL 9 cannot read: its .bin files start with a 0x19 magic
 * byte and a 12-byte header. LVGL 8 files are converted here once into an
 * LVGL 9 image in PSRAM and cached; LVGL 9 .bin and PNG files are passed
 * through to LVGL's own decoders.
 */

static const char *TAG = "ui_image";

#define CACHE_MAX      32
#define PATH_MAX_LEN   160
#define V9_MAGIC       0x19
#define MAX_SIDE       256

/* LVGL 8 color formats (lv_img_cf_t, 16-bit color depth). */
#define V8_TRUE_COLOR               4
#define V8_TRUE_COLOR_ALPHA         5
#define V8_TRUE_COLOR_CHROMA_KEYED  6
#define V8_INDEXED_1BIT             7
#define V8_INDEXED_8BIT             10

typedef struct
{
    char path[PATH_MAX_LEN];
    lv_image_dsc_t *dsc;     /* NULL: use the path as is */
} cache_entry_t;

static cache_entry_t s_cache[CACHE_MAX];
static int s_cache_count = 0;

/* "S:/apps/x/icon.bin" -> "/sdcard/apps/x/icon.bin" */
static void to_vfs_path(const char *lv_path, char *out, size_t size)
{
    if (strncmp(lv_path, "S:", 2) == 0)
    {
        snprintf(out, size, "/sdcard%s", lv_path + 2);
    }
    else
    {
        snprintf(out, size, "%s", lv_path);
    }
}

static lv_image_dsc_t *new_dsc(lv_color_format_t cf, uint32_t w, uint32_t h, uint32_t stride, uint32_t data_size)
{
    lv_image_dsc_t *dsc = heap_caps_calloc(1, sizeof(lv_image_dsc_t), MALLOC_CAP_SPIRAM);
    uint8_t *data = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM);

    if (dsc == NULL || data == NULL)
    {
        free(dsc);
        free(data);
        return NULL;
    }

    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = cf;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = stride;
    dsc->data_size = data_size;
    dsc->data = data;

    return dsc;
}

static lv_image_dsc_t *load_v8(FILE *file, uint32_t header)
{
    uint32_t cf = header & 0x1F;
    uint32_t w = (header >> 10) & 0x7FF;
    uint32_t h = (header >> 21) & 0x7FF;

    if (w == 0 || h == 0 || w > MAX_SIDE || h > MAX_SIDE)
    {
        return NULL;
    }

    uint32_t pixels = w * h;
    lv_image_dsc_t *dsc = NULL;

    if (cf == V8_TRUE_COLOR || cf == V8_TRUE_COLOR_CHROMA_KEYED)
    {
        /* RGB565 pixels; chroma key (pure green) becomes transparent -> RGB565A8. */
        dsc = new_dsc(LV_COLOR_FORMAT_RGB565A8, w, h, w * 2, pixels * 3);

        if (dsc != NULL)
        {
            uint8_t *rgb = (uint8_t *)dsc->data;
            uint8_t *alpha = rgb + pixels * 2;

            if (fread(rgb, 2, pixels, file) != pixels)
            {
                goto fail;
            }

            for (uint32_t i = 0; i < pixels; i++)
            {
                uint16_t c = (uint16_t)(rgb[2 * i] | (rgb[2 * i + 1] << 8));
                alpha[i] = (cf == V8_TRUE_COLOR_CHROMA_KEYED && c == 0x07E0) ? 0 : 0xFF;
            }
        }
    }
    else if (cf == V8_TRUE_COLOR_ALPHA)
    {
        /* Interleaved RGB565 + A8 (3 bytes) -> LVGL 9 planar RGB565A8. */
        dsc = new_dsc(LV_COLOR_FORMAT_RGB565A8, w, h, w * 2, pixels * 3);
        uint8_t *raw = heap_caps_malloc(pixels * 3, MALLOC_CAP_SPIRAM);

        if (dsc == NULL || raw == NULL || fread(raw, 3, pixels, file) != pixels)
        {
            free(raw);
            goto fail;
        }

        uint8_t *rgb = (uint8_t *)dsc->data;
        uint8_t *alpha = rgb + pixels * 2;

        for (uint32_t i = 0; i < pixels; i++)
        {
            rgb[2 * i] = raw[3 * i];
            rgb[2 * i + 1] = raw[3 * i + 1];
            alpha[i] = raw[3 * i + 2];
        }

        free(raw);
    }
    else if (cf >= V8_INDEXED_1BIT && cf <= V8_INDEXED_8BIT)
    {
        /* Palette (ARGB8888 entries) + packed indices -> ARGB8888. */
        uint32_t bpp = 1u << (cf - V8_INDEXED_1BIT);
        uint32_t colors = 1u << bpp;
        uint32_t row_bytes = (w * bpp + 7) / 8;
        uint32_t palette[256];
        uint8_t *row = malloc(row_bytes);

        dsc = new_dsc(LV_COLOR_FORMAT_ARGB8888, w, h, w * 4, pixels * 4);

        if (dsc == NULL || row == NULL || fread(palette, 4, colors, file) != colors)
        {
            free(row);
            goto fail;
        }

        uint32_t *out = (uint32_t *)dsc->data;

        for (uint32_t y = 0; y < h; y++)
        {
            if (fread(row, 1, row_bytes, file) != row_bytes)
            {
                free(row);
                goto fail;
            }

            for (uint32_t x = 0; x < w; x++)
            {
                uint32_t bit = x * bpp;
                uint32_t index = (row[bit / 8] >> (8 - bpp - (bit % 8))) & (colors - 1);
                out[y * w + x] = palette[index];
            }
        }

        free(row);
    }

    return dsc;

fail:
    if (dsc != NULL)
    {
        free((void *)dsc->data);
        free(dsc);
    }

    return NULL;
}

const void *ui_image_src(const char *path)
{
    if (path == NULL || path[0] == '\0')
    {
        return NULL;
    }

    for (int i = 0; i < s_cache_count; i++)
    {
        if (strcmp(s_cache[i].path, path) == 0)
        {
            return s_cache[i].dsc != NULL ? (const void *)s_cache[i].dsc : (const void *)s_cache[i].path;
        }
    }

    const char *ext = strrchr(path, '.');
    lv_image_dsc_t *dsc = NULL;
    bool ok = true;

    if (ext != NULL && strcasecmp(ext, ".bin") == 0)
    {
        char vfs[PATH_MAX_LEN + 8];
        to_vfs_path(path, vfs, sizeof(vfs));
        FILE *file = fopen(vfs, "rb");
        uint8_t head[4];

        if (file == NULL || fread(head, 1, 4, file) != 4)
        {
            ok = false;
        }
        else if (head[0] != V9_MAGIC)
        {
            dsc = load_v8(file, (uint32_t)(head[0] | (head[1] << 8) | (head[2] << 16) | ((uint32_t)head[3] << 24)));
            ok = dsc != NULL;
            ESP_LOGI(TAG, "%s: LVGL 8 icon %s", path, ok ? "converted" : "not supported");
        }

        if (file != NULL)
        {
            fclose(file);
        }
    }

    if (!ok)
    {
        return NULL;
    }

    /* Remember it (the oldest entry is replaced when full; its image stays valid). */
    int slot = s_cache_count < CACHE_MAX ? s_cache_count++ : 0;
    snprintf(s_cache[slot].path, sizeof(s_cache[slot].path), "%s", path);
    s_cache[slot].dsc = dsc;

    return dsc != NULL ? (const void *)dsc : (const void *)s_cache[slot].path;
}
