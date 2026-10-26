#pragma once
#include "lvgl.h"
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif
lv_obj_t *agentdeck_panel_create(lv_obj_t *parent,int width,int height,bool round);
void agentdeck_panel_update(lv_obj_t *object,const cJSON *cards,bool all,bool recent,int64_t received);
bool agentdeck_panel_back(lv_obj_t *object);
void agentdeck_panel_seen(lv_obj_t *object,int64_t received);
#ifdef __cplusplus
}
#endif
