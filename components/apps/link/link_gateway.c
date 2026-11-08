#include "link_gateway.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static cJSON *command(const char *op, const cJSON *body)
{
    cJSON *out = body != NULL ? cJSON_Duplicate(body, true) : cJSON_CreateObject();
    if (!cJSON_IsObject(out)) { cJSON_Delete(out); return NULL; }
    cJSON_DeleteItemFromObjectCaseSensitive(out, "op");
    if (cJSON_AddStringToObject(out, "op", op) == NULL) { cJSON_Delete(out); return NULL; }
    return out;
}

cJSON *link_gateway_command(const char *path, const cJSON *body)
{
    if (path == NULL || (body != NULL && !cJSON_IsObject(body))) return NULL;
    if (strcmp(path, "/api/link/scene") == 0 && body != NULL) return command("scene", body);
    if (strcmp(path, "/api/timer") == 0) return command("timer", body);
    if (strcmp(path, "/api/link") == 0 && body == NULL) return command("info", NULL);
    if (strcmp(path, "/api/voice") == 0) return command("memo", body);
    if (strcmp(path, "/api/link/route") == 0 && body != NULL) return command("route", body);
    if (body == NULL)
    {
        const char *part = strcmp(path, "/api/link/sensors") == 0 || strcmp(path, "/api/link/sensors?part=accel") == 0 ? "accel" :
                           strcmp(path, "/api/link/sensors?part=gyro") == 0 ? "gyro" :
                           strcmp(path, "/api/link/sensors?part=temperature") == 0 ? "temperature" : NULL;
        if (part != NULL)
        {
            cJSON *out = command("sensor", NULL);
            if (out != NULL) cJSON_AddStringToObject(out, "part", part);
            return out;
        }
    }
    if (strcmp(path, "/api/link/events") == 0 && body == NULL)
    {
        cJSON *out = command("events", NULL);
        if (out != NULL) cJSON_AddNumberToObject(out, "since", UINT32_MAX);
        return out;
    }
    static const char events[] = "/api/link/events?since=";
    if (body == NULL && strncmp(path, events, sizeof(events) - 1) == 0)
    {
        const char *cursor = path + sizeof(events) - 1;
        if (*cursor == '\0') return NULL;
        uint64_t value = 0;
        for (; *cursor != '\0'; ++cursor)
        {
            if (*cursor < '0' || *cursor > '9') return NULL;
            value = value * 10 + (unsigned)(*cursor - '0');
            if (value > UINT32_MAX) return NULL;
        }
        cJSON *out = command("events", NULL);
        if (out != NULL) cJSON_AddNumberToObject(out, "since", (double)value);
        return out;
    }
    return NULL;
}

static void copy_field(cJSON *to, const char *name, const cJSON *from, const char *source)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(from, source);
    if (item != NULL) cJSON_AddItemToObject(to, name, cJSON_Duplicate(item, true));
}

cJSON *link_gateway_response(const char *path, cJSON *reply)
{
    if (path == NULL || !cJSON_IsObject(reply)) { cJSON_Delete(reply); return NULL; }
    if (strcmp(path, "/api/link") == 0 && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(reply, "w")))
    {
        cJSON *out = cJSON_CreateObject();
        if (out != NULL)
        {
            cJSON_AddNumberToObject(out, "version", 1);
            cJSON *features = cJSON_AddArrayToObject(out, "features");
            const char *always[] = {"scene", "events", "dial", "timer"};
            for (size_t i = 0; i < sizeof(always) / sizeof(*always); ++i)
                cJSON_AddItemToArray(features, cJSON_CreateString(always[i]));
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "voice")))
                cJSON_AddItemToArray(features, cJSON_CreateString("voice"));
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "knob")))
                cJSON_AddItemToArray(features, cJSON_CreateString("knob"));
            cJSON *screen = cJSON_AddObjectToObject(out, "screen");
            copy_field(screen, "w", reply, "w");
            copy_field(screen, "h", reply, "h");
            copy_field(screen, "round", reply, "round");
        }
        cJSON_Delete(reply);
        return out;
    }
    if (strcmp(path, "/api/timer") == 0 && cJSON_GetObjectItemCaseSensitive(reply, "running") != NULL)
    {
        cJSON *out = cJSON_CreateObject();
        if (out != NULL)
        {
            cJSON *countdown = cJSON_AddObjectToObject(out, "countdown");
            cJSON *watch = cJSON_AddObjectToObject(out, "stopwatch");
            copy_field(countdown, "running", reply, "running");
            copy_field(countdown, "ringing", reply, "ringing");
            copy_field(countdown, "total_s", reply, "total");
            copy_field(countdown, "remaining_s", reply, "remaining");
            copy_field(watch, "running", reply, "watch");
            copy_field(watch, "elapsed_ms", reply, "elapsed");
        }
        cJSON_Delete(reply);
        return out;
    }
    if (strcmp(path, "/api/link/events") == 0 || strncmp(path, "/api/link/events?since=", 23) == 0)
    {
        cJSON *events = cJSON_AddArrayToObject(reply, "events");
        cJSON *event = cJSON_DetachItemFromObjectCaseSensitive(reply, "event");
        if (event != NULL && cJSON_IsObject(event))
        {
            /* HTTP clients use seq as their next cursor. Do not skip the
             * remaining backlog when this bounded RPC returns one event. */
            const cJSON *seq = cJSON_GetObjectItemCaseSensitive(event, "seq");
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "more")) && cJSON_IsNumber(seq))
                cJSON_ReplaceItemInObjectCaseSensitive(reply, "seq", cJSON_Duplicate(seq, true));
            cJSON_AddItemToArray(events, event);
        }
        else cJSON_Delete(event);
    }
    return reply;
}
