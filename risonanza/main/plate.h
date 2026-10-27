#ifndef RISONANZA_PLATE_H
#define RISONANZA_PLATE_H
#include "lvgl.h"
typedef struct r_plate r_plate_t;
r_plate_t *r_plate_create(lv_obj_t *parent,int x,int y,int size);
void r_plate_frame(r_plate_t *p,int mode,float energy,bool frozen);
#endif
