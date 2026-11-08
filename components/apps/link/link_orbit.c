#include "link_orbit.h"
#include "link_orbit_json.h"

#include <string.h>

static const link_orbit_ops_t *s_ops;

void link_orbit_set_ops(const link_orbit_ops_t *ops)
{
    s_ops = ops;
}

static cJSON *event_json(const link_orbit_event_t *event)
{
    cJSON *out = cJSON_CreateObject();
    if (out == NULL) return NULL;
    cJSON_AddNumberToObject(out, "seq", event->seq);
    cJSON_AddStringToObject(out, "type", event->type);
    if (event->from[0] != '\0') cJSON_AddStringToObject(out, "from", event->from);
    if (event->text[0] != '\0') cJSON_AddStringToObject(out, "value", event->text);
    else cJSON_AddNumberToObject(out, "value", event->value);
    return out;
}

static cJSON *timer_json(const link_orbit_timer_state_t *state)
{
    cJSON *out = cJSON_CreateObject();
    if (out == NULL) return NULL;
    cJSON_AddBoolToObject(out, "running", state->countdown_running);
    cJSON_AddBoolToObject(out, "ringing", state->countdown_ringing);
    cJSON_AddNumberToObject(out, "total", state->countdown_total_s);
    cJSON_AddNumberToObject(out, "remaining", state->countdown_remaining_s);
    cJSON_AddBoolToObject(out, "watch", state->watch_running);
    cJSON_AddNumberToObject(out, "elapsed", (double)state->watch_elapsed_ms);
    return out;
}

size_t link_orbit_dispatch(const char *from_id, const uint8_t *request, size_t length,
                           uint8_t *reply, size_t max)
{
    if (s_ops == NULL || request == NULL || reply == NULL || max == 0 || length == 0 || length > LINK_ORBIT_JSON_MAX)
        return 0;
    if (max > LINK_ORBIT_MAX) max = LINK_ORBIT_MAX;

    cJSON *in = link_orbit_parse_json(request, length);
    if (!cJSON_IsObject(in))
    {
        cJSON_Delete(in);
        return 0;
    }
    const char *op = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(in, "op"));
    cJSON *out = NULL;
    bool ok = false;

    if (op != NULL && strcmp(op, "info") == 0)
    {
        if (s_ops->info != NULL) out = s_ops->info();
    }
    else if (op != NULL && strcmp(op, "sensor") == 0)
    {
        const cJSON *part_item = cJSON_GetObjectItemCaseSensitive(in, "part");
        const char *part = part_item != NULL ? cJSON_GetStringValue(part_item) : "accel";
        if (part != NULL && s_ops->sensor != NULL) out = s_ops->sensor(part);
        if (out == NULL)
        {
            out = cJSON_CreateObject();
            if (out != NULL) cJSON_AddStringToObject(out, "error", "sensor_unavailable");
        }
    }
    else if (op != NULL && strcmp(op, "intercom") == 0)
    {
        ok = from_id != NULL && s_ops->intercom != NULL && s_ops->intercom(from_id, in);
        out = cJSON_CreateObject();
        if (out != NULL)
        {
            cJSON_AddBoolToObject(out, "ok", ok);
            cJSON_AddBoolToObject(out, "queued", ok);
        }
    }
    else if (op != NULL && strcmp(op, "route") == 0)
    {
        cJSON_DeleteItemFromObjectCaseSensitive(in, "op");
        if (s_ops->route != NULL) out = s_ops->route(in);
        if (out == NULL)
        {
            out = cJSON_CreateObject();
            if (out != NULL) cJSON_AddBoolToObject(out, "ok", false);
        }
    }
    else if (op != NULL && strcmp(op, "input") == 0)
    {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(in, "type"));
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(in, "value");
        const char *text = cJSON_GetStringValue(value);
        bool known = type != NULL && (strcmp(type, "tap") == 0 || strcmp(type, "dial") == 0 ||
                                     strcmp(type, "dial_done") == 0 || strcmp(type, "orbi") == 0);
        bool number = cJSON_IsNumber(value) && value->valuedouble >= INT32_MIN && value->valuedouble <= INT32_MAX &&
                      value->valuedouble == (double)value->valueint;
        bool string = text != NULL && text[0] != '\0' && strlen(text) < LINK_ORBIT_EVENT_TEXT_SIZE;
        ok = known && from_id != NULL && (number || string) && s_ops->input != NULL &&
             s_ops->input(from_id, type, string ? text : NULL, number ? value->valueint : 0);
        out = cJSON_CreateObject();
        if (out != NULL) cJSON_AddBoolToObject(out, "ok", ok);
    }
    else if (op != NULL && strcmp(op, "scene") == 0)
    {
        cJSON_DeleteItemFromObjectCaseSensitive(in, "op");
        if (s_ops->scene != NULL)
        {
            ok = s_ops->scene(in); /* callback takes ownership */
        }
        else cJSON_Delete(in);
        in = NULL;
        out = cJSON_CreateObject();
        if (out != NULL) cJSON_AddBoolToObject(out, "ok", ok);
    }
    else if (op != NULL && (strcmp(op, "house_scene") == 0 || strcmp(op, "house_timer") == 0))
    {
        bool is_scene = strcmp(op, "house_scene") == 0;
        const cJSON *direct_item = cJSON_GetObjectItemCaseSensitive(in, "direct");
        bool valid_direct = cJSON_IsTrue(direct_item);
        cJSON_DeleteItemFromObjectCaseSensitive(in, "op");
        cJSON_DeleteItemFromObjectCaseSensitive(in, "direct");
        if (valid_direct)
        {
            if (is_scene && s_ops->house_scene != NULL)
                ok = s_ops->house_scene(in, true);
            else if (!is_scene && s_ops->house_timer != NULL)
                ok = s_ops->house_timer(in, true);
            else cJSON_Delete(in);
            in = NULL;
        }
        out = cJSON_CreateObject();
        if (out != NULL)
        {
            cJSON_AddBoolToObject(out, "ok", ok);
            cJSON_AddBoolToObject(out, "queued", ok);
        }
    }
    else if (op != NULL && strcmp(op, "events") == 0)
    {
        const cJSON *since_item = cJSON_GetObjectItemCaseSensitive(in, "since");
        bool valid = since_item == NULL ||
                     (cJSON_IsNumber(since_item) && since_item->valuedouble >= 0 &&
                      since_item->valuedouble <= UINT32_MAX &&
                      since_item->valuedouble == (double)(uint32_t)since_item->valuedouble);
        if (valid && s_ops->events != NULL)
        {
            uint32_t since = since_item != NULL ? (uint32_t)since_item->valuedouble : 0;
            uint32_t latest = 0, first = 1;
            link_orbit_event_t event = {0};
            s_ops->events(since, &latest, &first, &event);
            if (first == 0) first = 1;
            out = cJSON_CreateObject();
            if (out != NULL)
            {
                cJSON_AddNumberToObject(out, "seq", latest);
                cJSON_AddBoolToObject(out, "gap", (since < first - 1) || since > latest);
                if (event.seq != 0)
                {
                    cJSON_AddItemToObject(out, "event", event_json(&event));
                    cJSON_AddBoolToObject(out, "more", event.seq < latest);
                }
                else
                {
                    cJSON_AddNullToObject(out, "event");
                    cJSON_AddBoolToObject(out, "more", false);
                }
            }
        }
    }
    else if (op != NULL && strcmp(op, "timer") == 0)
    {
        const cJSON *action_item = cJSON_GetObjectItemCaseSensitive(in, "action");
        const char *action = cJSON_GetStringValue(action_item);
        const cJSON *seconds_item = cJSON_GetObjectItemCaseSensitive(in, "seconds");
        bool seconds_valid = seconds_item == NULL ||
                             (cJSON_IsNumber(seconds_item) && seconds_item->valuedouble >= 0 &&
                              seconds_item->valuedouble <= 24 * 3600 &&
                              seconds_item->valuedouble == (double)seconds_item->valueint);
        bool query = action_item == NULL && seconds_item == NULL;
        bool valid = query || (action != NULL && seconds_valid);
        link_orbit_timer_state_t state = {0};
        ok = valid && s_ops->timer != NULL &&
             s_ops->timer(action, cJSON_IsNumber(seconds_item) ? seconds_item->valueint : 0, &state);
        if (ok) out = timer_json(&state);
        else
        {
            out = cJSON_CreateObject();
            if (out != NULL) cJSON_AddBoolToObject(out, "ok", false);
        }
    }
    else if (op != NULL && strcmp(op, "memo") == 0)
    {
        const cJSON *action_item = cJSON_GetObjectItemCaseSensitive(in, "action");
        const char *action = cJSON_GetStringValue(action_item);
        const cJSON *index_item = cJSON_GetObjectItemCaseSensitive(in, "index");
        bool index_valid = index_item == NULL ||
                           (cJSON_IsNumber(index_item) && index_item->valuedouble >= 0 &&
                            index_item->valuedouble == (double)index_item->valueint);
        int index = cJSON_IsNumber(index_item) ? index_item->valueint : -1;
        if (s_ops->memo_supported == NULL || !s_ops->memo_supported())
        {
            out = cJSON_CreateObject();
            if (out != NULL) cJSON_AddStringToObject(out, "error", "unsupported");
        }
        else if (index_valid && (action_item == NULL || action != NULL) && s_ops->memo != NULL)
        {
            link_orbit_memo_state_t state = {0};
            ok = s_ops->memo(action, index, &state);
            out = cJSON_CreateObject();
            if (out != NULL && !ok) cJSON_AddBoolToObject(out, "ok", false);
            else if (out != NULL)
            {
                if (action != NULL) cJSON_AddBoolToObject(out, "ok", ok);
                else if (index >= 0)
                {
                    cJSON_AddNumberToObject(out, "count", state.count);
                    cJSON_AddBoolToObject(out, "found", state.found);
                    if (state.found)
                    {
                        cJSON_AddNumberToObject(out, "index", index);
                        cJSON_AddStringToObject(out, "name", state.name);
                    }
                }
                else
                {
                    cJSON_AddNumberToObject(out, "phase", state.phase);
                    cJSON_AddNumberToObject(out, "elapsed", state.elapsed_ms);
                    cJSON_AddNumberToObject(out, "duration", state.duration_ms);
                    cJSON_AddNumberToObject(out, "selected", state.selected);
                    cJSON_AddNumberToObject(out, "count", state.count);
                    /* Lossless peak history within the 144-byte RPC budget. */
                    static const char hex[] = "0123456789abcdef";
                    char levels[49];
                    for (size_t i = 0; i < sizeof(state.levels); ++i)
                    {
                        levels[2 * i] = hex[state.levels[i] >> 4];
                        levels[2 * i + 1] = hex[state.levels[i] & 15];
                    }
                    levels[48] = '\0';
                    cJSON_AddStringToObject(out, "lv", levels);
                }
            }
        }
        else
        {
            out = cJSON_CreateObject();
            if (out != NULL) cJSON_AddBoolToObject(out, "ok", false);
        }
    }

    cJSON_Delete(in);
    if (out == NULL) return 0;
    char *encoded = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (encoded == NULL) return 0;
    size_t n = strlen(encoded);
    if (n > max)
    {
        cJSON_free(encoded);
        static const char too_large[] = "{\"error\":\"reply_too_large\"}";
        if (sizeof(too_large) - 1 > max) return 0;
        memcpy(reply, too_large, sizeof(too_large) - 1);
        return sizeof(too_large) - 1;
    }
    memcpy(reply, encoded, n);
    cJSON_free(encoded);
    return n;
}
