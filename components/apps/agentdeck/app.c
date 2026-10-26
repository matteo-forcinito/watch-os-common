/* On-demand mission control. No task, radio or retained snapshot when closed. */
#include "core/app.h"
#include "core/state.h"
#include "appkit.h"
#include "ui/ui.h"
#include "hardware/board.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "model.h"
#include "critter.h"
#include "panel.h"
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
static lv_obj_t *native_panel;
static lv_timer_t *timer;
static lv_obj_t *sprites[5];
static int sprite_count;
static int offset, selected = -1;
static char selected_id[101];
static bool dashboard;
static bool show_all, sort_recent, controls;
static bool assistant_view;
static char assistant_draft[1001],displayed_question[37];
static struct {char thread_id[37],question_id[37],question[241],message[601];} assistant;
static const char *confirmation;
static struct {
    char id[33],action[32],status[24],result_id[80],message[181];
    char text[1001],question_id[37];
} command;
static int64_t control_seen;
static void render(void);
static bool back(void);
static void gesture(lv_event_t *event)
{
    (void)event;
    if(native_panel)return; /* Upstream renderer owns its touch gestures. */
    lv_indev_t *input=lv_indev_active();if(!input)return;
    lv_dir_t direction=lv_indev_get_gesture_dir(input);
    if(direction!=LV_DIR_LEFT&&direction!=LV_DIR_RIGHT)return;
    lv_indev_wait_release(input);
    if(selected>=0||dashboard||controls){back();return;}
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
    if (!strcmp(s,"pending")) return "Invio al Surface";
    if (!strcmp(s,"queued")) return "In coda sul Surface";
    if (!strcmp(s,"running")) return "In esecuzione";
    if (!strcmp(s,"succeeded")) return "Riuscito";
    if (!strcmp(s,"failed")) return "Non riuscito";
    if (!strcmp(s,"needs_review")) return "Controlla il report sul PC";
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
    return !strcmp(s,"needs_input") || !strcmp(s,"working") ? 0xFF4D00 :
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
static cJSON *control_snapshot(void)
{
    cJSON *out=cJSON_CreateObject(),*item=cJSON_AddObjectToObject(out,"request");
    cJSON_AddNumberToObject(out,"protocol",1);
    xSemaphoreTake(lock,portMAX_DELAY);
    cJSON_AddStringToObject(item,"id",command.id);cJSON_AddStringToObject(item,"action",command.action);
    cJSON_AddStringToObject(item,"status",command.status);cJSON_AddStringToObject(item,"result_id",command.result_id);
    cJSON_AddStringToObject(item,"message",command.message);
    cJSON_AddStringToObject(item,"text",command.text);cJSON_AddStringToObject(item,"question_id",command.question_id);
    cJSON *a=cJSON_AddObjectToObject(out,"assistant");cJSON_AddBoolToObject(a,"supported",true);
    xSemaphoreGive(lock);
    return out;
}
static bool enqueue(const char *action)
{
    if(!agentdeck_action_allowed(action))return false;
    xSemaphoreTake(lock,portMAX_DELAY);
    bool busy=!strcmp(command.status,"pending")||!strcmp(command.status,"queued")||!strcmp(command.status,"running");
    if(!busy){
        memset(&command,0,sizeof(command));
        snprintf(command.id,sizeof(command.id),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());
        snprintf(command.action,sizeof(command.action),"%s",action);
        snprintf(command.status,sizeof(command.status),"pending");
        snprintf(command.message,sizeof(command.message),"In attesa di Aetherion sul Surface");
    }
    xSemaphoreGive(lock);
    if(!busy)state_bump(STATE_AGENTDECK_VERSION);
    return !busy;
}
static esp_err_t get_control(httpd_req_t *request)
{
    if(!appkit_authorized(request))return appkit_unauthorized(request);
    xSemaphoreTake(lock,portMAX_DELAY);bool reconnected=!control_seen||esp_timer_get_time()-control_seen>20000000;control_seen=esp_timer_get_time();xSemaphoreGive(lock);
    if(reconnected)state_bump(STATE_AGENTDECK_VERSION);
    return appkit_json(request,control_snapshot());
}
static bool assistant_enqueue(const char *text,const char *question_id);
static esp_err_t request_control(httpd_req_t *request)
{
    if(!appkit_authorized(request))return appkit_unauthorized(request);
    char *raw=appkit_body(request,4096);cJSON *input=raw?cJSON_Parse(raw):NULL;free(raw);
    bool ai=!strcmp(value(input,"action"),"assistant-message");
    const cJSON *q=cJSON_GetObjectItemCaseSensitive(input,"question_id"),*t=cJSON_GetObjectItemCaseSensitive(input,"text");
    bool allowed=cJSON_IsObject(input)&&agentdeck_action_allowed(value(input,"action"))&&
        (ai?(cJSON_IsString(t)&&t->valuestring[0]&&strlen(t->valuestring)<=1000&&(!q||cJSON_IsString(q))&&strlen(value(input,"question_id"))<=36&&cJSON_GetArraySize(input)==(q?3:2)):cJSON_GetArraySize(input)==1);
    bool accepted=allowed&&(ai?assistant_enqueue(value(input,"text"),value(input,"question_id")):enqueue(value(input,"action")));cJSON_Delete(input);
    if(!allowed)return appkit_error(request,"400 Bad Request","Azione non consentita");
    if(!accepted)return appkit_error(request,"409 Conflict","Richiesta gia in corso");
    return appkit_json(request,control_snapshot());
}
static bool assistant_enqueue(const char *text,const char *question_id)
{
    if(!text||!text[0]||strlen(text)>1000||strlen(question_id)>36)return false;
    if(!enqueue("assistant-message"))return false;
    xSemaphoreTake(lock,portMAX_DELAY);
    snprintf(command.text,sizeof(command.text),"%s",text);snprintf(command.question_id,sizeof(command.question_id),"%s",question_id);
    xSemaphoreGive(lock);return true;
}
static esp_err_t assistant_update(httpd_req_t *request)
{
    if(!appkit_authorized(request))return appkit_unauthorized(request);
    char *raw=appkit_body(request,4096);cJSON *input=raw?cJSON_Parse(raw):NULL;free(raw);
    if(!cJSON_IsObject(input)||strlen(value(input,"thread_id"))>36||strlen(value(input,"question_id"))>36||strlen(value(input,"question"))>240||strlen(value(input,"message"))>600){cJSON_Delete(input);return appkit_error(request,"400 Bad Request","Messaggio assistente non valido");}
    xSemaphoreTake(lock,portMAX_DELAY);
    bool changed=strcmp(assistant.question_id,value(input,"question_id"))||strcmp(assistant.message,value(input,"message"));
    snprintf(assistant.thread_id,sizeof(assistant.thread_id),"%s",value(input,"thread_id"));snprintf(assistant.question_id,sizeof(assistant.question_id),"%s",value(input,"question_id"));
    snprintf(assistant.question,sizeof(assistant.question),"%s",value(input,"question"));snprintf(assistant.message,sizeof(assistant.message),"%s",value(input,"message"));
    xSemaphoreGive(lock);cJSON_Delete(input);if(changed)state_bump(STATE_AGENTDECK_VERSION);
    return appkit_json(request,cJSON_CreateObject());
}
static esp_err_t acknowledge_control(httpd_req_t *request)
{
    if(!appkit_authorized(request))return appkit_unauthorized(request);
    char *raw=appkit_body(request,1024);cJSON *input=raw?cJSON_Parse(raw):NULL;free(raw);
    const char *status=value(input,"status"),*id=value(input,"id");
    bool valid=cJSON_IsObject(input)&&strlen(value(input,"message"))<=180&&strlen(value(input,"result_id"))<=79&&
        (!strcmp(status,"queued")||!strcmp(status,"running")||!strcmp(status,"succeeded")||!strcmp(status,"failed")||!strcmp(status,"cancelled")||!strcmp(status,"interrupted")||!strcmp(status,"needs_review"));
    if(!valid){cJSON_Delete(input);return appkit_error(request,"400 Bad Request","Esito non valido");}
    xSemaphoreTake(lock,portMAX_DELAY);
    bool match=command.id[0]&&!strcmp(command.id,id);
    bool changed=match&&(strcmp(command.status,status)||strcmp(command.message,value(input,"message"))||strcmp(command.result_id,value(input,"result_id")));
    if(match){
        snprintf(command.status,sizeof(command.status),"%s",status);
        snprintf(command.message,sizeof(command.message),"%s",value(input,"message"));
        snprintf(command.result_id,sizeof(command.result_id),"%s",value(input,"result_id"));
    }
    xSemaphoreGive(lock);cJSON_Delete(input);
    if(!match)return appkit_error(request,"409 Conflict","Richiesta non piu attuale");
    if(changed)state_bump(STATE_AGENTDECK_VERSION);
    return appkit_json(request,control_snapshot());
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
    {.uri="/api/snapshot",.method=HTTP_POST,.handler=push},
    {.uri="/api/control",.method=HTTP_GET,.handler=get_control},
    {.uri="/api/control",.method=HTTP_POST,.handler=acknowledge_control},
    {.uri="/api/control/request",.method=HTTP_POST,.handler=request_control},
    {.uri="/api/assistant",.method=HTTP_POST,.handler=assistant_update}
};
static const appkit_web_config_t config = {.name="AgentDeck",.page_start=deck_start,.page_end=deck_end,
    .routes=routes,.route_count=sizeof(routes)/sizeof(routes[0]),.hotspot=true};
static void click(lv_event_t *event)
{
    intptr_t key=(intptr_t)lv_event_get_user_data(event);
    if(key==-1){selected=-1;selected_id[0]=0;dashboard=false;controls=false;assistant_view=false;confirmation=NULL;}
    else if(key==-2){offset=offset>=4?offset-4:0;}
    else if(key==-3){offset+=4;}
    else if(key==-4){dashboard=true;selected=-1;}
    else if(key==-5){show_all=!show_all;offset=0;}
    else if(key==-6){sort_recent=!sort_recent;offset=0;}
    else if(key==-7){controls=true;selected=-1;confirmation=NULL;}
    else if(key==-8){confirmation="verify-projects";}
    else if(key==-9){confirmation="build-amoled";}
    else if(key==-10){if(confirmation)enqueue(confirmation);confirmation=NULL;}
    else if(key==-11){assistant_view=true;controls=true;}
    else if(key==-12){if(assistant_enqueue(assistant_draft,displayed_question))assistant_draft[0]=0;}
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
    for(int i=0;i<sprite_count;i++)lv_obj_invalidate(sprites[i]);
    xSemaphoreTake(lock,portMAX_DELAY);int64_t at=received;xSemaphoreGive(lock);
    if(native_panel)agentdeck_panel_seen(native_panel,at);
    if (!header) return;
    int age=at?(int)((esp_timer_get_time()-at)/1000000):-1;
    char text[100];
    if(age<0) snprintf(text,sizeof(text),"In attesa del monitor PC");
    else if(age>20) snprintf(text,sizeof(text),"Offline - ultimi dati %d s fa",age);
    else snprintf(text,sizeof(text),"Monitor collegato - %d s fa",age);
    lv_label_set_text(header,text);
    lv_obj_set_style_text_color(header,ui_color(age>=0&&age<=20?0xA8D5A0:0xFF4D00),0);
}
static void draw_robot(lv_event_t *event)
{
    critter_t *critter=lv_event_get_user_data(event);
    if(lv_event_get_code(event)==LV_EVENT_DELETE){free(critter);return;}
    lv_obj_t *object=lv_event_get_target(event);lv_area_t area;lv_obj_get_coords(object,&area);
    critter_draw(lv_event_get_layer(event),critter,area.x1,area.y1,lv_obj_get_width(object)/18.0f,(uint32_t)(esp_timer_get_time()/1000));
}
static void robot(lv_obj_t *parent,const cJSON *card,int size)
{
    critter_t *critter=calloc(1,sizeof(*critter));if(!critter)return;
    const char *status=value(card,"status");
    critter->onigiri=!strcmp(value(card,"agent"),"codex");
    critter->body=critter->onigiri?0x4FB3A6:!strcmp(value(card,"agent"),"claude")?0xC98B6B:0xA8BBFA;
    critter->cut=0x141412;
    critter->pose=!strcmp(status,"working")?POSE_WRITE:!strcmp(status,"needs_input")?POSE_WAIT:
        !strcmp(status,"error")||!strcmp(status,"interrupted")?POSE_ERR:!strcmp(status,"done")?POSE_CELEBRATE:POSE_IDLE;
    lv_obj_t *object=lv_obj_create(parent);lv_obj_remove_style_all(object);lv_obj_set_size(object,size,size*13/18);
    lv_obj_remove_flag(object,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(object,draw_robot,LV_EVENT_DRAW_MAIN,critter);lv_obj_add_event_cb(object,draw_robot,LV_EVENT_DELETE,critter);
    if(sprite_count<5)sprites[sprite_count++]=object;
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
static void panel_menu(lv_event_t *event)
{
    lv_indev_t *input=lv_indev_active();if(!input)return;
    lv_point_t point;lv_indev_get_point(input,&point);
    if(lv_event_get_code(event)==LV_EVENT_LONG_PRESSED||(point.y<50&&point.x<130)){controls=true;render();}
    (void)event;
}
static void draft_changed(lv_event_t *event)
{
    const char *text=lv_textarea_get_text(lv_event_get_target(event));size_t n=strlen(text);
    if(n>=sizeof(assistant_draft)){n=sizeof(assistant_draft)-1;while(n&&((unsigned char)text[n]&0xC0)==0x80)n--;}
    memcpy(assistant_draft,text,n);assistant_draft[n]=0;
}
static void render(void)
{
    if(!active||!root)return;
    bool upstream=!dashboard&&!controls;
    if(upstream){
        cJSON *copy=snapshot();
        if(!native_panel){
            sprite_count=0;header=NULL;lv_obj_clean(root);
            lv_obj_set_style_pad_all(root,0,0);lv_obj_set_style_pad_row(root,0,0);
            lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
            native_panel=agentdeck_panel_create(root,board_info()->width,board_info()->height,board_info()->round);
            if(native_panel)lv_obj_add_event_cb(native_panel,panel_menu,LV_EVENT_SHORT_CLICKED,NULL);
            if(native_panel)lv_obj_add_event_cb(native_panel,panel_menu,LV_EVENT_LONG_PRESSED,NULL);
            else label(root,"Memoria PSRAM insufficiente per il deck",0xFF4D00);
        }
        xSemaphoreTake(lock,portMAX_DELAY);int64_t observed=received;xSemaphoreGive(lock);
        if(native_panel)agentdeck_panel_update(native_panel,cJSON_GetObjectItemCaseSensitive(copy,"cards"),show_all,sort_recent,observed);
        cJSON_Delete(copy);return;
    }
    native_panel=NULL;
    int scroll=selected>=0?lv_obj_get_scroll_y(root):0;
    sprite_count=0;lv_obj_clean(root);header=NULL;
    lv_obj_add_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(root,12,0);lv_obj_set_style_pad_row(root,8,0);
    ui_title(root,"Agent Deck");
    if(dashboard){appkit_dashboard_page(root,ui_color(0xFF4D00));ui_button(root,"Torna al deck",0x30333A,click,(void *)-1);return;}
    header=lv_label_create(root);lv_obj_set_style_text_font(header,UI_FONT_SMALL,0);caption(NULL);
    if(controls){
        if(assistant_view){
            /* The provider never receives an answer to a question the user did not see. */
            char question[241],message[601];
            xSemaphoreTake(lock,portMAX_DELAY);snprintf(question,sizeof(question),"%s",assistant.question);snprintf(message,sizeof(message),"%s",assistant.message);snprintf(displayed_question,sizeof(displayed_question),"%s",assistant.question_id);xSemaphoreGive(lock);
            ui_title(root,"Assistente Aetherion");label(root,question[0]?question:message,0xE8E4DC);
            lv_obj_t *input=lv_textarea_create(root);lv_obj_set_width(input,LV_PCT(100));lv_obj_set_height(input,65);lv_textarea_set_max_length(input,500);lv_textarea_set_placeholder_text(input,"Richiesta o risposta...");lv_textarea_set_text(input,assistant_draft);lv_obj_add_event_cb(input,draft_changed,LV_EVENT_VALUE_CHANGED,NULL);
            ui_button(root,"Invia input",0xFF4D00,click,(void *)-12);
            lv_obj_t *keyboard=lv_keyboard_create(root);lv_obj_set_size(keyboard,LV_PCT(100),145);lv_keyboard_set_textarea(keyboard,input);
            ui_button(root,"Torna al deck",0x30333A,click,(void *)-1);return;
        }
        ui_title(root,"Aetherion / comandi");
        ui_button(root,"Assistente / input",0xFF4D00,click,(void *)-11);
        ui_button(root,show_all?"Filtro: tutte":"Filtro: attive",0x30333A,click,(void *)-5);
        ui_button(root,sort_recent?"Ordine: recenti":"Ordine: priorita",0x30333A,click,(void *)-6);
        ui_button(root,"Collegamento / dashboard",0x30333A,click,(void *)-4);
        xSemaphoreTake(lock,portMAX_DELAY);int64_t seen=control_seen;xSemaphoreGive(lock);
        label(root,seen&&esp_timer_get_time()-seen<20000000?"Surface collegato":"Apri Aetherion sul Surface",0xA8BBFA);
        if(confirmation){
            label(root,!strcmp(confirmation,"build-amoled")?"Compilare il firmware AMOLED sul Surface? Nessun flash automatico.":"Avviare le verifiche dei progetti configurati sul Surface?",0xE8E4DC);
            ui_button(root,"Conferma avvio",0xFF4D00,click,(void *)-10);
        }else{
            cJSON *state=control_snapshot(),*item=cJSON_GetObjectItemCaseSensitive(state,"request");
            if(value(item,"id")[0]){
                ui_section(root,!strcmp(value(item,"action"),"build-amoled")?"Build AMOLED":"Verifica progetti");
                label(root,status_name(value(item,"status")),0xFF4D00);label(root,value(item,"message"),0xE8E4DC);
                if(value(item,"result_id")[0])label(root,value(item,"result_id"),0xA8BBFA);
            }
            bool busy=!strcmp(value(item,"status"),"pending")||!strcmp(value(item,"status"),"queued")||!strcmp(value(item,"status"),"running");
            if(!busy){
                ui_button(root,"Verifica progetti",0x30333A,click,(void *)-8);
                ui_button(root,"Compila AMOLED 1.8",0x30333A,click,(void *)-9);
            }
            cJSON_Delete(state);
        }
        label(root,"Richiesta conservata fino alla chiusura dell'app. Lavori gia presi in carico restano sul PC.",0x959AAB);
        ui_button(root,"Torna al deck",0x30333A,click,(void *)-1);return;
    }
    cJSON *copy=snapshot(),*cards=cJSON_GetObjectItemCaseSensitive(copy,"cards");int count=cJSON_GetArraySize(cards);
    int indices[16],visible=agentdeck_indices(cards,show_all,sort_recent,indices,16);
    if(selected>=0){
        cJSON *card=NULL;
        for(int i=0;i<count;i++)if(!strcmp(value(cJSON_GetArrayItem(cards,i),"id"),selected_id)){card=cJSON_GetArrayItem(cards,i);selected=i;break;}
        if(!card){selected=-1;label(root,"Sessione non piu presente nel monitor",0x959AAB);}
        else {
            robot(root,card,80);ui_title(root,value(card,"name"));
            label(root,status_name(value(card,"status")),accent(value(card,"status")));
            label(root,value(card,"activity"),0xE8E4DC);
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
            if(!strcmp(value(card,"status"),"needs_input"))label(root,"Rispondi nella sessione originale sul PC",0xFF4D00);
        }
        ui_button(root,"Tutte le sessioni",0x30333A,click,(void *)-1);
    }else if(!visible){
        if(count)label(root,"Nessuna sessione attiva. Premi Filtro: tutte per la cronologia.",0x959AAB);
        else {
        label(root,"Apri il monitor sul PC e associa questo dispositivo dalla dashboard.",0x959AAB);
        label(root,"Codex e Claude Code compaiono dai loro dati locali. Build e rilasci vengono tracciati dal monitor.",0x959AAB);
        }
    }else{
        if(offset>=visible)offset=0;
        lv_obj_t *grid=lv_obj_create(root);lv_obj_remove_style_all(grid);lv_obj_set_width(grid,LV_PCT(100));lv_obj_set_height(grid,LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(grid,LV_FLEX_FLOW_ROW_WRAP);lv_obj_set_style_pad_column(grid,8,0);lv_obj_set_style_pad_row(grid,8,0);
        int width=board_info()->width;int tile_height=board_info()->height/2-85;
        if(tile_height<126)tile_height=126;
        for(int position=offset;position<visible&&position<offset+4;position++){
            int i=indices[position];
            cJSON *card=cJSON_GetArrayItem(cards,i);const char *status=value(card,"status");
            lv_obj_t *tile=lv_obj_create(grid);lv_obj_set_width(tile,LV_PCT(48));lv_obj_set_height(tile,tile_height);
            lv_obj_set_style_bg_color(tile,ui_color(!strcmp(status,"needs_input")?0x583820:!strcmp(status,"working")?0x211B18:0x15171B),0);
            lv_obj_set_style_border_color(tile,ui_color(accent(status)),0);lv_obj_set_style_border_width(tile,1,0);
            lv_obj_set_style_radius(tile,4,0);lv_obj_set_style_pad_all(tile,8,0);lv_obj_set_style_pad_row(tile,3,0);
            lv_obj_set_flex_flow(tile,LV_FLEX_FLOW_COLUMN);lv_obj_set_flex_align(tile,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
            lv_obj_remove_flag(tile,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_event_cb(tile,click,LV_EVENT_CLICKED,(void *)(intptr_t)i);
            robot(tile,card,width>=360?48:32);
            label(tile,value(card,"name"),0xE8E4DC);
            /* Limit tile labels to one line; detail contains the full text. */
            lv_obj_t *name=lv_obj_get_child(tile,-1);lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);
            label(tile,!strcmp(status,"needs_input")?"Input richiesto":status_name(status),accent(status));
            label(tile,value(card,"repo"),0x959AAB);lv_label_set_long_mode(lv_obj_get_child(tile,-1),LV_LABEL_LONG_DOT);
            const cJSON *context=cJSON_GetObjectItemCaseSensitive(card,"context_percent");
            if(cJSON_IsNumber(context)){lv_obj_t *bar=lv_bar_create(tile);lv_obj_set_size(bar,LV_PCT(100),3);lv_bar_set_value(bar,(int)context->valuedouble,LV_ANIM_OFF);lv_obj_set_style_bg_color(bar,ui_color(0xFF4D00),LV_PART_INDICATOR);}
        }
        char position[48];snprintf(position,sizeof(position),"Pagina %d / %d",offset/4+1,(visible+3)/4);label(root,position,0x959AAB);
        if(offset)ui_button(root,"Precedenti",0x30333A,click,(void *)-2);
        if(offset+4<visible)ui_button(root,"Successive",0x30333A,click,(void *)-3);
        const cJSON *total=cJSON_GetObjectItemCaseSensitive(copy,"total");
        if(cJSON_IsNumber(total)&&total->valueint>count)label(root,"Altre sessioni disponibili sul PC",0x959AAB);
    }
    cJSON_Delete(copy);
    if(selected<0){
        ui_button(root,show_all?"Filtro: tutte":"Filtro: attive",0x30333A,click,(void *)-5);
        ui_button(root,sort_recent?"Ordine: recenti":"Ordine: priorita",0x30333A,click,(void *)-6);
        ui_button(root,"Comandi Aetherion",0xFF4D00,click,(void *)-7);
    }
    ui_button(root,"Collegamento / dashboard",0x30333A,click,(void *)-4);
    if(scroll)lv_obj_scroll_to_y(root,scroll,LV_ANIM_OFF);
}
static void changed(lv_observer_t *observer,lv_subject_t *subject)
{
    (void)observer;(void)subject;
    /* Incoming PC activity must not rebuild the pairing QR under the user. */
    if(!dashboard&&(!assistant_view||!assistant_draft[0]))render();
}
static void create(lv_obj_t *screen,void *arg)
{
    (void)arg;
    if(!lock)lock=xSemaphoreCreateMutexStatic(&lock_memory);
    active=true;native_panel=NULL;offset=0;selected=-1;selected_id[0]=0;dashboard=false;show_all=false;sort_recent=false;controls=false;assistant_view=false;confirmation=NULL;
    root=ui_page(screen,NULL);
    lv_obj_set_style_bg_color(root,ui_color(0x0A0A0A),0);lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);
    lv_obj_set_style_pad_top(root,board_info()->round?36:12,0);
    lv_obj_set_style_pad_bottom(root,board_info()->round?30:20,0);
    render();
    lv_obj_add_event_cb(screen,gesture,LV_EVENT_GESTURE,NULL);
    lv_subject_add_observer_obj(state_subject(STATE_AGENTDECK_VERSION),changed,root,NULL);
    timer=lv_timer_create(caption,250,NULL);
    appkit_web_start(&config);
    appkit_web_activate(); /* This app's requested function is a live PC link. */
}
static void destroy(void)
{
    active=false;sprite_count=0;header=NULL;native_panel=NULL;root=NULL;
    if(timer){lv_timer_delete(timer);timer=NULL;}
    appkit_web_stop_if(&config);
    xSemaphoreTake(lock,portMAX_DELAY);cJSON_Delete(document);document=NULL;free(canonical);canonical=NULL;received=0;control_seen=0;memset(&command,0,sizeof(command));memset(&assistant,0,sizeof(assistant));assistant_draft[0]=0;displayed_question[0]=0;xSemaphoreGive(lock);
}
static bool back(void)
{
    if(native_panel&&agentdeck_panel_back(native_panel))return true;
    if(assistant_view){assistant_view=false;render();return true;}
    if(confirmation){confirmation=NULL;render();return true;}
    if(selected>=0||dashboard||controls){selected=-1;selected_id[0]=0;dashboard=false;controls=false;render();return true;}
    return false;
}
const app_t agentdeck_app={.id="agentdeck",.name="Agent Deck",.icon=LV_SYMBOL_LIST,.color=0xFF4D00,.flags=APP_FLAG_NO_BACK_GESTURE|APP_FLAG_STAY_ON_WAKE,
    .create=create,.destroy=destroy,.back=back,
    .about="Stato locale di Codex, Claude Code, build e rilasci. Monitor PC richiesto, nessun processo in background ad app chiusa."};
