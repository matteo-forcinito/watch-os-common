/*
 * LVGL memory in PSRAM.
 *
 * Every LVGL widget, style, font glyph and layer is a heap allocation, most
 * of them small. With the default allocator the small ones land in the
 * ~300 KB of internal RAM, which Wi-Fi, Bluetooth, TLS and every task stack
 * also need: a full UI used to starve them (BLE "Malloc failed", tasks not
 * created). Here LVGL gets the 8 MB PSRAM instead, and uses internal RAM
 * only if the PSRAM is full.
 *
 * Selected with CONFIG_LV_USE_CUSTOM_MALLOC (see sdkconfig.defaults).
 */

#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include "esp_heap_caps.h"

#define PSRAM_CAPS    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define FALLBACK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, PSRAM_CAPS);
    return p != NULL ? p : heap_caps_malloc(size, FALLBACK_CAPS);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, PSRAM_CAPS);
    return n != NULL ? n : heap_caps_realloc(p, new_size, FALLBACK_CAPS);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, PSRAM_CAPS);

    mon_p->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon_p->free_size = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->free_cnt = info.free_blocks;
    mon_p->used_cnt = info.allocated_blocks;
    mon_p->max_used = mon_p->total_size - info.minimum_free_bytes;
    mon_p->used_pct = mon_p->total_size ? (uint8_t)(100 - (info.total_free_bytes * 100) / mon_p->total_size) : 0;
    mon_p->frag_pct = info.total_free_bytes ? (uint8_t)(100 - (info.largest_free_block * 100) / info.total_free_bytes) : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity_all(true) ? LV_RESULT_OK : LV_RESULT_INVALID;
}

#endif
