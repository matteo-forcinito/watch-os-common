/* Read the full OS calendar cache without starting a radio or a sync task. */
#include "giornata.h"
#include "internal.h"
#include "services/storage.h"
#include <stdio.h>
#include <string.h>
typedef struct
{
    uint32_t magic;
    int32_t count;
    int64_t synced;
} cache_header_t;
void gd_sync_now(void)
{
    snprintf(gd_state.message, sizeof(gd_state.message), "Lite: calendari in cache; aggiorna dal sistema completo");
}
void gd_sync_task_start(void)
{
}
void gd_sync_background_tick(void)
{
}
void gd_cache_save(void)
{
}
bool gd_app_is_open(void)
{
    return false;
}
void gd_app_open(bool open)
{
    (void)open;
}
void gd_cache_load(void)
{
    char path[80];
    if (!gd_path("cache.bin", path, sizeof(path)) || !storage_files_acquire())
        return;
    FILE *f = fopen(path, "rb");
    cache_header_t h;
    if (f && fread(&h, sizeof(h), 1, f) == 1 && h.magic == 0x47444331u && h.count >= 0 && h.count <= GD_MAX_ITEMS &&
        fread(gd_cache, sizeof(gd_item_t), (size_t)h.count, f) == (size_t)h.count)
    {
        gd_lock();
        gd_cache_count = h.count;
        gd_state.synced = h.synced;
        snprintf(gd_state.message, sizeof(gd_state.message), "Calendari dalla memoria");
        for (int i = 0; i < gd_source_count(); i++)
        {
            gd_source_t *s = gd_source_at(i);
            if (fread(&s->synced, sizeof(s->synced), 1, f) != 1 || fread(&s->count, sizeof(s->count), 1, f) != 1)
                break;
        }
        gd_unlock();
    }
    if (f)
        fclose(f);
    storage_files_release();
}
