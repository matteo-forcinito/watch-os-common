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
#endif
