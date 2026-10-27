#include "store.h"
#include "nvs.h"
#include "hardware/sdcard.h"
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
static const char *key(int slot){return slot?"snapshot_b":"snapshot_a";}
bool r_store_load(int slot,r_frame_t *frame) {
 nvs_handle_t h;if(nvs_open("risonanza",NVS_READONLY,&h)!=ESP_OK)return false;
 size_t size=sizeof(*frame);esp_err_t err=nvs_get_blob(h,key(slot),frame,&size);nvs_close(h);
 return err==ESP_OK&&size==sizeof(*frame)&&frame->version==R_VERSION&&frame->source<2&&isfinite(frame->rms)&&frame->rate>0;
}
int r_store_save(int slot,const r_frame_t *frame) {
 nvs_handle_t h;if(nvs_open("risonanza",NVS_READWRITE,&h)!=ESP_OK)return -1;
 esp_err_t err=nvs_set_blob(h,key(slot),frame,sizeof(*frame));if(err==ESP_OK)err=nvs_commit(h);nvs_close(h);
 if(err!=ESP_OK)return -1;
 if(!sdcard_is_mounted())return 0;
 mkdir("/sdcard/risonanza",0775);char path[80];snprintf(path,sizeof(path),"/sdcard/risonanza/%c.csv",slot?'B':'A');
 FILE *f=fopen(path,"w");if(!f)return 0;
 bool ok=fprintf(f,"source,axis,sample_rate_hz,rms,peak,dominant_hz,dominant_valid,timing_ok,clipped,confidence,unit\n%u,%d,%.5f,%.9f,%.9f,%.5f,%d,%d,%d,%.5f,%s\nindex,signal_dc_removed\n",
 frame->source,frame->axis,(double)frame->rate,(double)frame->rms,(double)frame->peak,(double)frame->hz,frame->valid,frame->timing_ok,frame->clipped,(double)frame->confidence,frame->source==0?"g":"normalized_pcm")>0;
 for(int i=0;i<R_N&&ok;i++)ok=fprintf(f,"%d,%.9f\n",i,(double)frame->samples[i])>0;
 ok=fflush(f)==0&&ok;ok=fsync(fileno(f))==0&&ok;ok=fclose(f)==0&&ok;
 return ok?1:0;
}
