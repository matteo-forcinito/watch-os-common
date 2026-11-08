#pragma once

/* Parse bounded, non-NUL-terminated commands directly from their PSRAM input.
 * Bound recursion before cJSON sees the input: its generic depth limit is much
 * larger than the stack budget of the family worker. */
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINK_ORBIT_JSON_MAX 1024
#define LINK_ORBIT_JSON_DEPTH 12

static inline cJSON *link_orbit_parse_json_limit(const uint8_t *input, size_t length, size_t maximum)
{
    if (input == NULL || length == 0 || maximum > 2048 || length > maximum) return NULL;
    unsigned depth = 0;
    bool string = false, escaped = false;
    for (size_t i = 0; i < length; ++i)
    {
        uint8_t c = input[i];
        if (c == 0 || (c < 32 && (string || (c != '\t' && c != '\r' && c != '\n')))) return NULL;
        if (string)
        {
            if (escaped) escaped = false;
            else if (c == '\\')
            {
                /* cJSON exposes strings as C strings; reject a decoded NUL
                 * rather than interpreting only the prefix of an action. */
                if (i + 5 < length && input[i + 1] == 'u' && input[i + 2] == '0' &&
                    input[i + 3] == '0' && input[i + 4] == '0' && input[i + 5] == '0') return NULL;
                escaped = true;
            }
            else if (c == '"') string = false;
        }
        else if (c == '"') string = true;
        else if (c == '{' || c == '[')
        {
            if (++depth > LINK_ORBIT_JSON_DEPTH) return NULL;
        }
        else if (c == '}' || c == ']')
        {
            if (depth == 0) return NULL;
            --depth;
        }
    }
    if (string || depth != 0) return NULL;
    const char *end = NULL;
    const char *limit = (const char *)input + length;
    cJSON *json = cJSON_ParseWithLengthOpts((const char *)input, length, &end, false);
    if (json != NULL && end != NULL)
    {
        while (end < limit && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    }
    if (!cJSON_IsObject(json) || end != limit)
    {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static inline cJSON *link_orbit_parse_json(const uint8_t *input, size_t length)
{
    return link_orbit_parse_json_limit(input, length, LINK_ORBIT_JSON_MAX);
}
