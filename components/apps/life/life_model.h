#ifndef LIFE_MODEL_H
#define LIFE_MODEL_H
#include "cJSON.h"
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum
{
    LIFE_RACCOLTA,
    LIFE_ZAMPE,
    LIFE_COUNT
} life_kind_t;
#define LIFE_NO_DAY INT_MIN
int life_day(const char *iso);
void life_date(int day, char out[11]);
int life_weekday(int day); /* Monday=0 */
cJSON *life_defaults(life_kind_t kind);
bool life_apply(life_kind_t kind, cJSON *document, const cJSON *command, int today, char *error, size_t size);
cJSON *life_collections(const cJSON *city, int day);
cJSON *life_view(life_kind_t kind, const cJSON *document, int today);
const char *life_string(const cJSON *object, const char *key);
int life_number(const cJSON *object, const char *key, int fallback);
const cJSON *life_find(const cJSON *array, int id);
#endif
