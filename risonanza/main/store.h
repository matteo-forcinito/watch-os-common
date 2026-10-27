#ifndef RISONANZA_STORE_H
#define RISONANZA_STORE_H
#include "dsp.h"
bool r_store_load(int slot,r_frame_t *frame);
int r_store_save(int slot,const r_frame_t *frame); /* -1 failure, 0 NVS only, 1 NVS + CSV */
#endif
