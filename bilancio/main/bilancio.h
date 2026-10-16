#ifndef BILANCIO_H
#define BILANCIO_H

/*
 * Bilancio: how much can I spend today, what is coming this month, what am I
 * getting wrong. Hosted app (internal with CONFIG_APPKIT_HOSTED, external on
 * the Market with the same code), see docs/progetti/BILANCIO.md.
 *
 *   calc.c    pure logic, tested in an emulator (dates, rules, budget, analysis)
 *   store.c   the book in PSRAM, saved to <storage>/config/bilancio.json
 *   web.c     the dashboard API, bilancio_dashboard.html the page
 *
 * Everything stays on the watch; the dashboard needs the paired token.
 */

#include "calc.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BL_MAX_RULES 64
#define BL_MAX_TX 3000            /* about 24 months of a busy household */
#define BL_MAX_CATEGORIES 16
#define BL_CATEGORY_LEN 20
#define BL_MAX_ARCHIVE 120        /* months older than 24, summed up */
#define BL_KEEP_MONTHS 24

typedef struct
{
    int32_t month;                /* month index */
    bl_cents_t in;
    bl_cents_t out;               /* positive */
} bl_archive_t;

/* Load the book (once; later calls do nothing). */
esp_err_t bl_start(void);

/* Today on the local calendar, BL_NONE while the clock is not set. */
bl_day_t bl_today(void);
/* Changes on every edit (the UI and the page redraw when it moves). */
uint32_t bl_revision(void);

/* Read access: hold the lock while using the pointers. */
void bl_lock(void);
void bl_unlock(void);
const bl_book_t *bl_book(void);
const char (*bl_categories(int *count))[BL_CATEGORY_LEN];
const bl_archive_t *bl_archive(int *count);
const bl_rule_t *bl_rule_by_id(uint32_t id);

/* Edits (take the lock themselves); saved a moment later. */
bool bl_set_balance(bl_cents_t amount, bl_day_t date);
/* id 0: a new movement. Returns its id, 0 if full or not found. */
uint32_t bl_tx_put(const bl_tx_t *tx);
bool bl_tx_delete(uint32_t id);
uint32_t bl_rule_put(const bl_rule_t *rule);
bool bl_rule_delete(uint32_t id);
/* Index of a category, added if new; -1 if the list is full. */
int bl_category_add(const char *name);
bool bl_category_rename(int index, const char *name);

/* The whole book as the JSON file (PSRAM, free() it), NULL without memory. */
char *bl_export(void);
/* Replace the book with a JSON file. false + why if it is not a Bilancio file. */
bool bl_import(const char *text, char *why, size_t size);
/* Save now if something changed (closing the app). */
void bl_flush(void);

/* Months between dates a rule may use: 1 2 3 4 6 12, then 24 36 48 60 120 (inspection, licence). */
bool bl_every_valid(int every);

/* Copy cut at a UTF-8 boundary, bad bytes dropped. */
void bl_copy(char *out, size_t size, const char *in);

esp_err_t bilancio_web_start(void);

/* The watch screens (ui.c). */
#include "lvgl.h"
void bilancio_ui_create(lv_obj_t *screen);
bool bilancio_ui_back(void);
void bilancio_ui_destroy(void);

#endif
