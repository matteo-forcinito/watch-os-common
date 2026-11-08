#ifndef LINK_ORBIT_H
#define LINK_ORBIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define LINK_ORBIT_MAX 144
#define LINK_ORBIT_EVENT_TYPE_SIZE 12
#define LINK_ORBIT_EVENT_TEXT_SIZE 16
#define LINK_ORBIT_MEMO_NAME_SIZE 64

typedef struct
{
    uint32_t seq;
    char type[LINK_ORBIT_EVENT_TYPE_SIZE];
    char text[LINK_ORBIT_EVENT_TEXT_SIZE];
    char from[13];
    int32_t value;
} link_orbit_event_t;

typedef struct
{
    bool countdown_running, countdown_ringing, watch_running;
    uint32_t countdown_total_s, countdown_remaining_s;
    uint64_t watch_elapsed_ms;
} link_orbit_timer_state_t;

typedef struct
{
    int phase, selected, count;
    uint32_t elapsed_ms, duration_ms;
    uint8_t levels[24];
    bool found;
    char name[LINK_ORBIT_MEMO_NAME_SIZE];
} link_orbit_memo_state_t;

typedef struct
{
    cJSON *(*info)(void); /* compact capability snapshot; caller owns it */
    cJSON *(*sensor)(const char *part); /* cached sample; never starts hardware */
    bool (*intercom)(const char *from_id, const cJSON *json); /* authenticated control, queued */
    cJSON *(*route)(const cJSON *json);
    bool (*input)(const char *from_id, const char *type, const char *text, int value);
    bool (*scene)(cJSON *json); /* takes ownership */
    bool (*house_scene)(cJSON *json, bool direct); /* takes ownership; queued */
    bool (*house_timer)(cJSON *json, bool direct); /* takes ownership; queued */
    bool (*timer)(const char *action, int seconds, link_orbit_timer_state_t *state);
    void (*events)(uint32_t since, uint32_t *latest, uint32_t *first,
                   link_orbit_event_t *next);
    bool (*memo_supported)(void);
    bool (*memo)(const char *action, int index, link_orbit_memo_state_t *state);
} link_orbit_ops_t;

void link_orbit_set_ops(const link_orbit_ops_t *ops);
size_t link_orbit_dispatch(const char *from_id, const uint8_t *request, size_t length,
                           uint8_t *reply, size_t max);

#endif
