#include "plate.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "dsp.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define SIZE 176
#define GRAINS 680
typedef struct {float x,y,vx,vy;} grain_t;
struct r_plate {float *field;uint16_t *pixels,*base;grain_t grains[GRAINS];lv_image_dsc_t image;lv_obj_t *object;int mode;unsigned random;};
static float random_value(r_plate_t *p) {p->random=p->random*1664525u+1013904223u;return ((p->random>>8)&65535)/32767.5f-1;}
static uint16_t rgb(int r,int g,int b) {return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));}
r_plate_t *r_plate_create(lv_obj_t *parent,int x,int y,int size) {
 r_plate_t *p=heap_caps_calloc(1,sizeof(*p),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!p)return NULL;
 p->field=heap_caps_malloc(SIZE*SIZE*sizeof(float),MALLOC_CAP_SPIRAM);
 p->pixels=heap_caps_malloc(SIZE*SIZE*2,MALLOC_CAP_SPIRAM);p->base=heap_caps_malloc(SIZE*SIZE*2,MALLOC_CAP_SPIRAM);
 if(!p->field||!p->pixels||!p->base){free(p->field);free(p->pixels);free(p->base);free(p);return NULL;}
 p->random=5317;p->mode=-1;
 for(int i=0;i<GRAINS;i++){p->grains[i].x=(random_value(p)+1)*.5f*(SIZE-3)+1;p->grains[i].y=(random_value(p)+1)*.5f*(SIZE-3)+1;}
 p->image.header.magic=LV_IMAGE_HEADER_MAGIC;p->image.header.cf=LV_COLOR_FORMAT_RGB565;
 p->image.header.w=SIZE;p->image.header.h=SIZE;p->image.header.stride=SIZE*2;
 p->image.data_size=SIZE*SIZE*2;p->image.data=(const unsigned char*)p->pixels;
 p->object=lv_image_create(parent);lv_obj_set_pos(p->object,x,y);lv_obj_set_size(p->object,size,size);
 lv_image_set_src(p->object,&p->image);lv_image_set_inner_align(p->object,LV_IMAGE_ALIGN_STRETCH);
 return p;
}
void r_plate_frame(r_plate_t *p,int mode,float energy,bool frozen) {
 if(!p||frozen)return;
 if(mode!=p->mode) {
  int m,n;r_mode(mode,&m,&n);float cm[SIZE],cn[SIZE];
  for(int i=0;i<SIZE;i++){cm[i]=cosf(m*3.141592654f*i/(SIZE-1));cn[i]=cosf(n*3.141592654f*i/(SIZE-1));}
  for(int y=0;y<SIZE;y++)for(int x=0;x<SIZE;x++) {
   float f=cm[x]*cn[y]-cn[x]*cm[y];int at=y*SIZE+x;p->field[at]=f;
   float node=1/(1+f*f*160);p->base[at]=rgb(6+(int)(node*6),16+(int)(node*23),24+(int)(node*28));
  }
  p->mode=mode;
 }
 memcpy(p->pixels,p->base,SIZE*SIZE*2);
 energy=fmaxf(0,fminf(1,energy));
 for(int i=0;i<GRAINS;i++) {
  grain_t *g=&p->grains[i];int x=(int)g->x,y=(int)g->y,at=y*SIZE+x;
  float f=p->field[at],dx=(p->field[at+1]-p->field[at-1])*.5f,dy=(p->field[at+SIZE]-p->field[at-SIZE])*.5f;
  g->vx=(g->vx-7*f*dx*energy+random_value(p)*.12f*energy)*.88f;
  g->vy=(g->vy-7*f*dy*energy+random_value(p)*.12f*energy)*.88f;
  g->x+=g->vx;g->y+=g->vy;
  if(g->x<1){g->x=1;g->vx*=-.5f;}if(g->x>SIZE-2){g->x=SIZE-2;g->vx*=-.5f;}
  if(g->y<1){g->y=1;g->vy*=-.5f;}if(g->y>SIZE-2){g->y=SIZE-2;g->vy*=-.5f;}
  x=(int)g->x;y=(int)g->y;at=y*SIZE+x;
  p->pixels[at]=i%5?rgb(238,206,138):rgb(116,250,222);
  if(i%3==0 && x<SIZE-2)p->pixels[at+1]=rgb(159,153,105);
 }
 lv_image_cache_drop(&p->image);lv_obj_invalidate(p->object);
}
