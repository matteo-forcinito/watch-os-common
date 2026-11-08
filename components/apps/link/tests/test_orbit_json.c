#include "../link_orbit_json.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void rejected(const uint8_t *text, size_t length)
{
    cJSON *json = link_orbit_parse_json(text, length);
    assert(json == NULL);
}

int main(void)
{
    const uint8_t raw[] = {'{','"','x','"',':','1','}'};
    cJSON *json = link_orbit_parse_json(raw, sizeof(raw));
    assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(json, "x")) == 1);
    cJSON_Delete(json);
    uint8_t maximum[LINK_ORBIT_JSON_MAX + 1];
    memset(maximum, ' ', sizeof(maximum));
    memcpy(maximum, raw, sizeof(raw));
    json = link_orbit_parse_json(maximum, LINK_ORBIT_JSON_MAX);
    assert(json != NULL);
    cJSON_Delete(json);
    rejected(maximum, LINK_ORBIT_JSON_MAX + 1);
    maximum[100] = 0;
    rejected(maximum, LINK_ORBIT_JSON_MAX);
    rejected((const uint8_t *)"{} trailing", 11);
    rejected((const uint8_t *)"[1,2]", 5);
    rejected((const uint8_t *)"{\"x\":1", 6);
    rejected((const uint8_t *)"{\"x\":\"a\nb\"}", 11);
    const char hidden[] = "{\"op\":\"scene\\u0000hidden\"}";
    rejected((const uint8_t *)hidden, strlen(hidden));
    const char quoted[] = "{\"text\":\"\\\" [[{{ brackets inside a string\"}";
    json = link_orbit_parse_json((const uint8_t *)quoted, strlen(quoted));
    assert(json != NULL);
    cJSON_Delete(json);
    uint8_t nested[80];
    size_t at = 0;
    memcpy(nested, "{\"a\":", 5); at += 5;
    for (unsigned i = 0; i < LINK_ORBIT_JSON_DEPTH; ++i) nested[at++] = '[';
    nested[at++] = '0';
    for (unsigned i = 0; i < LINK_ORBIT_JSON_DEPTH; ++i) nested[at++] = ']';
    nested[at++] = '}';
    rejected(nested, at);
    puts("Orbit bounded JSON tests passed");
    return 0;
}
