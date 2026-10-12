#ifndef SERVICES_MARKET_H
#define SERVICES_MARKET_H

/* GitHub-backed external app library. Network and SD writes run in a worker;
 * getters copy snapshots and never expose worker-owned memory to LVGL. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "services/extapp.h"

#define MARKET_MAX_APPS 32
#define MARKET_URL_MAX 320
#define MARKET_CATALOG_MAX (48 * 1024)
#define MARKET_ICON_MAX (64 * 1024)
#define MARKET_CATALOG_URL "https://raw.githubusercontent.com/matteo-forcinito/watch-firmware-releases/main/market/catalog.json"

typedef struct
{
    char id[32];
    char name[32];
    char summary[160];
    char version[32];
    char board[32];
    char project[32];
    char channel[8];
    char binary_url[MARKET_URL_MAX];
    char icon_url[MARKET_URL_MAX];
    char binary_sha256[65];
    char icon_sha256[65];
    size_t binary_size;
    size_t icon_size;
    uint32_t color;
    bool installed;
    char installed_version[32];
    extapp_t local;
} market_entry_t;

typedef enum
{
    MARKET_IDLE, MARKET_FETCHING, MARKET_READY, MARKET_DOWNLOADING,
    MARKET_VERIFYING, MARKET_PUBLISHING, MARKET_DONE, MARKET_FAILED, MARKET_CANCELLED
} market_phase_t;

typedef struct
{
    market_phase_t phase;
    int percent;
    uint32_t revision;
    bool busy;
    char app_id[32];
    char message[96];
} market_status_t;

esp_err_t market_init(void);
void market_status(market_status_t *out);
bool market_busy(void);
int market_count(void);
bool market_entry(int index, market_entry_t *out);
esp_err_t market_refresh(void);
/* Downloads verified app files to SD only; it never boots an application. */
esp_err_t market_install(const char *id);
esp_err_t market_remove(const char *id);
void market_cancel(void);
bool market_development(void);
void market_set_development(bool enabled);
int market_view(void); /* 0 grid, 1 list, 2 full-screen cards */
void market_set_view(int view);

/* Bounded catalog parser and origin checks, also used by development QA. */
bool market_trusted_url(const char *url);
bool market_safe_id(const char *id);
esp_err_t market_parse_catalog(const char *text, const char *board,
                               market_entry_t *entries, int capacity, int *count);
bool market_catalog_selftest(void);

#endif
