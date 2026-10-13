/* Fullscreen radar entrance. Points are observations, never invented hosts.
 * Pages: 0 radar, 1 menu, 2 devices of one network (the current one by default),
 * 3 dashboard, 4 device, 5 diagnostics, 6 stored networks, 7 Wi-Fi, 8 quality, 9 find. */
#include "sentinel.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "ui/ui.h"
#include "hardware/board.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RADAR, MENU, DEVICES, DASHBOARD, DEVICE, DIAGNOSTICS, NETWORKS, WIFI, QUALITY, FIND };

typedef struct
{
    lv_obj_t *screen, *content, *border, *headline, *caption, *sweep, *legend;
    lv_obj_t *dots[24];
    lv_obj_t *network_text, *live, *live2, *bar, *chart;
    lv_chart_series_t *series;
    lv_point_precise_t line[2];
    lv_timer_t *timer;
    int page, offset, selected, angle, field;
    uint32_t rendered_revision;
    char filter[33]; /* network shown in the device list; "" = the current network */
    char ssid[33];   /* access point chosen for joining */
    sn_ap_t aps[SN_APS];
    int ap_count;
    sn_status_t status;
} sentinel_ui_t;
static sentinel_ui_t *s_ui;
static void render(sentinel_ui_t *ui);

static void result(esp_err_t error)
{
    if (error == ESP_OK) ui_toast("Operazione avviata");
    else ui_toast(error == ESP_ERR_INVALID_STATE ? "Attendi o arresta la scansione" : "Dati non validi o memoria occupata");
}

static void go(sentinel_ui_t *ui, int page)
{
    ui->page = page;
    ui->offset = 0;
    render(ui);
}

static bool current_network(const sentinel_ui_t *ui)
{
    return !ui->filter[0] || strcmp(ui->filter,ui->status.network) == 0;
}

static void action(lv_event_t *event)
{
    sentinel_ui_t *ui = s_ui;
    int action_id = (int)(uintptr_t)lv_event_get_user_data(event);
    sn_device_t device;
    if (action_id == 1) result(sn_scan(""));
    else if (action_id == 2) { ui->filter[0] = 0; go(ui,DEVICES); }
    else if (action_id == 3) go(ui,DASHBOARD);
    else if (action_id == 4) { sn_cancel(); ui_toast("Arresto richiesto"); }
    else if (action_id == 5) go(ui,DIAGNOSTICS);
    else if (action_id == 6) result(sn_exit(false));
    else if (action_id == 7) result(sn_settings(ui->status.interval == 0 ? 60 : ui->status.interval == 60 ? 300 : 0));
    else if (action_id == 8) { ui->offset = ui->offset >= 8 ? ui->offset-8 : 0; render(ui); }
    else if (action_id == 9) { ui->offset += 8; render(ui); }
    else if (action_id == 10) go(ui,NETWORKS);
    else if (action_id == 11 && sn_device_at(ui->selected,&device)) { result(sn_diagnose(device.ip)); go(ui,DIAGNOSTICS); }
    else if (action_id == 12 && sn_device_at(ui->selected,&device)) { device.trusted = !device.trusted; result(sn_edit(&device)); }
    else if (action_id == 13 && sn_device_at(ui->selected,&device)) { result(sn_identify(device.ip)); go(ui,DIAGNOSTICS); }
    else if (action_id == 14 && sn_device_at(ui->selected,&device))
    {
        esp_err_t error = sn_find(device.id);
        if (error == ESP_OK) go(ui,FIND); else result(error);
    }
    else if (action_id == 15) go(ui,WIFI);
    else if (action_id == 16) result(sn_wifi_scan());
    else if (action_id == 17) go(ui,QUALITY);
    else if (action_id == 18) result(sn_quality_start(30));
    else if (action_id == 19) result(sn_speed_start(10,false));
    else if (action_id == 20) result(sn_speed_start(2,true));
}

static void discard(bool confirmed, void *unused)
{
    (void)unused;
    if (confirmed) result(sn_exit(true));
}
static void leave_unsaved(lv_event_t *event)
{
    (void)event;
    ui_confirm("Esci senza salvare?","Le modifiche rimaste in RAM andranno perse.","Esci",UI_COLOR_RED,discard,NULL);
}

static void enter(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) go(s_ui,MENU);
    else if (lv_event_get_code(event) == LV_EVENT_GESTURE)
    {
        lv_indev_t *input = lv_indev_active();
        if (input && lv_indev_get_gesture_dir(input) == LV_DIR_TOP)
        { lv_indev_wait_release(input); go(s_ui,MENU); }
    }
}

static lv_obj_t *text_at(lv_obj_t *parent, const char *text, int y, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label,LV_PCT(76));
    lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_color(label,ui_color(color),0);
    lv_obj_set_style_text_font(label,font,0);
    lv_label_set_text(label,text);
    lv_obj_align(label,LV_ALIGN_TOP_MID,0,y);
    lv_obj_set_clickable(label,false);
    return label;
}

static void scene(sentinel_ui_t *ui)
{
    int w = board_info()->width, h = board_info()->height;
    int diameter = (w < h ? w : h)*86/100, center_y = h*46/100;
    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *ring = lv_obj_create(ui->content);
        lv_obj_remove_style_all(ring);
        int size = diameter*(4-i)/4;
        lv_obj_set_size(ring,size,size);
        lv_obj_set_pos(ring,(w-size)/2,center_y-size/2);
        lv_obj_set_style_radius(ring,LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_border_width(ring,1,0);
        lv_obj_set_style_border_color(ring,ui_color(0x174B4A),0);
        lv_obj_set_style_border_opa(ring,LV_OPA_COVER,0);
        lv_obj_set_clickable(ring,false);
    }
    ui->sweep = lv_line_create(ui->content);
    lv_obj_set_style_line_color(ui->sweep,ui_color(0x48E3B7),0);
    lv_obj_set_style_line_width(ui->sweep,2,0);
    lv_obj_set_style_line_opa(ui->sweep,LV_OPA_50,0);
    lv_obj_set_clickable(ui->sweep,false);
    for (int i = 0; i < 24; i++)
    {
        ui->dots[i] = lv_obj_create(ui->content);
        lv_obj_remove_style_all(ui->dots[i]);
        lv_obj_set_size(ui->dots[i],7,7);
        lv_obj_set_style_radius(ui->dots[i],LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_bg_color(ui->dots[i],ui_color(0x57E9C2),0);
        lv_obj_set_style_bg_opa(ui->dots[i],LV_OPA_COVER,0);
        lv_obj_set_hidden(ui->dots[i],true);
        lv_obj_set_clickable(ui->dots[i],false);
    }
    lv_obj_t *title = text_at(ui->content,"SENTINEL",h*12/100,ui_font_title,0xB8EBE1);
    lv_obj_set_style_text_letter_space(title,2,0);
    char count[16]; snprintf(count,sizeof(count),"%d",ui->status.cycle ? ui->status.observed : ui->status.here);
    ui->headline = text_at(ui->content,count,center_y-(h >= 320 ? 72 : 36),h >= 320 ? ui_font_clock : ui_font_big,0xE9FFF8);
    ui->legend = text_at(ui->content,"dispositivi in questa rete",center_y+(h >= 320 ? 76 : 38),UI_FONT_SMALL,0x90C7BC);
    ui->caption = text_at(ui->content,"Connessione...",h*74/100,UI_FONT_SMALL,0x92AEB5);
    text_at(ui->content,h >= 320 ? "Tocca per esplorare" : "Esplora",h*89/100,UI_FONT_SMALL,0x4DE2BB);
    lv_obj_set_clickable(ui->content,true);
    lv_obj_set_gesture_bubble(ui->content,false);
    lv_obj_add_event_cb(ui->content,enter,LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(ui->content,enter,LV_EVENT_GESTURE,NULL);
    lv_port_set_focusable(ui->content);
}

static void select_device(lv_event_t *event)
{
    s_ui->selected = (int)(uintptr_t)lv_event_get_user_data(event)-1;
    go(s_ui,DEVICE);
}

static void edited(const char *text, void *unused)
{
    (void)unused;
    if (text == NULL) return;
    sn_device_t device;
    if (!sn_device_at(s_ui->selected,&device)) { ui_toast("Riprova"); return; }
    char *target = s_ui->field == 0 ? device.label : s_ui->field == 1 ? device.category : device.note;
    size_t size = s_ui->field == 0 ? sizeof(device.label) : s_ui->field == 1 ? sizeof(device.category) : sizeof(device.note);
    if (strlen(text) >= size) { ui_toast("Testo troppo lungo"); return; }
    snprintf(target,size,"%s",text);
    result(sn_edit(&device));
}

static void edit(lv_event_t *event)
{
    s_ui->field = (int)(uintptr_t)lv_event_get_user_data(event);
    sn_device_t device;
    if (!sn_device_at(s_ui->selected,&device)) return;
    ui_text_input(s_ui->field == 0 ? "Nome personale" : s_ui->field == 1 ? "Categoria" : "Note",
        s_ui->field == 0 ? device.label : s_ui->field == 1 ? device.category : device.note,false,edited,NULL);
}

/* Name for a row: user label, announced names, then the deduction or the vendor. */
static void row_name(const sn_device_t *d, char *out, size_t size)
{
    const char *name = sn_display_name(d);
    if (name != d->ip) { snprintf(out,size,"%s",name); return; }
    char type[64], reasons[160], vendor[48];
    bool gateway = strcmp(d->ip,s_ui->status.gateway) == 0;
    if (sn_guess(d,gateway,type,sizeof(type),reasons,sizeof(reasons)) && strcmp(type,"Non determinato") != 0)
    { snprintf(out,size,"%s?",type); return; }
    sn_vendor(d,vendor,sizeof(vendor));
    snprintf(out,size,"%s",strcmp(vendor,"MAC non rilevato") == 0 ? "Dispositivo" : vendor);
}

static void detail(sentinel_ui_t *ui, lv_obj_t *page)
{
    sn_device_t d;
    if (!sn_device_at(ui->selected,&d)) { ui_text(page,"Dispositivo non disponibile",true); return; }
    char name[80]; row_name(&d,name,sizeof(name));
    ui_title(page,name);
    char vendor[48], names[200] = "", text[640];
    sn_vendor(&d,vendor,sizeof(vendor));
    if (d.hostname[0]) snprintf(names,sizeof(names),"%s",d.hostname);
    if (d.dns[0]) { size_t u = strlen(names); snprintf(names+u,sizeof(names)-u,"%s%s",u ? "\n" : "",d.dns); }
    if (d.netbios[0]) { size_t u = strlen(names); snprintf(names+u,sizeof(names)-u,"%sPC: %s",u ? "\n" : "",d.netbios); }
    bool now = d.seen_cycle == ui->status.cycle && ui->status.cycle;
    char state[40];
    if (!now) snprintf(state,sizeof(state),"Non rilevato ora");
    else if (!sn_confirmed(&d)) snprintf(state,sizeof(state),"Risposta proxy ARP");
    else if (d.rtt_ms >= 0) snprintf(state,sizeof(state),"Rilevato ora · ping %d ms",d.rtt_ms);
    else snprintf(state,sizeof(state),"Rilevato ora");
    snprintf(text,sizeof(text),"%s\n%s\n%s%s%s%s%s\n%s",d.ip,d.mac[0] ? d.mac : "MAC non rilevato",vendor,
        names[0] ? "\n" : "",names,d.model[0] ? "\n" : "",d.model,state);
    ui_text(page,text,false);
    if (d.services[0]) ui_text(page,d.services,true);
    char type[64], reasons[160], guess[260];
    sn_guess(&d,strcmp(d.ip,ui->status.gateway) == 0,type,sizeof(type),reasons,sizeof(reasons));
    ui_section(page,"Deduzione");
    snprintf(guess,sizeof(guess),"%s\nIndizi: %s",type,reasons);
    ui_text(page,guess,true);
    ui_row(page,LV_SYMBOL_EDIT,UI_COLOR_TEAL,"Nome",d.label[0] ? d.label : "Assegna",edit,(void *)0);
    ui_row(page,LV_SYMBOL_LIST,UI_COLOR_BLUE,"Categoria",d.category[0] ? d.category : "Assegna",edit,(void *)1);
    ui_row(page,LV_SYMBOL_EDIT,UI_COLOR_GRAY,"Note",d.note[0] ? "Modifica" : "Aggiungi",edit,(void *)2);
    ui_row(page,LV_SYMBOL_OK,UI_COLOR_GREEN,"Fidato",d.trusted ? "Si" : "No",action,(void *)12);
    if (strcmp(d.network,ui->status.network) == 0)
    {
        ui_button(page,"Identifica",UI_COLOR_TEAL,action,(void *)13);
        if (d.mac[0] && !(d.sources & SN_PROXY)) ui_button(page,"Trova: caldo / freddo",UI_COLOR_ORANGE,action,(void *)14);
        ui_button(page,"Diagnostica",UI_COLOR_PURPLE,action,(void *)11);
        ui_text(page,"Identifica chiede nome, descrizione e 28 servizi comuni. Trova misura il segnale del dispositivo mentre ti sposti.",true);
    }
    else ui_text(page,"Rete non collegata: dati dell'ultima osservazione. Identifica e Trova funzionano solo sulla rete attuale.",true);
}

typedef struct { uint32_t ip; int index; } entry_t;
static int by_ip(const void *a, const void *b)
{
    uint32_t x = ((const entry_t *)a)->ip, y = ((const entry_t *)b)->ip;
    return x < y ? -1 : x > y;
}

static void devices(sentinel_ui_t *ui, lv_obj_t *page)
{
    const char *network = ui->filter[0] ? ui->filter : ui->status.network;
    sn_network_t info = {0};
    int stored = 0;
    for (int i = 0; i < ui->status.networks; i++)
        if (sn_network_at(i,&info,&stored) && strcmp(info.id,network) == 0) break;
        else info.id[0] = 0;
    ui_title(page,current_network(ui) ? "Questa rete" : info.label[0] ? info.label : "Rete archiviata");
    if (!network[0]) { ui_text(page,"Wi-Fi non connesso: apri Altre reti per l'archivio.",true); return; }
    if (info.id[0])
    {
        char text[96]; snprintf(text,sizeof(text),"%s · %s",info.ssid,info.subnet);
        ui_text(page,text,true);
    }
    entry_t *list = malloc(sizeof(entry_t)*SN_DEVICES);
    sn_device_t *rows = malloc(sizeof(sn_device_t)*8);
    int matched = 0, shown = 0;
    if (list && rows && xSemaphoreTake(sn_mutex,pdMS_TO_TICKS(50)) == pdTRUE)
    {
        for (int i = 0; i < sn_model->device_count; i++)
        {
            struct in_addr ip;
            if (strcmp(sn_model->devices[i].network,network) != 0) continue;
            list[matched].ip = inet_pton(AF_INET,sn_model->devices[i].ip,&ip) == 1 ? ntohl(ip.s_addr) : 0;
            list[matched++].index = i;
        }
        qsort(list,matched,sizeof(entry_t),by_ip);
        for (int i = ui->offset; i < matched && shown < 8; i++) rows[shown++] = sn_model->devices[list[i].index];
        xSemaphoreGive(sn_mutex);
    }
    for (int i = 0; i < shown; i++)
    {
        const sn_device_t *d = &rows[i];
        char name[80], value[40];
        row_name(d,name,sizeof(name));
        snprintf(value,sizeof(value),"%s%s",d->ip,d->trusted ? "  OK" : "");
        bool now = d->seen_cycle == ui->status.cycle && ui->status.cycle;
        ui_row(page,LV_SYMBOL_WIFI,now ? sn_confirmed(d) ? UI_COLOR_TEAL : UI_COLOR_ORANGE : UI_COLOR_GRAY,name,value,
               select_device,(void *)(uintptr_t)(list[ui->offset+i].index+1));
    }
    if (!matched) ui_text(page,"Nessun dispositivo. Avvia una scansione dalla pagina principale.",true);
    if (ui->offset) ui_button(page,"Precedenti",UI_COLOR_GRAY,action,(void *)8);
    if (matched > ui->offset+8) ui_button(page,"Successivi",UI_COLOR_GRAY,action,(void *)9);
    if (matched) ui_text(page,"Il nome con ? e una deduzione. Tocca per i dettagli e Identifica.",true);
    free(list);
    free(rows);
}

static void open_network(lv_event_t *event)
{
    sn_network_t network;
    int devices;
    if (!sn_network_at((int)(uintptr_t)lv_event_get_user_data(event),&network,&devices)) return;
    snprintf(s_ui->filter,sizeof(s_ui->filter),"%s",network.id);
    go(s_ui,DEVICES);
}

static void networks(sentinel_ui_t *ui, lv_obj_t *page)
{
    ui_title(page,"Reti archiviate");
    for (int i = 0; i < ui->status.networks; i++)
    {
        sn_network_t network;
        int devices;
        if (!sn_network_at(i,&network,&devices)) continue;
        char value[40];
        bool here = strcmp(network.id,ui->status.network) == 0;
        snprintf(value,sizeof(value),"%d disp.%s",devices,here ? " · attuale" : "");
        ui_row(page,LV_SYMBOL_WIFI,here ? UI_COLOR_GREEN : UI_COLOR_BLUE,network.label[0] ? network.label : network.ssid,value,
               open_network,(void *)(uintptr_t)i);
    }
    if (!ui->status.networks) ui_text(page,"Nessuna rete archiviata.",true);
    ui_text(page,"Ogni profilo unisce SSID, sottorete e gateway: la stessa rete con un altro router appare separata.",true);
}

static void joined(const char *text, void *unused)
{
    (void)unused;
    if (text == NULL) return;
    result(sn_wifi_join(s_ui->ssid,text));
}

static void join_confirmed(bool confirmed, void *argument)
{
    if (confirmed) result(sn_wifi_join(s_ui->ssid,argument ? "" : NULL));
}

static void choose_ap(lv_event_t *event)
{
    int index = (int)(uintptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= s_ui->ap_count) return;
    const sn_ap_t *ap = &s_ui->aps[index];
    if (ap->current) { ui_toast("Rete gia in uso"); return; }
    if (!ap->ssid[0]) { ui_toast("Rete nascosta: non supportata"); return; }
    snprintf(s_ui->ssid,sizeof(s_ui->ssid),"%s",ap->ssid);
    if (ap->known) ui_confirm("Cambiare rete?",ap->ssid,"Collega",UI_COLOR_TEAL,join_confirmed,NULL);
    else if (ap->open) ui_confirm("Rete aperta, senza cifratura",ap->ssid,"Collega",UI_COLOR_ORANGE,join_confirmed,(void *)1);
    else ui_text_input("Password Wi-Fi","",true,joined,NULL);
}

static void wifi(sentinel_ui_t *ui, lv_obj_t *page)
{
    ui_title(page,"Wi-Fi");
    ui->ap_count = sn_wifi_list(ui->aps,SN_APS);
    if (ui->ap_count < 0) ui->ap_count = 0;
    char text[200];
    snprintf(text,sizeof(text),"%s · canale %d · %d dBm",ui->status.connected ? ui->status.ssid : "Non connesso",
             ui->status.channel,ui->status.rssi);
    ui_text(page,text,false);
    ui_button(page,"Cerca reti",UI_COLOR_TEAL,action,(void *)16);
    if (ui->ap_count)
    {
        /* 2.4 GHz channels overlap: count every access point within 2 channels of 1, 6 and 11. */
        int load[14] = {0}, best = 1;
        for (int i = 0; i < ui->ap_count; i++)
            for (int c = 1; c <= 13; c++)
                if (ui->aps[i].channel && abs(ui->aps[i].channel - c) <= 2) load[c]++;
        static const int candidates[] = {1,6,11};
        for (int i = 0; i < 3; i++) if (load[candidates[i]] < load[best]) best = candidates[i];
        int mine = ui->status.channel >= 1 && ui->status.channel <= 13 ? load[ui->status.channel] : 0;
        snprintf(text,sizeof(text),"Canale in uso %d: %d reti vicine.\nPiu libero tra 1, 6, 11: canale %d (%d).%s",
                 ui->status.channel,mine,best,load[best],
                 mine > load[best]+2 ? "\nCambialo dal router se la rete e lenta." : "");
        ui_text(page,text,true);
    }
    for (int i = 0; i < ui->ap_count; i++)
    {
        const sn_ap_t *ap = &ui->aps[i];
        char value[48];
        if (ap->channel) snprintf(value,sizeof(value),"ch %d · %d dBm%s",ap->channel,ap->rssi,ap->current ? " · in uso" : "");
        else snprintf(value,sizeof(value),"%d dBm",ap->rssi);
        uint32_t color = ap->current ? UI_COLOR_GREEN : ap->known ? UI_COLOR_TEAL : ap->open ? UI_COLOR_ORANGE : UI_COLOR_GRAY;
        ui_row(page,ap->known ? LV_SYMBOL_OK : LV_SYMBOL_WIFI,color,ap->ssid[0] ? ap->ssid : "(nascosta)",value,
               choose_ap,(void *)(uintptr_t)i);
    }
    ui_text(page,"Solo 2,4 GHz. Verde: in uso, azzurro: salvata, arancio: aperta. Cambiando rete la dashboard sul PC va riaperta.",true);
}

static void quality_text(const sn_quality_t *q, char *out, size_t size)
{
    int loss = q->gateway_sent ? q->gateway_lost*100/q->gateway_sent : 0;
    if (q->gateway_sent == 0) { snprintf(out,size,"Nessuna misura: avvia la misura della qualita."); return; }
    snprintf(out,size,"%s %d/%d s\nSegnale %d dBm (min %d, max %d)\nGateway %d ms (min %d, max %d)\nJitter %d ms · persi %d%%\nInternet %s%d ms · DNS %d ms",
             q->running ? "In corso" : "Ultima misura",q->seconds,q->duration,q->rssi,q->rssi_min,q->rssi_max,
             q->gateway_avg,q->gateway_min,q->gateway_max,q->gateway_jitter,loss,
             q->internet_avg < 0 ? "non raggiunto " : "",q->internet_avg < 0 ? 0 : q->internet_avg,q->dns_ms);
}

static void quality(sentinel_ui_t *ui, lv_obj_t *page)
{
    ui_title(page,"Qualita e velocita");
    sn_quality_t q;
    if (!sn_quality(&q)) memset(&q,0,sizeof(q));
    char text[320];
    quality_text(&q,text,sizeof(text));
    ui->live = ui_text(page,text,false);
    ui->chart = lv_chart_create(page);
    lv_obj_set_size(ui->chart,LV_PCT(86),80);
    lv_chart_set_type(ui->chart,LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ui->chart,SN_HISTORY);
    lv_obj_set_style_bg_opa(ui->chart,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(ui->chart,0,0);
    lv_obj_set_style_size(ui->chart,0,0,LV_PART_INDICATOR);
    ui->series = lv_chart_add_series(ui->chart,ui_color(UI_COLOR_TEAL),LV_CHART_AXIS_PRIMARY_Y);
    ui->live2 = ui_text(page,q.verdict[0] ? q.verdict : "La misura dura 30 s: ping al router, connessione a 1.1.1.1:443 e DNS.",true);
    ui_button(page,"Misura qualita (30 s)",UI_COLOR_TEAL,action,(void *)18);
    ui_section(page,"Velocita via Internet");
    ui_text(page,q.speed_text[0] ? q.speed_text : "Nessun test eseguito.",false);
    ui_button(page,"Download 10 MB",UI_COLOR_BLUE,action,(void *)19);
    ui_button(page,"Upload 2 MB",UI_COLOR_INDIGO,action,(void *)20);
    ui_text(page,"Destinazione: speed.cloudflare.com (HTTPS). Usa i dati indicati e misura il collegamento dell'orologio, non la linea.",true);
    ui_button(page,"Arresta",UI_COLOR_ORANGE,action,(void *)4);
}

static void quality_update(sentinel_ui_t *ui)
{
    sn_quality_t q;
    if (!ui->live || !sn_quality(&q)) return;
    char text[320];
    quality_text(&q,text,sizeof(text));
    lv_label_set_text(ui->live,text);
    if (q.verdict[0]) lv_label_set_text(ui->live2,q.verdict);
    int top = 20;
    for (int i = 0; i < q.history; i++) if (q.rtt_history[i] > top) top = q.rtt_history[i];
    lv_chart_set_axis_range(ui->chart,LV_CHART_AXIS_PRIMARY_Y,0,top);
    for (int i = 0; i < SN_HISTORY; i++)
        lv_chart_set_series_value_by_id(ui->chart,ui->series,i,i < q.history && q.rtt_history[i] >= 0 ? q.rtt_history[i] : LV_CHART_POINT_NONE);
    lv_chart_refresh(ui->chart);
}

static void find(sentinel_ui_t *ui, lv_obj_t *page)
{
    sn_find_t f;
    if (!sn_find_state(&f)) memset(&f,0,sizeof(f));
    ui_title(page,f.name[0] ? f.name : "Trova");
    ui->live = ui_text(page,"In ascolto...",false);
    lv_obj_set_style_text_font(ui->live,ui_font_headline,0);
    ui->bar = lv_bar_create(page);
    lv_obj_set_size(ui->bar,LV_PCT(80),18);
    lv_bar_set_range(ui->bar,-95,-30);
    lv_obj_set_style_bg_color(ui->bar,ui_color(UI_COLOR_ORANGE),LV_PART_INDICATOR);
    ui->live2 = ui_text(page,"",true);
    ui_text(page,"Avvicinati al punto in cui il valore sale (es. da -80 a -45 dBm). Misura il segnale ricevuto, non una distanza: muri e corpo lo attenuano.",true);
    ui_button(page,"Ferma",UI_COLOR_ORANGE,action,(void *)4);
}

static void find_update(sentinel_ui_t *ui)
{
    sn_find_t f;
    if (!ui->live || !sn_find_state(&f)) return;
    char text[160];
    if (!f.frames) snprintf(text,sizeof(text),f.running ? "In ascolto..." : "Nessun segnale");
    else snprintf(text,sizeof(text),"%d dBm  %s",f.smooth,f.trend > 0 ? "piu caldo" : f.trend < 0 ? "piu freddo" : "stabile");
    lv_label_set_text(ui->live,text);
    lv_obj_set_style_text_color(ui->live,ui_color(f.trend > 0 ? UI_COLOR_ORANGE : f.trend < 0 ? UI_COLOR_BLUE : UI_COLOR_TEXT),0);
    lv_bar_set_value(ui->bar,f.frames ? f.smooth : -95,LV_ANIM_ON);
    if (f.frames) snprintf(text,sizeof(text),"Migliore %d dBm · %d trame · %s\n%s · %s",f.best,f.frames,
                           f.age_ms > 3000 ? "segnale perso" : "segnale attivo",f.ip,f.mac);
    else snprintf(text,sizeof(text),"%s · %s\n%s",f.ip,f.mac,
                  f.seconds > 8 ? "Nessuna trama: 5 GHz, via cavo o su un altro access point?" : "Invio ping per farlo trasmettere...");
    if (!f.running && f.seconds) snprintf(text+strlen(text),sizeof(text)-strlen(text),"\nRicerca terminata");
    lv_label_set_text(ui->live2,text);
}

static void render(sentinel_ui_t *ui)
{
    if (ui->content) lv_obj_delete(ui->content);
    ui->content = lv_obj_create(ui->screen);
    lv_obj_remove_style_all(ui->content);
    lv_obj_set_size(ui->content,LV_PCT(100),LV_PCT(100));
    lv_obj_set_scrollable(ui->content,false);
    lv_obj_move_to_index(ui->content,0);
    ui->headline = ui->caption = ui->sweep = ui->legend = ui->network_text = NULL;
    ui->live = ui->live2 = ui->bar = ui->chart = NULL;
    ui->series = NULL;
    memset(ui->dots,0,sizeof(ui->dots));
    if (ui->page == RADAR) { scene(ui); ui->rendered_revision = UINT32_MAX; return; }
    lv_obj_t *page = ui_page(ui->content,NULL);
    if (ui->page == MENU)
    {
        ui_title(page,"Sentinel");
        char network[200];
        snprintf(network,sizeof(network),"%s\nIP: %s\nGateway: %s · canale %d\n%s",ui->status.connected ? ui->status.ssid : "Wi-Fi non connesso",
            ui->status.ip[0] ? ui->status.ip : "non disponibile",ui->status.gateway,ui->status.channel,ui->status.security);
        ui->network_text = ui_text(page,network,true);
        ui->caption = ui_text(page,ui->status.message,true);
        ui_button(page,"Scansiona rete",UI_COLOR_TEAL,action,(void *)1);
        ui_button(page,"Arresta",UI_COLOR_ORANGE,action,(void *)4);
        char count[24]; snprintf(count,sizeof(count),"%d in questa rete",ui->status.here);
        ui_row(page,LV_SYMBOL_LIST,UI_COLOR_BLUE,"Dispositivi",count,action,(void *)2);
        char stored[24]; snprintf(stored,sizeof(stored),"%d archiviate",ui->status.networks);
        ui_row(page,LV_SYMBOL_DIRECTORY,UI_COLOR_INDIGO,"Altre reti",stored,action,(void *)10);
        ui_row(page,LV_SYMBOL_WIFI,UI_COLOR_GREEN,"Wi-Fi","Reti e canali",action,(void *)15);
        ui_row(page,LV_SYMBOL_CHARGE,UI_COLOR_ORANGE,"Qualita e velocita","Misura",action,(void *)17);
        ui_row(page,LV_SYMBOL_WIFI,UI_COLOR_TEAL,"Dashboard","PC",action,(void *)3);
        ui_row(page,LV_SYMBOL_SETTINGS,UI_COLOR_PURPLE,"Diagnostica","Risultati",action,(void *)5);
        char interval[24]; snprintf(interval,sizeof(interval),ui->status.interval ? "%lu s" : "Manuale",(unsigned long)ui->status.interval);
        ui_row(page,LV_SYMBOL_REFRESH,UI_COLOR_GREEN,"Monitoraggio",interval,action,(void *)7);
        ui_text(page,"IPv4: ARP, ICMP, mDNS, SSDP, nomi DNS e NetBIOS. Gli apparati isolati possono non essere visibili.",true);
        ui_button(page,"Torna al launcher",0x273A46,action,(void *)6);
        if (ui->status.dirty) ui_button(page,"Esci senza salvare",UI_COLOR_RED,leave_unsaved,NULL);
    }
    else if (ui->page == DEVICES) devices(ui,page);
    else if (ui->page == DASHBOARD)
    {
        ui_title(page,"Dashboard");
        char url[64]; snprintf(url,sizeof(url),"http://%s",ui->status.ip[0] ? ui->status.ip : "rete-non-connessa");
        ui_text(page,"Apri sul PC nella stessa rete:",true);
        ui_text(page,url,false);
        ui_section(page,"Codice di abbinamento");
        lv_obj_t *code = ui_text(page,ui->status.code,false);
        lv_obj_set_style_text_font(code,ui_font_headline,0);
        ui_text(page,"Connessione HTTP locale. Abbinamento non significa cifratura. Non aprire porte sul router.",true);
        ui_text(page,"Modifica nomi, categorie, note e fidati. Sfoglia le reti archiviate, misura qualita e velocita.",true);
        ui_text(page,"La dashboard e il monitoraggio restano attivi a schermo spento, finche Sentinel e in esecuzione.",true);
    }
    else if (ui->page == DEVICE) detail(ui,page);
    else if (ui->page == NETWORKS) networks(ui,page);
    else if (ui->page == WIFI) wifi(ui,page);
    else if (ui->page == QUALITY) { quality(ui,page); quality_update(ui); }
    else if (ui->page == FIND) { find(ui,page); find_update(ui); }
    else
    {
        ui_title(page,"Diagnostica");
        ui->caption = ui_text(page,ui->status.busy ? ui->status.message : "",true);
        ui_text(page,ui->status.diagnostic[0] ? ui->status.diagnostic : "Apri un dispositivo e scegli Identifica o Diagnostica.",false);
        ui_text(page,"Reti isolate, firewall e apparati dormienti limitano la scoperta. Una scansione non certifica la sicurezza.",true);
    }
}

static void tick(lv_timer_t *timer)
{
    sentinel_ui_t *ui = lv_timer_get_user_data(timer);
    sn_status_t status;
    if (!sn_status(&status)) return;
    bool changed = status.revision != ui->rendered_revision || status.busy != ui->status.busy;
    bool finished = ui->status.busy && !status.busy;
    ui->status = status;
    lv_obj_set_hidden(ui->border,!status.busy);
    if (status.busy) ui_border_progress_set(ui->border,status.percent,100);
    if (power_screen() != POWER_SCREEN_ON || lv_screen_active() != ui->screen) return;
    ui->rendered_revision = status.revision;
    if (ui->page == QUALITY) { quality_update(ui); if (finished) render(ui); return; }
    if (ui->page == FIND) { find_update(ui); return; }
    if (ui->page != RADAR)
    {
        if (ui->caption) lv_label_set_text(ui->caption,status.message);
        if (changed && ui->network_text)
        {
            char network[200];
            snprintf(network,sizeof(network),"%s\nIP: %s\nGateway: %s · canale %d\nDNS: %s\n%s",status.connected ? status.ssid : "Wi-Fi non connesso",
                status.ip[0] ? status.ip : "non disponibile",status.gateway,status.channel,status.dns,status.security);
            lv_label_set_text(ui->network_text,network);
        }
        /* Rebuild lists only when a job ends: never under the finger while scrolling. */
        if (finished && ui->page != MENU && ui->page != DASHBOARD) render(ui);
        return;
    }
    ui->angle = (ui->angle+4)%360;
    int w = board_info()->width, h = board_info()->height;
    int r = (w < h ? w : h)*43/100;
    ui->line[0] = (lv_point_precise_t){w/2,h*46/100};
    ui->line[1] = (lv_point_precise_t){w/2+lv_trigo_cos(ui->angle)*r/32768,h*46/100+lv_trigo_sin(ui->angle)*r/32768};
    lv_line_set_points(ui->sweep,ui->line,2);
    if (!changed) return;
    char count[16]; snprintf(count,sizeof(count),"%d",status.cycle ? status.observed : status.here);
    lv_label_set_text(ui->headline,count);
    lv_label_set_text(ui->legend,status.cycle ? status.busy ? "rilevati finora" : "rilevati nella scansione" : "dispositivi in questa rete");
    char caption[96];
    if (h >= 320) snprintf(caption,sizeof(caption),"%d in questa rete · IP %s\n%s",status.here,status.ip[0] ? status.ip : "non disponibile",status.connected ? status.ssid : "Wi-Fi assente");
    else snprintf(caption,sizeof(caption),"IP %s",status.ip[0] ? status.ip : "non disponibile");
    lv_label_set_text(ui->caption,caption);
    int shown = 0;
    sn_device_t device;
    for (int i = 0; i < status.devices && shown < 24; i++)
    {
        if (!sn_device_at(i,&device) || device.seen_cycle != status.cycle || !status.cycle ||
            strcmp(device.network,status.network) != 0) continue;
        uint32_t hash = 2166136261u;
        for (const char *p = device.id; *p; p++) hash = (hash ^ (unsigned char)*p)*16777619u;
        int angle = hash%360, radius = r*(35+(hash>>10)%60)/100;
        lv_obj_set_pos(ui->dots[shown],w/2+lv_trigo_cos(angle)*radius/32768-3,h*46/100+lv_trigo_sin(angle)*radius/32768-3);
        lv_obj_set_style_bg_color(ui->dots[shown],ui_color(sn_confirmed(&device) ? UI_COLOR_TEAL : UI_COLOR_ORANGE),0);
        lv_obj_set_hidden(ui->dots[shown++],false);
    }
    for (; shown < 24; shown++) lv_obj_set_hidden(ui->dots[shown],true);
}

bool sn_ui_back(void)
{
    sentinel_ui_t *ui = s_ui;
    if (!ui) return true;
    switch (ui->page)
    {
        case RADAR:
            if (ui->status.busy) { sn_cancel(); ui_toast("Attendi l'arresto, poi premi indietro"); }
            else result(sn_exit(false));
            return true;
        case FIND: sn_cancel(); go(ui,DEVICE); break;
        case DEVICE: ui->page = DEVICES; render(ui); break; /* keep the list page */
        case DEVICES: go(ui,ui->filter[0] && !current_network(ui) ? NETWORKS : MENU); break;
        case MENU: go(ui,RADAR); break;
        default: go(ui,MENU); break;
    }
    return true;
}

void sn_ui_create(lv_obj_t *screen)
{
    sentinel_ui_t *ui = calloc(1,sizeof(*ui));
    ESP_ERROR_CHECK(ui == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_ui = ui; ui->screen = screen;
    sn_status(&ui->status);
    ui->border = ui_border_progress_create(screen,UI_COLOR_TEAL);
    render(ui);
    ui->timer = lv_timer_create(tick,100,ui);
    ESP_ERROR_CHECK(ui->timer == NULL ? ESP_ERR_NO_MEM : ESP_OK);
}
