#ifndef GIORNATA_INTERNAL_H
#define GIORNATA_INTERNAL_H

/* Shared between Giornata's modules only (all under gd_lock unless noted). */

#include "giornata.h"

#include <stddef.h>

extern gd_item_t *gd_cache;          /* calendar occurrences from the last sync */
extern int gd_cache_count;
extern gd_status_t gd_state;

void gd_changed(bool save);
/* Copy cut at a UTF-8 character boundary. */
void gd_copy(char *out, size_t size, const char *in);
bool gd_collect_own(const ics_occurrence_t *occurrence, void *ctx);
void gd_task_credit(uint32_t id, int minutes);
void gd_task_add_minutes(uint32_t id, int minutes);
gd_settings_t gd_settings_unlocked(void);
gd_source_t *gd_source_at(int index);
int gd_source_count(void);

/* Not locked: they take the lock themselves. */
bool gd_path(const char *name, char *out, size_t size);
void gd_store_flush(bool force);
void gd_cache_load(void);
void gd_cache_save(void);

#endif
