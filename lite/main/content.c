/* On-demand native summaries. No document, worker or row cache stays resident. */
#include "content.h"
#include "core/clock.h"
#include "giornata.h"
#include "life_store.h"
#include "services/alarm.h"
#include <stdio.h>
#include <string.h>
static lite_page_t shown;
static uint32_t shown_id;
static char result[80];
void lite_content_load(const lite_navigation_t *nav, lite_content_t *out)
{
    memset(out, 0, sizeof(*out));
    if (shown != nav->page) result[0] = 0;
    shown = nav->page; shown_id = 0;
    if (nav->page == LITE_ALARMS) {
        snprintf(out->heading, sizeof(out->heading), "SVEGLIE");
        alarm_t items[ALARM_MAX]; int count = alarm_list(items, ALARM_MAX);
        if (!count) { snprintf(out->title,sizeof(out->title),"NESSUNA SVEGLIA"); snprintf(out->detail[0],80,"AGGIUNGI DALLA DASHBOARD WEB"); return; }
        alarm_t *a = &items[nav->offset % count]; shown_id = a->id;
        snprintf(out->title,80,"%02u:%02u - %s",a->hour,a->minute,a->enabled?"ATTIVA":"SPENTA");
        snprintf(out->detail[0],80,"%s",a->label); alarm_days_text(a->days,out->detail[1],80);
        snprintf(out->detail[2],80,"%d DI %d",nav->offset % count + 1,count);
        snprintf(out->button,32,"%s",a->enabled?"DISATTIVA":"ATTIVA");
    } else if (nav->page == LITE_AGENDA) {
        snprintf(out->heading,32,"AGENDA");
        gd_lock(); int count; const gd_item_t *items = gd_items(&count); int available = 0; ics_time_t now = gd_now();
        for(int i=0;i<count;i++) if(items[i].end>=now) available++;
        int wanted = available ? nav->offset % available : 0;
        for(int i=0,found=0;i<count;i++) if(items[i].end>=now && found++==wanted) {
            snprintf(out->title,80,"%s",items[i].title);
            /* Giornata stores civil seconds; UTC conversion formats that civil time. */
            time_t t=items[i].start;struct tm tm;gmtime_r(&t,&tm);
            strftime(out->detail[0],80,"%d/%m/%Y - %H:%M",&tm);
            snprintf(out->detail[1],80,"EVENTI LOCALI E CALENDARI IN CACHE");break;
        }
        gd_unlock();
        if(!available) {snprintf(out->title,80,"NESSUN EVENTO IN ARRIVO");snprintf(out->detail[0],80,"GESTISCI GLI EVENTI DAL WEB");}
    } else {
        bool dogs = nav->page == LITE_DOGS;
        snprintf(out->heading,32,"%s",dogs?"ZAMPE":"RACCOLTA");
        cJSON *s=life_snapshot(dogs?LIFE_ZAMPE:LIFE_RACCOLTA);
        if (!s) {snprintf(out->title,80,"ARCHIVIO NON DISPONIBILE");return;}
        if (!dogs) {
            const cJSON *city=life_find(cJSON_GetObjectItemCaseSensitive(s,"cities"),life_number(s,"preferred",0));
            snprintf(out->title,80,"%s",city?life_string(city,"name"):"AGGIUNGI UNA CITTA DAL WEB");
            char date[11];life_date(life_today()+nav->offset%31,date);
            snprintf(out->detail[0],80,"%s%s",nav->offset%31==0?"OGGI - ":nav->offset%31==1?"DOMANI - ":"",date);
            cJSON *bins=life_collections(city,life_today()+nav->offset%31);const cJSON *b;int row=1;
            cJSON_ArrayForEach(b,bins) {
                int index=row<3?row:2;size_t used=strlen(out->detail[index]);
                snprintf(out->detail[index]+used,80-used,"%s%s",used?", ":"",life_string(b,"name")); row++;
            }
            if (!cJSON_GetArraySize(bins)) snprintf(out->detail[1],80,"NESSUN RITIRO");
            cJSON_Delete(bins);
        } else {
            const cJSON *tasks=cJSON_GetObjectItemCaseSensitive(s,"tasks"),*t;int count=0;
            cJSON_ArrayForEach(t,tasks) if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(t,"enabled"))) count++;
            int wanted=count?nav->offset%count:0;
            cJSON_ArrayForEach(t,tasks) if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(t,"enabled")) && wanted--==0) {
                shown_id=life_number(t,"id",0);snprintf(out->title,80,"%s",life_string(t,"label"));
                const cJSON *pet=life_find(cJSON_GetObjectItemCaseSensitive(s,"pets"),life_number(t,"pet",0));
                snprintf(out->detail[0],80,"%s",life_string(pet,"name"));int minute=life_number(t,"minute",0);
                snprintf(out->detail[1],80,"%s - %02d:%02d",life_string(t,"due"),minute/60,minute%60);
                snprintf(out->button,32,"SEGNA FATTO");break;
            }
            if(!count) {snprintf(out->title,80,"NESSUNA ATTIVITA ATTIVA");snprintf(out->detail[0],80,"CREA IL PROGRAMMA DAL WEB");}
        }
        if (life_string(s,"save_error")[0]) snprintf(result,80,"SALVATAGGIO NON RIUSCITO: APRI IL WEB");
        cJSON_Delete(s);
    }
    if (result[0]) snprintf(out->detail[2],80,"%s",result);
}
void lite_content_activate(lite_page_t page)
{
    if (page != shown || !shown_id) return;
    if (page == LITE_ALARMS) {
        alarm_t a;
        bool ok = alarm_get(shown_id,&a) && alarm_set_enabled(shown_id,!a.enabled) == ESP_OK;
        snprintf(result,80,"%s",ok?"IMPOSTAZIONE SALVATA":"OPERAZIONE NON RIUSCITA");
    } else if (page == LITE_DOGS) {
        cJSON *c=cJSON_CreateObject();cJSON_AddStringToObject(c,"op","task.done");cJSON_AddNumberToObject(c,"id",shown_id);
        char error[100];
        bool ok = life_command(LIFE_ZAMPE,c,error,sizeof(error));
        snprintf(result,80,"%.79s",ok?"SALVATAGGIO IN CORSO":error);
        cJSON_Delete(c);
    }
    shown_id=0;
}
