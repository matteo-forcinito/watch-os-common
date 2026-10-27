#include "measure.h"
#include "core/sys.h"
#include "hardware/imu.h"
#include "hardware/microphone.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static portMUX_TYPE guard=portMUX_INITIALIZER_UNLOCKED;
static r_measure_t state;
static int requested,calibrate;
static bool stop_requested;
static void publish(r_measure_t *s) {portENTER_CRITICAL(&guard);state=*s;portEXIT_CRITICAL(&guard);}
void r_measure_get(r_measure_t *out) {portENTER_CRITICAL(&guard);*out=state;portEXIT_CRITICAL(&guard);}
void r_measure_source(int source) {if(source>=0&&source<=2)__atomic_store_n(&requested,source,__ATOMIC_RELEASE);}
void r_measure_calibrate(void) {__atomic_store_n(&calibrate,1,__ATOMIC_RELEASE);}
void r_measure_stop(void) {__atomic_store_n(&stop_requested,true,__ATOMIC_RELEASE);}
typedef struct {float axes[3][R_N],ordered[R_N];long long times[R_N],ordered_times[R_N];int16_t pcm[R_N*2];} buffers_t;
static void worker(void *arg) {
 (void)arg;buffers_t *b=calloc(1,sizeof(*b));
 r_measure_t s={.source=-1};int active=-1,index=0,count=0,since=0;unsigned seq=0;float floor=.0006f;
 if(!b){s.error=ESP_ERR_NO_MEM;publish(&s);vTaskDelete(NULL);return;}
 while(!__atomic_load_n(&stop_requested,__ATOMIC_ACQUIRE)) {
  int source=__atomic_load_n(&requested,__ATOMIC_ACQUIRE);
  if(source!=active) {
   if(active==0){imu_stop();}else if(active==1){microphone_stop();}
   active=source;index=count=since=0;memset(&s,0,sizeof(s));s.source=source;
   s.error=source==0?imu_start():source==1?microphone_start(8000,1):ESP_OK;
   s.running=s.error==ESP_OK;publish(&s);
  }
  if(!s.running||source==2){vTaskDelay(pdMS_TO_TICKS(30));continue;}
  if(source==0) {
   imu_sample_t sample;esp_err_t err=imu_read(&sample);
   if(err==ESP_ERR_NOT_FINISHED){vTaskDelay(pdMS_TO_TICKS(1));continue;}
   if(err!=ESP_OK){s.errors++;s.error=err;publish(&s);vTaskDelay(pdMS_TO_TICKS(10));continue;}
   s.error=ESP_OK;
   if(__atomic_exchange_n(&calibrate,0,__ATOMIC_ACQ_REL)){index=count=since=0;s.calibrating=true;publish(&s);}
   for(int a=0;a<3;a++)b->axes[a][index]=sample.acceleration_g[a];
   b->times[index]=esp_timer_get_time();index=(index+1)%R_N;if(count<R_N)count++;since++;
   if(count<R_N||since<64){vTaskDelay(pdMS_TO_TICKS(1));continue;}since=0;
   float variance[3]={0},mean[3]={0};
   for(int a=0;a<3;a++) {
    for(int i=0;i<R_N;i++)mean[a]+=b->axes[a][i]/R_N;
    for(int i=0;i<R_N;i++){float x=b->axes[a][i]-mean[a];variance[a]+=x*x;}
   }
   int axis=variance[1]>variance[0]?1:0;if(variance[2]>variance[axis])axis=2;
   for(int i=0;i<R_N;i++){int at=(index+i)%R_N;b->ordered[i]=b->axes[axis][at];b->ordered_times[i]=b->times[at];}
   memset(&s.frame,0,sizeof(s.frame));s.frame.source=0;s.frame.axis=axis;
   float rate=0;s.frame.timing_ok=r_timing(b->ordered_times,&rate);
   for(int i=0;i<R_N;i++)if(fabsf(b->ordered[i])>3.95f)s.frame.clipped=true;
   r_analyze(b->ordered,rate,fminf(40,rate*.40f),floor,&s.frame);
   s.frame.tilt_x=atan2f(mean[0],sqrtf(mean[1]*mean[1]+mean[2]*mean[2]))*57.29578f;
   s.frame.tilt_y=atan2f(mean[1],sqrtf(mean[0]*mean[0]+mean[2]*mean[2]))*57.29578f;
   if(s.calibrating){floor=fmaxf(.0001f,s.frame.rms);s.calibrating=false;s.frame.noise=floor;s.frame.valid=false;}
  } else {
   size_t n=0;esp_err_t err=microphone_read(b->pcm,R_N*2,&n);
   if(err!=ESP_OK||n!=R_N){s.errors++;s.error=err==ESP_OK?ESP_ERR_INVALID_RESPONSE:err;publish(&s);vTaskDelay(pdMS_TO_TICKS(10));continue;}
   memset(&s.frame,0,sizeof(s.frame));s.frame.source=1;s.frame.timing_ok=true;
   for(int i=0;i<R_N;i++){b->ordered[i]=b->pcm[i]/32768.f;if(abs(b->pcm[i])>=32760)s.frame.clipped=true;}
   r_analyze(b->ordered,8000,2400,.0002f,&s.frame);s.error=ESP_OK;
  }
  s.frame.sequence=++seq;publish(&s);
 }
 if(active==0){imu_stop();}else if(active==1){microphone_stop();}free(b);s.running=false;publish(&s);vTaskDelete(NULL);
}
void r_measure_start(void) {sys_task_create(worker,"risonanza",16384,NULL,4,SYS_CORE_ANY);}
