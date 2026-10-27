#include "dsp.h"
#include <math.h>
#include <string.h>
#define PI 3.14159265358979323846f
void r_mode(int index,int *m,int *n) {
 static const int modes[][2]={{1,2},{1,3},{2,3},{1,4},{2,4},{3,4},{1,5},{2,5},{3,5},{4,5},{2,6},{3,6},{4,6},{5,6},{3,7},{4,7}};
 index=index<0?0:index>15?15:index;*m=modes[index][0];*n=modes[index][1];
}
float r_field(int m,int n,float x,float y) {
 return cosf(m*PI*x)*cosf(n*PI*y)-cosf(n*PI*x)*cosf(m*PI*y);
}
bool r_timing(const long long *t,float *rate) {
 if(t[R_N-1]<=t[0])return false;
 double mean=(double)(t[R_N-1]-t[0])/(R_N-1),variance=0;
 for(int i=1;i<R_N;i++) {
  double dt=(double)(t[i]-t[i-1]);if(dt<=0 || dt>mean*2.5)return false;
  variance+=(dt-mean)*(dt-mean);
 }
 *rate=(float)(1000000.0/mean);
 return sqrt(variance/(R_N-1))/mean<.22;
}
void r_analyze(const float *in,float rate,float max_hz,float floor,r_frame_t *o) {
 float real[R_N],imag[R_N]={0};
 o->version=R_VERSION;o->rate=rate;o->noise=floor;o->valid=false;o->hz=0;o->confidence=0;o->rms=0;o->peak=0;
 memset(o->spectrum,0,sizeof(o->spectrum));
 if(!isfinite(rate)||rate<=0)return;
 float mean=0;for(int i=0;i<R_N;i++){if(!isfinite(in[i]))return;mean+=in[i]/R_N;}
 float sum=0,peak=0;
 for(int i=0;i<R_N;i++) {
  float s=in[i]-mean;o->samples[i]=s;sum+=s*s;if(fabsf(s)>peak)peak=fabsf(s);
  real[i]=s*(.5f-.5f*cosf(2*PI*i/(R_N-1)));
 }
 o->rms=sqrtf(sum/R_N);o->peak=peak;
 for(int i=1,j=0;i<R_N;i++) {
  int bit=R_N>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;
  if(i<j){float z=real[i];real[i]=real[j];real[j]=z;}
 }
 for(int len=2;len<=R_N;len<<=1) {
  float wr=cosf(-2*PI/len),wi=sinf(-2*PI/len);
  for(int i=0;i<R_N;i+=len) {
   float ur=1,ui=0;
   for(int j=0;j<len/2;j++) {
    int a=i+j,b=a+len/2;float vr=real[b]*ur-imag[b]*ui,vi=real[b]*ui+imag[b]*ur;
    real[b]=real[a]-vr;imag[b]=imag[a]-vi;real[a]+=vr;imag[a]+=vi;
    float next=ur*wr-ui*wi;ui=ur*wi+ui*wr;ur=next;
   }
  }
 }
 float power[R_N/2];float total=0,all=0,best=0;int kbest=0;
 int last=(int)(max_hz*R_N/rate);if(last>R_N/2-2)last=R_N/2-2;
 float low=o->source==1?60.0f:2.0f;
 for(int k=1;k<R_N/2;k++) {
  power[k]=real[k]*real[k]+imag[k]*imag[k];all+=power[k];
  if(k<=last && k*rate/R_N>=low){total+=power[k];if(power[k]>best){best=power[k];kbest=k;}}
 }
 for(int b=0;b<R_BINS;b++) {
  int first=1+b*last/R_BINS,end=1+(b+1)*last/R_BINS;
  float p=0;for(int k=first;k<end && k<R_N/2;k++)if(power[k]>p)p=power[k];
  o->spectrum[b]=sqrtf(p)*4/R_N;
 }
 if(kbest>1 && total>1e-15f) {
  float left=power[kbest-1],mid=power[kbest],right=power[kbest+1];
  float denom=left-2*mid+right,offset=fabsf(denom)>1e-15f?.5f*(left-right)/denom:0;
  if(offset<-.5f)offset=-.5f;
  if(offset>.5f)offset=.5f;
  o->hz=(kbest+offset)*rate/R_N;o->confidence=best/total;
  o->valid=o->timing_ok&&!o->clipped&&o->rms>fmaxf(floor*3,o->source==1?.0007f:.002f)&&o->confidence>.18f && total>all*.15f;
 }
}
float r_ratio(const r_frame_t *a,const r_frame_t *b,bool *valid) {
 *valid=a->version==R_VERSION&&b->version==R_VERSION&&a->source==b->source&&a->source<2&&!a->clipped&&!b->clipped&&(a->source!=0||a->axis==b->axis)&&a->timing_ok&&b->timing_ok&&b->rms>1e-9f;
 return *valid?a->rms/b->rms:0;
}
