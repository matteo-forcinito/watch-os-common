#ifndef NEWS_INTERNAL_H
#define NEWS_INTERNAL_H

/* Shared by News' own files (store.c, fetch.c). */

#include "news.h"

extern nw_item_t *nw_item_table;
extern int nw_item_count;
extern nw_status_t nw_state;
extern nw_hit_t *nw_hits;
extern int nw_hit_count;
extern char nw_query[80];
extern char nw_ctx_title[80];
extern char *nw_ctx_text;

esp_err_t nw_store_init(void);
nw_feed_t *nw_feed_table(void);
int nw_feed_total(void);
void nw_changed(void);                       /* locked */
bool nw_add_item(const nw_item_t *item);     /* locked */
void nw_cluster(void);                       /* locked */
void nw_save_items(void);                    /* locked */
void nw_archive(const nw_item_t *items, int count);
void nw_archive_trim(bool small_storage);
void nw_write_day_terms(const nw_item_t *items, int count);
void nw_compute_trends(const nw_item_t *items, int count);
void nw_run_search(const char *query, int months, volatile bool *cancel);

#endif
