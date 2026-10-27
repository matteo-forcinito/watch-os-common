#ifndef RISONANZA_DSP_H
#define RISONANZA_DSP_H
#include <stdbool.h>
#define R_N 256
#define R_BINS 64
#define R_VERSION 1
/* All amplitudes retain the input unit: g for IMU, normalized PCM for mic. */
typedef struct {
 unsigned version,source,sequence; int axis; float rate,rms,peak,hz,confidence,noise,tilt_x,tilt_y;
 float samples[R_N],spectrum[R_BINS];
 bool timing_ok,valid,clipped; int mode_m,mode_n;
} r_frame_t;
void r_analyze(const float *input,float rate,float max_hz,float floor,r_frame_t *out);
bool r_timing(const long long *timestamps,float *rate);
float r_ratio(const r_frame_t *a,const r_frame_t *b,bool *valid);
void r_mode(int index,int *m,int *n);
float r_field(int m,int n,float x,float y);
#endif
