/* Watch OS adapter. Renderer, fonts and interaction are upstream panel-core. */
#include "panel.h"
#include "landscape.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <initializer_list>
#include "upstream/layout_square.h"
#include "upstream/layout_big.h"
#include "upstream/layout_spring.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <new>
#include <time.h>
extern "C" int agentdeck_indices(const cJSON*,bool,bool,int*,int);

struct NativePanel {
    PanelLayout *layout{};
    SpringPanel *round{};
    RoundUi round_ui{};
    int width{},height{},physical_w{},physical_h{};
    PanelUi ui{};
    Fleet fleet{};
    lv_obj_t *image{};
    lv_timer_t *timer{};
    lv_image_dsc_t descriptor{};
    uint16_t *pixels{};
    int64_t received{};
};
static const char *str(const cJSON *o,const char *k){auto v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsString(v)?v->valuestring:"";}
static void paint(NativePanel *p){
    auto ms=(uint32_t)(esp_timer_get_time()/1000);
    if(p->round)roundTick(p->round_ui,*p->round,p->fleet,ms);else p->layout->tick(p->ui,p->fleet,ms);
    Canvas canvas{p->pixels,p->width,p->height};
    bool fresh=p->received&&esp_timer_get_time()-p->received<20000000;
    PanelInput input{&p->fleet,fresh?LinkState::Live:p->received?LinkState::Lost:LinkState::Connecting,"Aetherion",DeckScope::View,ms,(int64_t)time(nullptr),&p->ui,0};
    if(p->round){RoundInput ri{&p->fleet,input.link,input.device,input.scope,ms,input.nowEpoch,&p->round_ui,0};p->round->draw(canvas,ri);}
    else p->layout->draw(canvas,input);
    lv_image_cache_drop(&p->descriptor);lv_obj_invalidate(p->image);
}
static void frame(lv_timer_t *timer){paint((NativePanel*)lv_timer_get_user_data(timer));}
static void touch(lv_event_t *e){
    auto p=(NativePanel*)lv_event_get_user_data(e);
    if(lv_event_get_code(e)==LV_EVENT_DELETE){if(p->timer)lv_timer_delete(p->timer);lv_image_cache_drop(&p->descriptor);heap_caps_free(p->pixels);delete p->layout;delete p->round;spring_draw::releaseStamps();delete p;return;}
    auto input=lv_indev_active();if(!input)return;
    lv_point_t point;lv_indev_get_point(input,&point);lv_area_t bounds;lv_obj_get_coords(p->image,&bounds);
    bool down=lv_event_get_code(e)!=LV_EVENT_RELEASED&&lv_event_get_code(e)!=LV_EVENT_PRESS_LOST;
    int x=(point.x-bounds.x1)*p->width/p->physical_w,y=(point.y-bounds.y1)*p->height/p->physical_h;
    auto ms=(uint32_t)(esp_timer_get_time()/1000);
    if(p->round)roundTouch(p->round_ui,*p->round,p->fleet,down,x,y,DeckScope::View,time(nullptr),ms);
    else p->layout->touch(p->ui,p->fleet,down,x,y,DeckScope::View,ms);
    paint(p);
}
extern "C" lv_obj_t *agentdeck_panel_create(lv_obj_t *parent,int width,int height,bool round){
    auto p=new(std::nothrow) NativePanel;if(!p)return nullptr;
    p->physical_w=width;p->physical_h=height;
    if(round)p->round=new(std::nothrow) SpringPanel(layoutSpring21());
    else if(width>height)p->layout=new(std::nothrow) SquarePanel(specLandscape(width,height));
    else if(width==height)p->layout=new(std::nothrow) BigPanel(specBig480());
    else p->layout=new(std::nothrow) SquarePanel(width>=450&&height>=600?specT4S3():specAmoled18());
    if(!p->round&&!p->layout){delete p;return nullptr;}
    p->width=p->round?480:p->layout->width();p->height=p->round?480:p->layout->height();
    p->pixels=(uint16_t*)heap_caps_malloc(p->width*p->height*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!p->pixels){delete p->layout;delete p->round;spring_draw::releaseStamps();delete p;return nullptr;}
    p->descriptor.header.magic=LV_IMAGE_HEADER_MAGIC;p->descriptor.header.cf=LV_COLOR_FORMAT_RGB565;
    p->descriptor.header.w=p->width;p->descriptor.header.h=p->height;p->descriptor.header.stride=p->width*2;
    p->descriptor.data_size=p->width*p->height*2;p->descriptor.data=(uint8_t*)p->pixels;
    p->image=lv_image_create(parent);lv_obj_set_size(p->image,width,height);lv_image_set_src(p->image,&p->descriptor);
    lv_image_set_inner_align(p->image,LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_flag(p->image,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_flag(p->image,(lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_set_user_data(p->image,p);
    for(auto code:{LV_EVENT_PRESSED,LV_EVENT_PRESSING,LV_EVENT_RELEASED,LV_EVENT_PRESS_LOST,LV_EVENT_DELETE})lv_obj_add_event_cb(p->image,touch,code,p);
    p->timer=lv_timer_create(frame,100,p);paint(p);return p->image;
}
extern "C" void agentdeck_panel_update(lv_obj_t *object,const cJSON *cards,bool all,bool recent,int64_t received){
    auto p=(NativePanel*)lv_obj_get_user_data(object);int indices[16];int count=agentdeck_indices(cards,all,recent,indices,16);
    // Fleet is tens of KiB: keep it on the heap, never on the LVGL task stack.
    memset(&p->fleet,0,sizeof(p->fleet));Fleet &next=p->fleet;next.count=count;next.total=count;
    for(int j=0;j<count;j++){
        auto card=cJSON_GetArrayItem(cards,indices[j]);auto &a=next.agents[j];
        // Stable bounded identity; upstream IDs are 11 bytes, monitor IDs up to 100.
        uint32_t hash=2166136261u;for(auto s=str(card,"id");*s;s++)hash=(hash^(uint8_t)*s)*16777619u;
        snprintf(a.id,sizeof(a.id),"%08lx",(unsigned long)hash);a.slot=j;
        copyUtf8(a.title,sizeof(a.title),str(card,"name"));copyUtf8(a.actDetail,sizeof(a.actDetail),str(card,"activity"));
        copyUtf8(a.folder,sizeof(a.folder),str(card,"repo"));copyUtf8(a.branch,sizeof(a.branch),str(card,"branch"));
        copyUtf8(a.model,sizeof(a.model),str(card,"model"));/* No last prompt is exported by the bounded monitor protocol. */
        auto status=str(card,"status");a.status=!strcmp(status,"working")?Status::Running:!strcmp(status,"needs_input")?Status::Waiting:!strcmp(status,"error")||!strcmp(status,"interrupted")?Status::Error:Status::Idle;
        a.species=!strcmp(str(card,"agent"),"codex")?Species::Codex:Species::Claude;
        a.tool=!strcmp(str(card,"agent"),"codex")?ToolKind::Codex:!strcmp(str(card,"agent"),"claude")?ToolKind::Claude:ToolKind::Other;
        copyUtf8(a.actTool,sizeof(a.actTool),"task");a.costUsd=-1;a.usageKnown=false;a.turnsKnown=false;a.contextCountsKnown=false;
        auto context=cJSON_GetObjectItemCaseSensitive(card,"context_percent");if(cJSON_IsNumber(context)){a.ctxWindow=10000;a.ctxUsed=(uint32_t)(context->valuedouble*100);}
        auto used=cJSON_GetObjectItemCaseSensitive(card,"context_used"),window=cJSON_GetObjectItemCaseSensitive(card,"context_window");
        if(cJSON_IsNumber(used)&&cJSON_IsNumber(window)&&used->valuedouble<=UINT32_MAX&&window->valuedouble>0&&window->valuedouble<=UINT32_MAX){a.ctxUsed=(uint32_t)used->valuedouble;a.ctxWindow=(uint32_t)window->valuedouble;a.contextCountsKnown=true;}
        auto output=cJSON_GetObjectItemCaseSensitive(card,"tokens_output"),cached=cJSON_GetObjectItemCaseSensitive(card,"tokens_cached");
        if(cJSON_IsNumber(output)&&cJSON_IsNumber(cached)){a.tokensOut=(uint64_t)output->valuedouble;a.cacheRead=(uint64_t)cached->valuedouble;a.usageKnown=true;}
        auto at=cJSON_GetObjectItemCaseSensitive(card,"observed_at");if(cJSON_IsNumber(at))a.statusSince=(int64_t)at->valuedouble;
    }
    p->received=received;paint(p);
}
extern "C" void agentdeck_panel_seen(lv_obj_t *object,int64_t received){((NativePanel*)lv_obj_get_user_data(object))->received=received;}
extern "C" bool agentdeck_panel_back(lv_obj_t *object){auto p=(NativePanel*)lv_obj_get_user_data(object);if(p->round){if(p->round_ui.screen==RoundScreen::Home)return false;roundBack(p->round_ui,p->fleet);}else{if(p->ui.screen==Screen::Home)return false;p->layout->button(p->ui,p->fleet);}paint(p);return true;}
