/* On-demand mission control. No task, radio or retained snapshot when closed. */
#include "core/app.h"
#include "core/state.h"
#include "appkit.h"
#include "ui/ui.h"
#include "hardware/board.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "model.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

extern const char deck_start[] asm("_binary_agentdeck_dashboard_html_start");
extern const char deck_end[] asm("_binary_agentdeck_dashboard_html_end");
static SemaphoreHandle_t lock;
static StaticSemaphore_t lock_memory;
static cJSON *document;
static char *canonical;
static int64_t received;
static bool active;
static lv_obj_t *root, *header;
static lv_timer_t *timer;
static int offset, selected = -1;
static char selected_id[101];
static bool dashboard;
static void render(void);
static bool back(void);
static void gesture(lv_event_t *event)
{
    (void)event;
    lv_indev_t *input=lv_indev_active();if(!input)return;
    lv_dir_t direction=lv_indev_get_gesture_dir(input);
    if(direction!=LV_DIR_LEFT&&direction!=LV_DIR_RIGHT)return;
    lv_indev_wait_release(input);
    if(selected>=0||dashboard){back();return;}
    if(direction==LV_DIR_LEFT){offset+=4;render();}
    else if(offset){offset-=4;render();}
    else app_back_user();
}
static const char *value(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o,key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static const char *status_name(const char *s)
{
    if (!strcmp(s,"working")) return "In corso";
    if (!strcmp(s,"needs_input")) return "Serve il tuo input";
    if (!strcmp(s,"done")) return "Completato";
    if (!strcmp(s,"error")) return "Errore";
    if (!strcmp(s,"interrupted")) return "Interrotto";
    if (!strcmp(s,"idle")) return "Inattivo";
    return "Da verificare";
}
static uint32_t accent(const char *s)
{
    return !strcmp(s,"needs_input") || !strcmp(s,"working") ? 0xFF9559 :
           !strcmp(s,"done") ? 0xA8D5A0 : !strcmp(s,"error") ? 0xED9191 : 0x959AAB;
}
static cJSON *snapshot(void)
{
    xSemaphoreTake(lock,portMAX_DELAY);
    cJSON *out = document ? cJSON_Duplicate(document,true) : cJSON_CreateObject();
    xSemaphoreGive(lock);
    return out;
}
static esp_err_t get_state(httpd_req_t *request)
{
    if (!appkit_authorized(request)) return appkit_unauthorized(request);
    return appkit_json(request,snapshot());
}
static esp_err_t push(httpd_req_t *request)
{
    if (!appkit_authorized(request)) return appkit_unauthorized(request);
    char *raw = appkit_body(request,24576);
    cJSON *input = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    cJSON *next = agentdeck_validate(input);
    cJSON_Delete(input);
    if (!next) return appkit_error(request,"400 Bad Request","Snapshot non valido o oltre i limiti");
    /* generated_at changes every heartbeat; compare only the displayed cards. */
    char *printed = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(next,"cards"));
    if (!printed) { cJSON_Delete(next); return appkit_error(request,"503 Service Unavailable","Memoria insufficiente"); }
    xSemaphoreTake(lock,portMAX_DELAY);
    bool changed = !canonical || strcmp(canonical,printed);
    cJSON_Delete(document);document=next;
    free(canonical);canonical=printed;
    received=esp_timer_get_time();
    xSemaphoreGive(lock);
    if (changed) state_bump(STATE_AGENTDECK_VERSION);
    return appkit_json(request,cJSON_CreateObject());
}
static const httpd_uri_t routes[] = {
    {.uri="/api/state",.method=HTTP_GET,.handler=get_state},
    {.uri="/api/snapshot",.method=HTTP_POST,.handler=push}
};
static const appkit_web_config_t config = {.name="AgentDeck",.page_start=deck_start,.page_end=deck_end,
    .routes=routes,.route_count=sizeof(routes)/sizeof(routes[0]),.hotspot=true};
static void click(lv_event_t *event)
{
    intptr_t key=(intptr_t)lv_event_get_user_data(event);
    if(key==-1){selected=-1;selected_id[0]=0;dashboard=false;}
    else if(key==-2){offset=offset>=4?offset-4:0;}
    else if(key==-3){offset+=4;}
    else if(key==-4){dashboard=true;selected=-1;}
    else {
        selected=(int)key;
        cJSON *copy=snapshot();
        snprintf(selected_id,sizeof(selected_id),"%s",value(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(copy,"cards"),selected),"id"));
        cJSON_Delete(copy);
    }
    render();
}
static void caption(lv_timer_t *unused)
{
    (void)unused;
    if (!header) return;
    xSemaphoreTake(lock,portMAX_DELAY);int64_t at=received;xSemaphoreGive(lock);
    int age=at?(int)((esp_timer_get_time()-at)/1000000):-1;
    char text[100];
    if(age<0) snprintf(text,sizeof(text),"In attesa del monitor PC");
    else if(age>20) snprintf(text,sizeof(text),"Offline - ultimi dati %d s fa",age);
    else snprintf(text,sizeof(text),"Monitor collegato - %d s fa",age);
    lv_label_set_text(header,text);
    lv_obj_set_style_text_color(header,ui_color(age>=0&&age<=20?0xA8D5A0:0xFF9559),0);
}
static void robot(lv_obj_t *parent, const cJSON *card, int size)
{
    unsigned hash=0;for(const unsigned char *p=(const unsigned char *)value(card,"repo");*p;p++)hash=hash*31+*p;
    static const uint32_t colors[]={0xA8BBFA,0xE7AA89,0xB6CCA4,0xCCB0E0,0xE2CA85};
    lv_obj_t *body=lv_obj_create(parent);lv_obj_remove_style_all(body);lv_obj_set_size(body,size,size);
    lv_obj_remove_flag(body,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    static const uint8_t blocks[][4]={{4,5,8,6},{3,7,1,3},{12,7,1,3},{5,11,2,2},{9,11,2,2},{5,3,6,2},{5,7,2,2},{9,7,2,2},{7,10,2,1},{4,3,8,2},{6,1,4,2}};
    for(unsigned i=0;i<sizeof(blocks)/sizeof(blocks[0]);i++){
        lv_obj_t *pixel=lv_obj_create(body);lv_obj_remove_style_all(pixel);
        lv_obj_set_pos(pixel,blocks[i][0]*size/16,blocks[i][1]*size/16);
        lv_obj_set_size(pixel,blocks[i][2]*size/16,blocks[i][3]*size/16);
        lv_obj_set_style_bg_opa(pixel,LV_OPA_COVER,0);
        lv_obj_set_style_bg_color(pixel,ui_color(i>=6&&i<=8?0x20232A:i>=9?(!strcmp(value(card,"agent"),"claude")?0xF19462:0xA8BBFA):colors[hash%5]),0);
        lv_obj_remove_flag(pixel,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    }
}
static void label(lv_obj_t *parent,const char *text, uint32_t color)
{
    lv_obj_t *l=lv_label_create(parent);lv_label_set_text(l,text);
    lv_obj_set_width(l,LV_PCT(100));lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l,UI_FONT_SMALL,0);lv_obj_set_style_text_color(l,ui_color(color),0);
    lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
}
static void timestamp_label(lv_obj_t *parent,const char *prefix,const cJSON *number)
{
    if(!cJSON_IsNumber(number))return;
    time_t seconds=(time_t)number->valuedouble;struct tm local;
    if(!localtime_r(&seconds,&local))return;
    char date[24],text[64];strftime(date,sizeof(date),"%d/%m %H:%M",&local);
    snprintf(text,sizeof(text),"%s %s",prefix,date);label(parent,text,0x959AAB);
}
static void render(void)
{
    if(!active||!root)return;
    int scroll=selected>=0?lv_obj_get_scroll_y(root):0;
    lv_obj_clean(root);header=NULL;
    ui_title(root,"Agent Deck");
    if(dashboard){appkit_dashboard_page(root,ui_color(0xFF9559));ui_button(root,"Torna al deck",0x30333A,click,(void *)-1);return;}
    header=lv_label_create(root);lv_obj_set_style_text_font(header,UI_FONT_SMALL,0);caption(NULL);
    cJSON *copy=snapshot(),*cards=cJSON_GetObjectItemCaseSensitive(copy,"cards");int count=cJSON_GetArraySize(cards);
    if(selected>=0){
        cJSON *card=NULL;
        for(int i=0;i<count;i++)if(!strcmp(value(cJSON_GetArrayItem(cards,i),"id"),selected_id)){card=cJSON_GetArrayItem(cards,i);selected=i;break;}
        if(!card){selected=-1;label(root,"Sessione non piu presente nel monitor",0x959AAB);}
        else {
            robot(root,card,80);ui_title(root,value(card,"name"));
            label(root,status_name(value(card,"status")),accent(value(card,"status")));
            label(root,value(card,"activity"),0xEEEAE2);
            timestamp_label(root,"Rilevato:",cJSON_GetObjectItemCaseSensitive(card,"observed_at"));
            ui_section(root,"Progetto");label(root,value(card,"repo"),0xA8BBFA);label(root,value(card,"branch"),0x959AAB);
            if(value(card,"commit")[0])label(root,value(card,"commit"),0x959AAB);
            if(value(card,"model")[0])label(root,value(card,"model"),0x959AAB);
            const cJSON *context=cJSON_GetObjectItemCaseSensitive(card,"context_percent");
            char text[64];snprintf(text,sizeof(text),"Contesto: %s",cJSON_IsNumber(context)?"":"non disponibile");
            if(cJSON_IsNumber(context))snprintf(text,sizeof(text),"Contesto: %.0f%%",context->valuedouble);
            label(root,text,0x959AAB);
            const cJSON *limits=cJSON_GetObjectItemCaseSensitive(card,"quota"),*q;
            cJSON_ArrayForEach(q,limits){
                const cJSON *remaining=cJSON_GetObjectItemCaseSensitive(q,"remaining_percent");
                snprintf(text,sizeof(text),"Quota %s: %.0f%% residuo",value(q,"name"),remaining->valuedouble);label(root,text,0xA8D5A0);
                timestamp_label(root,"Reset:",cJSON_GetObjectItemCaseSensitive(q,"resets_at"));
            }
            if(value(card,"resume")[0]){ui_section(root,"Riprendi dal PC");label(root,value(card,"resume"),0xA8BBFA);}
            if(!strcmp(value(card,"status"),"needs_input"))label(root,"Rispondi nella sessione originale sul PC",0xFF9559);
        }
        ui_button(root,"Tutte le sessioni",0x30333A,click,(void *)-1);
    }else if(!count){
        label(root,"Apri il monitor sul PC e associa questo dispositivo dalla dashboard.",0x959AAB);
        label(root,"Codex e Claude Code compaiono dai loro dati locali. Build e rilasci vengono tracciati dal monitor.",0x959AAB);
    }else{
        if(offset>=count)offset=0;
        lv_obj_t *grid=lv_obj_create(root);lv_obj_remove_style_all(grid);lv_obj_set_width(grid,LV_PCT(100));lv_obj_set_height(grid,LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(grid,LV_FLEX_FLOW_ROW_WRAP);lv_obj_set_style_pad_column(grid,8,0);lv_obj_set_style_pad_row(grid,8,0);
        int width=board_info()->width;int tile_height=board_info()->height/2-85;
        if(tile_height<126)tile_height=126;
        for(int i=offset;i<count&&i<offset+4;i++){
            cJSON *card=cJSON_GetArrayItem(cards,i);const char *status=value(card,"status");
            lv_obj_t *tile=lv_obj_create(grid);lv_obj_set_width(tile,LV_PCT(48));lv_obj_set_height(tile,tile_height);
            lv_obj_set_style_bg_color(tile,ui_color(!strcmp(status,"needs_input")?0x583820:!strcmp(status,"working")?0x211B18:0x15171B),0);
            lv_obj_set_style_border_color(tile,ui_color(accent(status)),0);lv_obj_set_style_border_width(tile,1,0);
            lv_obj_set_style_radius(tile,12,0);lv_obj_set_style_pad_all(tile,8,0);lv_obj_set_style_pad_row(tile,3,0);
            lv_obj_set_flex_flow(tile,LV_FLEX_FLOW_COLUMN);lv_obj_set_flex_align(tile,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
            lv_obj_remove_flag(tile,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_event_cb(tile,click,LV_EVENT_CLICKED,(void *)(intptr_t)i);
            robot(tile,card,width>=360?48:32);
            label(tile,value(card,"name"),0xEEEAE2);
            /* Limit tile labels to one line; detail contains the full text. */
            lv_obj_t *name=lv_obj_get_child(tile,-1);lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);
            label(tile,!strcmp(status,"needs_input")?"Input richiesto":status_name(status),accent(status));
            label(tile,value(card,"repo"),0x959AAB);lv_label_set_long_mode(lv_obj_get_child(tile,-1),LV_LABEL_LONG_DOT);
            const cJSON *context=cJSON_GetObjectItemCaseSensitive(card,"context_percent");
            if(cJSON_IsNumber(context)){lv_obj_t *bar=lv_bar_create(tile);lv_obj_set_size(bar,LV_PCT(100),3);lv_bar_set_value(bar,(int)context->valuedouble,LV_ANIM_OFF);lv_obj_set_style_bg_color(bar,ui_color(0xFF9559),LV_PART_INDICATOR);}
        }
        char position[48];snprintf(position,sizeof(position),"Pagina %d / %d",offset/4+1,(count+3)/4);label(root,position,0x959AAB);
        if(offset)ui_button(root,"Precedenti",0x30333A,click,(void *)-2);
        if(offset+4<count)ui_button(root,"Successive",0x30333A,click,(void *)-3);
        const cJSON *total=cJSON_GetObjectItemCaseSensitive(copy,"total");
        if(cJSON_IsNumber(total)&&total->valueint>count)label(root,"Altre sessioni disponibili sul PC",0x959AAB);
    }
    cJSON_Delete(copy);
    ui_button(root,"Collegamento / dashboard",0x30333A,click,(void *)-4);
    if(scroll)lv_obj_scroll_to_y(root,scroll,LV_ANIM_OFF);
}
static void changed(lv_observer_t *observer,lv_subject_t *subject)
{
    (void)observer;(void)subject;
    /* Incoming PC activity must not rebuild the pairing QR under the user. */
    if(!dashboard)render();
}
static void create(lv_obj_t *screen,void *arg)
{
    (void)arg;
    if(!lock)lock=xSemaphoreCreateMutexStatic(&lock_memory);
    active=true;offset=0;selected=-1;selected_id[0]=0;dashboard=false;
    root=ui_page(screen,NULL);
    lv_obj_set_style_pad_top(root,board_info()->round?36:12,0);
    lv_obj_set_style_pad_bottom(root,board_info()->round?30:20,0);
    render();
    lv_obj_add_event_cb(screen,gesture,LV_EVENT_GESTURE,NULL);
    lv_subject_add_observer_obj(state_subject(STATE_AGENTDECK_VERSION),changed,root,NULL);
    timer=lv_timer_create(caption,1000,NULL);
    appkit_web_start(&config);
    appkit_web_activate(); /* This app's requested function is a live PC link. */
}
static void destroy(void)
{
    active=false;header=NULL;root=NULL;
    if(timer){lv_timer_delete(timer);timer=NULL;}
    appkit_web_stop_if(&config);
    xSemaphoreTake(lock,portMAX_DELAY);cJSON_Delete(document);document=NULL;free(canonical);canonical=NULL;received=0;xSemaphoreGive(lock);
}
static bool back(void)
{
    if(selected>=0||dashboard){selected=-1;selected_id[0]=0;dashboard=false;render();return true;}
    return false;
}
const app_t agentdeck_app={.id="agentdeck",.name="Agent Deck",.icon=LV_SYMBOL_LIST,.color=0xFF9559,.flags=APP_FLAG_NO_BACK_GESTURE|APP_FLAG_STAY_ON_WAKE,
    .create=create,.destroy=destroy,.back=back,
    .about="Stato locale di Codex, Claude Code, build e rilasci. Monitor PC richiesto, nessun processo in background ad app chiusa."};
