#ifndef APPS_PET_H
#define APPS_PET_H
#include "cJSON.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

enum
{
    PET_ACTIVE,
    PET_OUTGOING,
    PET_INCOMING,
    PET_DEPARTED
};
typedef struct
{
    char seed[17], origin[64], transfer[41];
    int32_t stage, hunger, energy, joy, xp, asleep;
    int64_t born, updated;
} pet_record_t;

bool pet_init(void);
bool pet_current(pet_record_t *out);
bool pet_select(const char *seed);
bool pet_company(void);
cJSON *pet_snapshot(void);
/* Caller owns the result; NULL on validation, storage or transaction conflict. */
cJSON *pet_command(const cJSON *request, const char **error);
void pet_cta(lv_obj_t *parent);
void pet_thumbnail(lv_obj_t *parent, int width);
void pet_companion_start(void);
/* Active pets (copies); the scene diffs them to animate arrivals and departures. */
int pet_list(pet_record_t *out, int max);
/* True once for a pet that arrived from a journey since the page last looked. */
bool pet_take_arrival(const char *seed);
int pet_dex(uint16_t *keys, int max);
int pet_list_all(pet_record_t *out, int max);   /* every record, any stage */
/* Journeys to paired devices over ESP-NOW (pet_travel.c). */
void pet_travel_init(void);
bool pet_travel_send(const char *ecos_id, const char *seed);
void pet_travel_resume(void);
bool pet_travel_busy(void);
/* Native seed remains available even when that pet is visiting elsewhere. */
bool pet_native(char out[17]);
bool pet_at_home(const char *seed);
bool pet_travel_home(const char *seed);
/* Outing: radio on, search nearby paired devices and their pets (async). */
#define PET_FRIENDS_MAX 6
typedef struct
{
    char id[13];
    char device[24];
    char pet[16];       /* "" if no pet answered */
    uint8_t species;
} pet_friend_t;
typedef void (*pet_outing_cb_t)(const pet_friend_t *friends, int count);   /* LVGL locked */
bool pet_outing_search(pet_outing_cb_t done);
void pet_outing_cancel(void);
/* Seed and genome traits of a record (curiosity, energy, sociability, appetite, playfulness). */
void pet_traits(const pet_record_t *p, uint8_t out[5]);
#endif
