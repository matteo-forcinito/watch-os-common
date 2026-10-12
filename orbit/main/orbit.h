#ifndef ORBIT_H
#define ORBIT_H
#include "lvgl.h"
void orbit_create(lv_obj_t *screen);
bool orbit_back(void);
void orbit_alarm_prepare(void);
void orbit_alarm_start(void);
#endif
