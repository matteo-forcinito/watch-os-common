#include "link_sensor.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    float accel[] = {1, -0.1234f, 0}, gyro[] = {2000, -2000, 1};
    cJSON *out = link_sensor_json("accel", true, true, true, 10, accel, gyro, 24.5f);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(out, "fresh")));
    assert(cJSON_GetArrayItem(cJSON_GetObjectItem(out, "v"), 1)->valueint == -123);
    char *text = cJSON_PrintUnformatted(out);
    assert(strlen(text) <= 144); cJSON_free(text); cJSON_Delete(out);
    out = link_sensor_json("gyro", true, true, true, 1000, accel, gyro, 0);
    assert(cJSON_GetArrayItem(cJSON_GetObjectItem(out, "v"), 0)->valueint == 2000000);
    cJSON_Delete(out);
    out = link_sensor_json("temperature", true, true, true, 1, accel, gyro, 24.5f);
    assert(cJSON_GetArrayItem(cJSON_GetObjectItem(out, "v"), 0)->valueint == 24500);
    cJSON_Delete(out);
    out = link_sensor_json("accel", true, true, true, 1001, accel, gyro, 0);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(out, "fresh")) && !cJSON_GetObjectItem(out, "v"));
    cJSON_Delete(out);
    accel[0] = NAN;
    out = link_sensor_json("accel", true, true, true, 0, accel, gyro, 0);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(out, "fresh"))); cJSON_Delete(out);
    out = link_sensor_json("accel", false, false, false, INT64_MAX, NULL, NULL, 0);
    assert(cJSON_GetObjectItem(out, "age")->valuedouble == INT32_MAX); cJSON_Delete(out);
    assert(!link_sensor_json("unknown", true, true, true, 0, accel, gyro, 0));
    puts("Orbit compact sensor tests passed");
    return 0;
}
