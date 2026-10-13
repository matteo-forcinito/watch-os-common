/* Fullscreen radar entrance. Points are observations, never invented hosts. */
#include "sentinel.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "ui/ui.h"
#include "hardware/board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    lv_obj_t *screen, *content, *border, *headline, *caption, *sweep;
    lv_obj_t *dots[24];
    lv_point_precise_t line[2];
    lv_timer_t *timer;
    int page, offset, selected, angle, field;
    uint32_t rendered_revision;
    bool all_networks;
    sn_status_t status;
} sentinel_ui_t;
static sentinel_ui_t *s_ui;
static void render(sentinel_ui_t *ui);

static void result(esp_err_t error)
{
    if (error == ESP_OK) ui_toast("Operazione avviata");
    else ui_toast(error == ESP_ERR_INVALID_STATE ? "Attendi o arresta la scansione" : "Dati non validi o memoria occupata");
}

static void action(lv_event_t *event)
{
    sentinel_ui_t *ui = s_ui;
    int action_id = (int)(uintptr_t)lv_event_get_user_data(event);
    if (action_id == 1) result(sn_scan(""));
    else if (action_id == 2) { ui->page = 2; ui->offset = 0; render(ui); }
    else if (action_id == 3) { ui->page = 3; render(ui); }
    else if (action_id == 4) { sn_cancel(); ui_toast("Arresto richiesto"); }
    else if (action_id == 5) { ui->page = 5; render(ui); }
    else if (action_id == 6) result(sn_exit(false));
    else if (action_id == 7) result(sn_settings(ui->status.interval == 0 ? 60 : ui->status.interval == 60 ? 300 : 0));
    else if (action_id == 8) { ui->offset = ui->offset >= 8 ? ui->offset-8 : 0; render(ui); }
    else if (action_id == 9) { ui->offset += 8; render(ui); }
    else if (action_id == 10) { ui->all_networks = !ui->all_networks; ui->offset = 0; render(ui); }
    else if (action_id == 11)
    {
        sn_device_t device;
        if (sn_device_at(ui->selected,&device)) { result(sn_diagnose(device.ip)); ui->page = 5; render(ui); }
    }
    else if (action_id == 12)
    {
        sn_device_t device;
        if (sn_device_at(ui->selected,&device)) { device.trusted = !device.trusted; result(sn_edit(&device)); }
    }
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
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    { s_ui->page = 1; render(s_ui); }
    else if (lv_event_get_code(event) == LV_EVENT_GESTURE)
    {
        lv_indev_t *input = lv_indev_active();
        if (input && lv_indev_get_gesture_dir(input) == LV_DIR_TOP)
        { lv_indev_wait_release(input); s_ui->page = 1; render(s_ui); }
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
    ui->headline = text_at(ui->content,"0",center_y-(h >= 320 ? 72 : 36),h >= 320 ? ui_font_clock : ui_font_big,0xE9FFF8);
    text_at(ui->content,"rilevati nella scansione",center_y+(h >= 320 ? 76 : 38),UI_FONT_SMALL,0x90C7BC);
    ui->caption = text_at(ui->content,"La tua rete, osservata da vicino",h*76/100,UI_FONT_SMALL,0x92AEB5);
    text_at(ui->content,"Tocca o scorri per esplorare",h*86/100,UI_FONT_SMALL,0x4DE2BB);
    lv_obj_set_clickable(ui->content,true);
    lv_obj_set_gesture_bubble(ui->content,false);
    lv_obj_add_event_cb(ui->content,enter,LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(ui->content,enter,LV_EVENT_GESTURE,NULL);
    lv_port_set_focusable(ui->content);
}

static void select_device(lv_event_t *event)
{
    s_ui->selected = (int)(uintptr_t)lv_event_get_user_data(event)-1;
    s_ui->page = 4;
    render(s_ui);
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

static void detail(sentinel_ui_t *ui, lv_obj_t *page)
{
    sn_device_t d;
    if (!sn_device_at(ui->selected,&d)) { ui_text(page,"Dispositivo non disponibile",true); return; }
    ui_title(page,d.label[0] ? d.label : d.hostname[0] ? d.hostname : d.ip);
    char text[256];
    snprintf(text,sizeof(text),"%s\n%s\n%s\n%s",d.ip,d.mac[0] ? d.mac : "MAC non rilevato",
        d.seen_cycle == ui->status.cycle && ui->status.cycle ? sn_confirmed(&d) ? "Rilevato nella scansione" : "Risposta proxy ARP" : "Non rilevato ora",
        d.services[0] ? d.services : "Servizi non annunciati");
    ui_text(page,text,true);
    ui_row(page,LV_SYMBOL_EDIT,UI_COLOR_TEAL,"Nome",d.label[0] ? d.label : "Assegna",edit,(void *)0);
    ui_row(page,LV_SYMBOL_LIST,UI_COLOR_BLUE,"Categoria",d.category[0] ? d.category : "Assegna",edit,(void *)1);
    ui_row(page,LV_SYMBOL_EDIT,UI_COLOR_GRAY,"Note",d.note[0] ? "Modifica" : "Aggiungi",edit,(void *)2);
    ui_row(page,LV_SYMBOL_OK,UI_COLOR_GREEN,"Fidato",d.trusted ? "Si" : "No",action,(void *)12);
    ui_button(page,"Diagnostica",UI_COLOR_TEAL,action,(void *)11);
    ui_text(page,"Non rilevato non significa scollegato. IP e MAC possono cambiare.",true);
}

static void render(sentinel_ui_t *ui)
{
    if (ui->content) lv_obj_delete(ui->content);
    ui->content = lv_obj_create(ui->screen);
    lv_obj_remove_style_all(ui->content);
    lv_obj_set_size(ui->content,LV_PCT(100),LV_PCT(100));
    lv_obj_set_scrollable(ui->content,false);
    lv_obj_move_to_index(ui->content,0);
    ui->headline = ui->caption = ui->sweep = NULL;
    memset(ui->dots,0,sizeof(ui->dots));
    if (ui->page == 0) { scene(ui); return; }
    lv_obj_t *page = ui_page(ui->content,NULL);
    if (ui->page == 1)
    {
        ui_title(page,"Sentinel");
        ui_text(page,ui->status.connected ? ui->status.ssid : "Wi-Fi non connesso",true);
        ui->caption = ui_text(page,ui->status.message,true);
        ui_button(page,"Scansiona rete",UI_COLOR_TEAL,action,(void *)1);
        ui_button(page,"Arresta scansione",UI_COLOR_ORANGE,action,(void *)4);
        char count[24]; snprintf(count,sizeof(count),"%d noti",ui->status.devices);
        ui_row(page,LV_SYMBOL_LIST,UI_COLOR_BLUE,"Inventario",count,action,(void *)2);
        ui_row(page,LV_SYMBOL_WIFI,UI_COLOR_TEAL,"Dashboard","PC",action,(void *)3);
        ui_row(page,LV_SYMBOL_SETTINGS,UI_COLOR_PURPLE,"Diagnostica","Risultati",action,(void *)5);
        char interval[24]; snprintf(interval,sizeof(interval),ui->status.interval ? "%lu s" : "Manuale",(unsigned long)ui->status.interval);
        ui_row(page,LV_SYMBOL_REFRESH,UI_COLOR_GREEN,"Monitoraggio",interval,action,(void *)7);
        ui_text(page,"IPv4: ARP, ICMP, mDNS, SSDP. Gli apparati isolati possono non essere visibili.",true);
        ui_button(page,"Torna al launcher",0x273A46,action,(void *)6);
        if (ui->status.dirty) ui_button(page,"Esci senza salvare",UI_COLOR_RED,leave_unsaved,NULL);
    }
    else if (ui->page == 2)
    {
        ui_title(page,"Inventario");
        ui_row(page,LV_SYMBOL_WIFI,UI_COLOR_TEAL,"Reti",ui->all_networks ? "Tutte" : "Attuale",action,(void *)10);
        int matched = 0, shown = 0;
        sn_device_t d;
        for (int i = 0; i < ui->status.devices; i++)
        {
            if (!sn_device_at(i,&d)) continue;
            if (!ui->all_networks && ui->status.network[0] && strcmp(d.network,ui->status.network) != 0) continue;
            if (matched++ < ui->offset || shown >= 8) continue;
            char value[40]; snprintf(value,sizeof(value),"%s%s",d.ip,d.trusted ? "  OK" : "");
            ui_row(page,LV_SYMBOL_WIFI,d.seen_cycle == ui->status.cycle && ui->status.cycle ? UI_COLOR_TEAL : UI_COLOR_GRAY,
                d.label[0] ? d.label : d.hostname[0] ? d.hostname : "Dispositivo",value,select_device,(void *)(uintptr_t)(i+1));
            shown++;
        }
        if (!matched) ui_text(page,"Nessun dispositivo. Avvia una scansione dalla pagina principale.",true);
        if (ui->offset) ui_button(page,"Precedenti",UI_COLOR_GRAY,action,(void *)8);
        if (matched > ui->offset+8) ui_button(page,"Successivi",UI_COLOR_GRAY,action,(void *)9);
    }
    else if (ui->page == 3)
    {
        ui_title(page,"Dashboard");
        char url[64]; snprintf(url,sizeof(url),"http://%s",ui->status.ip[0] ? ui->status.ip : "rete-non-connessa");
        ui_text(page,"Apri sul PC nella stessa rete:",true);
        ui_text(page,url,false);
        ui_section(page,"Codice di abbinamento");
        lv_obj_t *code = ui_text(page,ui->status.code,false);
        lv_obj_set_style_text_font(code,ui_font_headline,0);
        ui_text(page,"Connessione HTTP locale. Abbinamento non significa cifratura. Non aprire porte sul router.",true);
        ui_text(page,"Modifica nomi, categorie, note e fidati. Cerca in tutte le reti ed esporta/importa l'archivio.",true);
        ui_text(page,"La dashboard e il monitoraggio restano attivi a schermo spento, finche Sentinel e in esecuzione.",true);
    }
    else if (ui->page == 4) detail(ui,page);
    else
    {
        ui_title(page,"Diagnostica");
        ui_text(page,ui->status.diagnostic[0] ? ui->status.diagnostic : "Apri un dispositivo nell'inventario e scegli Diagnostica.",false);
        ui_text(page,"Reti isolate, firewall e apparati dormienti limitano la scoperta. Una scansione non certifica la sicurezza.",true);
    }
}

static void tick(lv_timer_t *timer)
{
    sentinel_ui_t *ui = lv_timer_get_user_data(timer);
    sn_status_t status;
    if (!sn_status(&status)) return;
    bool changed = status.revision != ui->rendered_revision || status.busy != ui->status.busy;
    ui->status = status;
    lv_obj_set_hidden(ui->border,!status.busy);
    if (status.busy) ui_border_progress_set(ui->border,status.percent,100);
    if (power_screen() != POWER_SCREEN_ON || lv_screen_active() != ui->screen) return;
    ui->rendered_revision = status.revision;
    if (ui->page != 0)
    {
        if (ui->caption) lv_label_set_text(ui->caption,status.message);
        if (changed && !status.busy && (ui->page == 4 || ui->page == 5)) render(ui);
        return;
    }
    ui->angle = (ui->angle+4)%360;
    int w = board_info()->width, h = board_info()->height;
    int r = (w < h ? w : h)*43/100;
    ui->line[0] = (lv_point_precise_t){w/2,h*46/100};
    ui->line[1] = (lv_point_precise_t){w/2+lv_trigo_cos(ui->angle)*r/32768,h*46/100+lv_trigo_sin(ui->angle)*r/32768};
    lv_line_set_points(ui->sweep,ui->line,2);
    if (!changed) return;
    char count[16]; snprintf(count,sizeof(count),"%d",status.observed);
    lv_label_set_text(ui->headline,count);
    char caption[96]; snprintf(caption,sizeof(caption),"%d in archivio  ·  %s",status.devices,status.connected ? status.ssid : "Wi-Fi assente");
    lv_label_set_text(ui->caption,caption);
    int shown = 0;
    sn_device_t device;
    for (int i = 0; i < status.devices && shown < 24; i++)
    {
        if (!sn_device_at(i,&device) || device.seen_cycle != status.cycle || !status.cycle) continue;
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
    if (!s_ui) return true;
    if (s_ui->page == 0)
    {
        if (s_ui->status.busy) { sn_cancel(); ui_toast("Attendi l'arresto, poi premi indietro"); }
        else result(sn_exit(false));
    }
    else { s_ui->page = s_ui->page == 4 ? 2 : s_ui->page == 1 ? 0 : 1; render(s_ui); }
    return true;
}

void sn_ui_create(lv_obj_t *screen)
{
    sentinel_ui_t *ui = calloc(1,sizeof(*ui));
    ESP_ERROR_CHECK(ui == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_ui = ui; ui->screen = screen; ui->all_networks = true;
    sn_status(&ui->status);
    ui->border = ui_border_progress_create(screen,UI_COLOR_TEAL);
    render(ui);
    ui->timer = lv_timer_create(tick,100,ui);
    ESP_ERROR_CHECK(ui->timer == NULL ? ESP_ERR_NO_MEM : ESP_OK);
}
