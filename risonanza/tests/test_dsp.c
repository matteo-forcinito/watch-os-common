#include "dsp.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static float data[R_N];
static void tone(float rate,float hz,float amp) {for(int i=0;i<R_N;i++)data[i]=1+amp*sinf(6.283185307f*hz*i/rate);}
int main(void) {
 r_frame_t a={0},b={0};a.timing_ok=true;
 tone(128,8,.2f);r_analyze(data,128,40,.0001f,&a);
 assert(a.valid&&fabsf(a.hz-8)<.1f&&fabsf(a.rms-.141421f)<.001f);
 b=a;b.rms*=.5f;bool valid;assert(fabsf(r_ratio(&a,&b,&valid)-2)<.01f&&valid);
 b.source=1;r_ratio(&a,&b,&valid);assert(!valid);
 for(int i=0;i<R_N;i++)data[i]=1;r_analyze(data,128,40,.0001f,&a);assert(!a.valid&&a.rms<.0001f);
 tone(128,60,.2f);r_analyze(data,128,40,.0001f,&a);assert(!a.valid);
 a.source=1;tone(8000,437,.15f);r_analyze(data,8000,2400,.0002f,&a);assert(a.valid&&fabsf(a.hz-437)<12);
 a.clipped=true;r_analyze(data,8000,2400,.0002f,&a);assert(!a.valid);
 long long stamps[R_N];for(int i=0;i<R_N;i++)stamps[i]=i*8000LL;
 float rate=0;assert(r_timing(stamps,&rate)&&fabsf(rate-125)<.001f);
 stamps[120]=stamps[119];assert(!r_timing(stamps,&rate));
 for(int i=0;i<16;i++){int m,n;r_mode(i,&m,&n);assert(m!=n);assert(fabsf(r_field(m,n,.37f,.37f))<1e-6f);assert(fabsf(r_field(m,n,.23f,.71f)+r_field(m,n,.71f,.23f))<1e-5f);}
 puts("RISONANZA DSP PASS: DC, tones, out-of-band, clipping, timing, comparison, mode symmetries");
}
