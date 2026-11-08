#include "link_sensor.h"
#include <math.h>
#include <string.h>

static bool quantize(float value, float scale, int *out)
{
    double scaled = (double)value * scale;
    if (!isfinite(scaled) || scaled < -2000000000.0 || scaled > 2000000000.0) return false;
    *out = (int)round(scaled);
    return true;
}

cJSON *link_sensor_json(const char *part, bool supported, bool running, bool valid,
                        int64_t age_ms, const float acceleration[3], const float angular[3], float temperature)
{
    if (part == NULL) return NULL;
    const float *axes = NULL;
    const char *unit = NULL;
    unsigned count = 3;
    if (strcmp(part, "accel") == 0) { axes = acceleration; unit = "mg"; }
    else if (strcmp(part, "gyro") == 0) { axes = angular; unit = "mdps"; }
    else if (strcmp(part, "temperature") == 0) { axes = &temperature; unit = "mC"; count = 1; }
    else return NULL;
    int values[3] = {0};
    bool fresh = supported && running && valid && age_ms >= 0 && age_ms <= 1000 && axes != NULL;
    for (unsigned i = 0; fresh && i < count; ++i) fresh = quantize(axes[i], 1000, &values[i]);
    cJSON *out = cJSON_CreateObject();
    if (out != NULL)
    {
        cJSON_AddBoolToObject(out, "supported", supported);
        cJSON_AddBoolToObject(out, "fresh", fresh);
        cJSON_AddNumberToObject(out, "age", age_ms < 0 ? -1 : age_ms > INT32_MAX ? INT32_MAX : (double)age_ms);
        cJSON_AddStringToObject(out, "u", unit);
        if (fresh)
        {
            cJSON *array = cJSON_AddArrayToObject(out, "v");
            for (unsigned i = 0; i < count; ++i) cJSON_AddItemToArray(array, cJSON_CreateNumber(values[i]));
        }
    }
    return out;
}
