#ifndef RISONANZA_MEASURE_H
#define RISONANZA_MEASURE_H
#include "dsp.h"
#include "esp_err.h"
typedef struct {r_frame_t frame;unsigned errors;int source;esp_err_t error;bool calibrating,running;} r_measure_t;
void r_measure_start(void);
void r_measure_source(int source);
void r_measure_calibrate(void);
void r_measure_get(r_measure_t *out);
void r_measure_stop(void);
#endif
