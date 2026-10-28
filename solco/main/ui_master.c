/*
 * MASTER sheet (BOOT click, or the VOL key in Studio): everything that is
 * not performance lives here, so the decks keep the whole screen.
 *
 *     MASTER //                       BOOT: CHIUDI
 *                     42
 *                  VOLUME %
 *              ▮▮▮▮▮▮▮▯▯▯▯▯             master peak
 *     |=========[ | ]------------|     relative drag, a tap never jumps
 *     [   -   ][  MUTE  ][   +   ]     hold - / + to repeat
 *     USCITA AUDIO //
 *     [ TUTTI ][ SOLO HUB ][ SEGUI MIX ]
 *     SOLO L'HUB // OFFLINE
 *        [ STUDIO ][ ESCI (TIENI) ]    exit needs a 1 s hold
 *
 * Opaque sheet (no partial opacity), slides in once, then only the number,
 * the meter and the fader thumb change.
 */
#include "core/lv_port.h"
#include "core/sys.h"
#include "engine.h"
#include "link.h"
#include "nvs.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "solco_ui.h"
#include "ui/ui.h"
#include "ui_kit.h"
#include <stdio.h>
#include <string.h>

typedef struct
{
    int x, y, w, h;
} rc_t;

static lv_obj_t *s_sheet, *s_value, *s_caption, *s_note, *s_root;
static sk_meter_t *s_meter;
static sk_fader_t *s_fader;
static sk_strip_t *s_steps, *s_out, *s_actions;
static lv_timer_t *s_hold;
static uint32_t s_hold_start;
static int s_hold_cell = -1, s_hold_strip;
static int s_restore = 10;
static int s_output = SOLCO_OUT_FOLLOW; /* user preset: the device that mixes plays */
static int s_shown_volume = -1;
static bool s_open;

static const char *k_out_text[3] = {"TUTTI", "SOLO HUB", "SEGUI MIX"};
static const char *k_out_note[3] = {"TUTTI I DEVICE DEL KIT", "SOLO L'HUB, CONSIGLIATO LIVE",
                                    "SUONA CHI MIXA PER ULTIMO"};

int solco_output_mode(void) { return s_output; }
bool solco_master_open(void) { return s_open; }

static int volume_now(void)
{
    solco_status_t st;
    solco_status(&st);
    return st.volume;
}

static void step(int delta, const char *source) { solco_ui_set_volume(volume_now() + delta, source); }

static void hold_tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t held = lv_tick_get() - s_hold_start;
    if (s_hold_strip == 1)
    {
        /* ESCI: fill the cell, leave after SK_HOLD_MS. */
        sk_strip_progress(s_actions, 2, (int)(held * 255 / SK_HOLD_MS));
        if (held >= SK_HOLD_MS)
        {
            lv_timer_delete(s_hold);
            s_hold = NULL;
            solco_ui_set_volume(0, "exit");
            extapp_return_to_launcher();
        }
    }
    else if (held > 400)
        step(s_hold_cell == 0 ? -1 : 1, "repeat"); /* auto-repeat of - / + */
}

static void hold_start(int strip, int cell)
{
    s_hold_strip = strip;
    s_hold_cell = cell;
    s_hold_start = lv_tick_get();
    if (!s_hold)
        s_hold = lv_timer_create(hold_tick, SK_TICK_MS * 2, NULL);
}

static void hold_stop(void)
{
    if (s_hold)
        lv_timer_delete(s_hold);
    s_hold = NULL;
    s_hold_cell = -1;
    sk_strip_progress(s_actions, 2, 0);
}

static void steps_cb(sk_strip_t *strip, int cell, bool pressed)
{
    (void)strip;
    if (!pressed)
    {
        hold_stop();
        return;
    }
    if (cell == 1)
    {
        int volume = volume_now();
        if (volume > 0)
        {
            s_restore = volume;
            solco_ui_set_volume(0, "mute");
        }
        else
            solco_ui_set_volume(s_restore, "unmute");
        return;
    }
    step(cell == 0 ? -1 : 1, "step");
    hold_start(0, cell);
}

static void out_cb(sk_strip_t *strip, int cell, bool pressed)
{
    (void)strip;
    if (!pressed)
        return;
    s_output = cell; /* applied by the hub once the kit link exists (F5) */
    s_shown_volume = -1;
    solco_master_refresh();
}

static void kit_show(bool show);

static void actions_cb(sk_strip_t *strip, int cell, bool pressed)
{
    (void)strip;
    if (cell == 1)
    {
        if (!pressed)
            kit_show(true);
        return;
    }
    if (cell == 0)
    {
        if (!pressed)
        {
            solco_master_close();
            solco_ui_show(SOLCO_VIEW_STUDIO, 0);
        }
        return;
    }
    if (pressed)
        hold_start(1, cell);
    else
        hold_stop();
}

static void fader_cb(sk_fader_t *fader, int value)
{
    (void)fader;
    solco_ui_set_volume(value, "slider");
}

static rc_t place(int x, int y, int w, int h, bool landscape, int sw, int sh)
{
    int bw = landscape ? 448 : 466, bh = landscape ? 368 : 466;
    return (rc_t){x * sw / bw, y * sh / bh, w * sw / bw, h * sh / bh};
}

void solco_master_create(lv_obj_t *root)
{
    int sw = lv_obj_get_width(root), sh = lv_obj_get_height(root);
    bool round = board_info()->round, landscape = !round && sw > sh;
#define P(x, y, w, h, rx, ry, rw, rh) (landscape ? place(x, y, w, h, true, sw, sh) : place(rx, ry, rw, rh, false, sw, sh))
    s_sheet = sk_panel(root, 0, 0, sw, sh, true);
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE); /* the sheet owns every touch while open */
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_text_align_t center = LV_TEXT_ALIGN_CENTER;
    rc_t r = P(40, 12, 200, 18, 113, 38, 240, 18);
    sk_hud(s_sheet, "MASTER //", TK_CYAN, r.x, r.y, r.w, landscape ? LV_TEXT_ALIGN_LEFT : center, false);
    r = P(240, 13, 168, 16, 133, 58, 200, 16);
    sk_hud(s_sheet, "BOOT: CHIUDI", TK_TEXT_3, r.x, r.y, r.w, landscape ? LV_TEXT_ALIGN_RIGHT : center, true);
    r = P(0, 26, 448, 72, 0, 74, 466, 72);
    s_value = sk_label(s_sheet, ui_font_big, TK_TEXT, r.x, r.y, r.w, center);
    r = P(124, 96, 200, 16, 133, 144, 200, 16);
    s_caption = sk_hud(s_sheet, "VOLUME %", TK_TEXT_2, r.x, r.y, r.w, center, true);
    r = P(124, 116, 200, 6, 153, 164, 160, 6);
    s_meter = sk_meter_create(s_sheet, r.x, r.y, r.w, r.h);
    r = P(40, 128, 368, 44, 40, 176, 386, 44);
    s_fader = sk_fader_create(s_sheet, r.x, r.y, r.w, r.h, SK_FADER_LEVEL, 0, 100, 0, TK_CYAN, fader_cb, 0);
    r = P(40, 178, 368, 44, 40, 226, 386, 44);
    static const uint8_t steps_w[3] = {1, 2, 1};
    s_steps = sk_strip_create(s_sheet, r.x, r.y, r.w, r.h, 3, steps_w, steps_cb, 0);
    s_steps->font = &lv_font_montserrat_20;
    sk_strip_set(s_steps, 0, LV_SYMBOL_MINUS, TK_TEXT, SK_IDLE);
    sk_strip_set(s_steps, 2, LV_SYMBOL_PLUS, TK_TEXT, SK_IDLE);
    r = P(40, 230, 368, 16, 83, 278, 300, 16);
    sk_hud(s_sheet, "USCITA AUDIO //", TK_CYAN, r.x, r.y, r.w, landscape ? LV_TEXT_ALIGN_LEFT : center, true);
    r = P(40, 250, 368, 44, 32, 298, 402, 44);
    s_out = sk_strip_create(s_sheet, r.x, r.y, r.w, r.h, 3, NULL, out_cb, 0);
    s_out->font = &lv_font_montserrat_14;
    r = P(40, 297, 368, 16, 33, 346, 400, 16);
    s_note = sk_hud(s_sheet, "", TK_TEXT_3, r.x, r.y, r.w, landscape ? LV_TEXT_ALIGN_LEFT : center, true);
    r = P(56, 318, 336, 44, 88, 368, 290, 44);
    static const uint8_t actions_w[3] = {28, 28, 44};
    s_actions = sk_strip_create(s_sheet, r.x, r.y, r.w, r.h, 3, actions_w, actions_cb, 0);
    s_actions->font = &lv_font_montserrat_14;
    sk_strip_set(s_actions, 0, "STUDIO", TK_VIOLET, SK_ARMED);
    sk_strip_set(s_actions, 1, "KIT", TK_CYAN, SK_ARMED);
    sk_strip_set(s_actions, 2, "ESCI (TIENI)", TK_RED, SK_IDLE);
    s_root = root;
#undef P
    lv_obj_set_hidden(s_sheet, true);
}

/* ------------------------------------------------------------------ KIT
 * Link the devices from the device: CREA KIT (this one is the hub), UNISCITI
 * (member: finds the hub on the kit channel), SCOLLEGA. Start/stop run on the
 * system worker (Wi-Fi bring-up must not stall the UI); the last role is kept
 * in Solco's own NVS namespace, written only on these explicit actions. */
#define KIT_CHANNEL 6
#define KIT_SEARCH_MS 15000

static lv_obj_t *s_kit, *k_role, *k_line[4], *k_note;
static sk_strip_t *k_actions, *k_back;
static volatile bool s_kit_busy;
static volatile esp_err_t s_kit_err = ESP_OK;
static int s_kit_last = -1; /* remembered role, -1 unknown */
static uint32_t s_kit_since;
static bool s_kit_stream;
static solco_link_stats_t s_link;

static void kit_job(void *arg)
{
    int role = (int)(intptr_t)arg;
    solco_link_stop();
    s_kit_err = role ? solco_link_start((solco_link_role_t)role, KIT_CHANNEL, false) : ESP_OK;
    nvs_handle_t h;
    if (nvs_open("solco", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_u8(h, "kit_role", (uint8_t)role);
        nvs_commit(h);
        nvs_close(h);
    }
    s_kit_last = role;
    s_kit_busy = false;
}

static void kit_request(int role)
{
    if (s_kit_busy)
        return;
    s_kit_busy = true;
    s_kit_stream = false;
    s_kit_since = lv_tick_get();
    if (!sys_post(kit_job, (void *)(intptr_t)role))
        s_kit_busy = false;
}

const char *solco_kit_tag(void)
{
    if (s_link.role == SOLCO_LINK_HUB)
        return "KIT HUB";
    if (s_link.role == SOLCO_LINK_MEMBER)
        return s_link.joined ? "KIT MEMBRO" : "CERCO HUB";
    return "";
}

void solco_kit_tick(void)
{
    static unsigned n;
    if (++n % 5)
        return; /* 5 Hz is plenty for link state */
    solco_link_stats(&s_link);
    /* Hub: stream as soon as a member joined; the routing preset decides who plays. */
    if (s_link.role == SOLCO_LINK_HUB && s_link.joins > 0 && !s_kit_stream)
    {
        solco_link_console("stream 1");
        s_kit_stream = true;
    }
}

static void kit_actions_cb(sk_strip_t *strip, int cell, bool pressed)
{
    (void)strip;
    if (!pressed)
        return;
    kit_request(cell == 0 ? SOLCO_LINK_HUB : cell == 1 ? SOLCO_LINK_MEMBER : SOLCO_LINK_OFF);
}

static void kit_back_cb(sk_strip_t *strip, int cell, bool pressed)
{
    (void)strip;
    (void)cell;
    if (!pressed)
        kit_show(false);
}

static void kit_create(void)
{
    int sw = lv_obj_get_width(s_root), sh = lv_obj_get_height(s_root);
    bool round = board_info()->round, landscape = !round && sw > sh;
#define P(x, y, w, h, rx, ry, rw, rh) (landscape ? place(x, y, w, h, true, sw, sh) : place(rx, ry, rw, rh, false, sw, sh))
    lv_text_align_t al = landscape ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER;
    s_kit = sk_panel(s_root, 0, 0, sw, sh, true);
    lv_obj_add_flag(s_kit, LV_OBJ_FLAG_CLICKABLE);
    rc_t r = P(40, 12, 368, 18, 113, 38, 240, 18);
    sk_hud(s_kit, "KIT // COLLEGA I DEVICE", TK_CYAN, r.x, r.y, r.w, al, false);
    r = P(40, 36, 368, 40, 0, 62, 466, 40);
    k_role = sk_label(s_kit, &lv_font_montserrat_32, TK_TEXT_3, r.x, r.y, r.w, landscape ? al : LV_TEXT_ALIGN_CENTER);
    for (int i = 0; i < 4; ++i)
    {
        r = P(40, 84 + i * 20, 368, 16, 63, 110 + i * 20, 340, 16);
        k_line[i] = sk_hud(s_kit, "", TK_TEXT_2, r.x, r.y, r.w, al, true);
    }
    r = P(40, 172, 368, 16, 63, 194, 340, 16);
    k_note = sk_hud(s_kit, "", TK_TEXT_3, r.x, r.y, r.w, al, true);
    r = P(40, 196, 368, 52, 32, 222, 402, 52);
    k_actions = sk_strip_create(s_kit, r.x, r.y, r.w, r.h, 3, NULL, kit_actions_cb, 0);
    k_actions->font = &lv_font_montserrat_14;
    r = P(124, 312, 200, 44, 133, 360, 200, 44);
    k_back = sk_strip_create(s_kit, r.x, r.y, r.w, r.h, 1, NULL, kit_back_cb, 0);
    k_back->font = &lv_font_montserrat_14;
    sk_strip_set(k_back, 0, LV_SYMBOL_LEFT " MASTER", TK_TEXT, SK_IDLE);
#undef P
    nvs_handle_t h;
    uint8_t role;
    if (s_kit_last < 0 && nvs_open("solco", NVS_READONLY, &h) == ESP_OK)
    {
        if (nvs_get_u8(h, "kit_role", &role) == ESP_OK)
            s_kit_last = role;
        nvs_close(h);
    }
}

static void kit_show(bool show)
{
    if (show && !s_kit)
        kit_create();
    if (!s_kit)
        return;
    if (show)
        lv_obj_move_foreground(s_kit);
    lv_obj_set_hidden(s_kit, !show);
}

static void kit_refresh(void)
{
    if (!s_kit || lv_obj_is_hidden(s_kit))
        return;
    solco_link_stats(&s_link);
    const solco_link_stats_t *l = &s_link;
    char t[80];
    const char *role = "OFFLINE";
    uint32_t color = TK_TEXT_3;
    bool searching = l->role == SOLCO_LINK_MEMBER && !l->joined;
    if (s_kit_busy)
        role = "AVVIO...", color = TK_AMBER;
    else if (l->role == SOLCO_LINK_HUB)
        role = "HUB", color = TK_CYAN;
    else if (searching)
    {
        bool late = lv_tick_get() - s_kit_since > KIT_SEARCH_MS;
        role = late ? "HUB NON TROVATO" : "CERCO HUB...", color = late ? TK_RED : TK_AMBER;
    }
    else if (l->role == SOLCO_LINK_MEMBER)
        role = "MEMBRO", color = TK_MAGENTA;
    sk_set_text(k_role, role);
    sk_set_color(k_role, color);
    if (l->role == SOLCO_LINK_HUB)
        snprintf(t, sizeof(t), "MEMBRI ENTRATI %lu // ACCOPPIATI %d // CANALE %d", (unsigned long)l->joins,
                 l->peers_paired, l->channel);
    else if (l->role == SOLCO_LINK_MEMBER)
        snprintf(t, sizeof(t), "HUB %02X:%02X // ACCOPPIATI %d // CANALE %d", l->hub_mac[4], l->hub_mac[5],
                 l->peers_paired, l->channel);
    else
        snprintf(t, sizeof(t), "NESSUN COLLEGAMENTO // CANALE %d", KIT_CHANNEL);
    sk_set_text(k_line[0], t);
    bool member = l->role == SOLCO_LINK_MEMBER;
    if (member)
        snprintf(t, sizeof(t), l->converged ? "SYNC OK // RESIDUO %lu US" : "SYNC IN CORSO (AUDIO MUTO)",
                 (unsigned long)l->residual_us);
    else
        snprintf(t, sizeof(t), l->role ? "CLOCK DEL KIT: QUESTO DEVICE" : "SYNC --");
    sk_set_text(k_line[1], t);
    sk_set_color(k_line[1], member ? (l->converged ? TK_LIME : TK_AMBER) : TK_TEXT_2);
    snprintf(t, sizeof(t), l->streaming ? "STREAM ON // RITARDO %lu MS" : "STREAM OFF",
             (unsigned long)(l->d_us / 1000));
    sk_set_text(k_line[2], t);
    sk_set_color(k_line[2], l->streaming ? TK_LIME : TK_TEXT_3);
    uint32_t seen = l->beacons + l->beacon_lost;
    int quality = member ? (seen ? (int)(l->beacons * 100 / seen) : 0) : (l->tx ? (int)(100 - l->tx_errors * 100 / l->tx) : 100);
    if (l->role)
        snprintf(t, sizeof(t), "LINK %d%% // PERSI %lu // RTT %lu.%lu MS", quality,
                 (unsigned long)(member ? l->beacon_lost + l->aud_lost_blocks : l->tx_errors),
                 (unsigned long)(l->rtt_last_us / 1000), (unsigned long)(l->rtt_last_us % 1000 / 100));
    else
        snprintf(t, sizeof(t), "LINK --");
    sk_set_text(k_line[3], t);
    sk_set_color(k_line[3], !l->role ? TK_TEXT_3 : quality >= 98 ? TK_LIME : quality >= 90 ? TK_AMBER : TK_RED);
    if (s_kit_err != ESP_OK)
        snprintf(t, sizeof(t), "ERRORE AVVIO: %s", esp_err_to_name(s_kit_err));
    else if (l->role && l->peers_paired == 0)
        snprintf(t, sizeof(t), "NESSUN DEVICE ACCOPPIATO IN ECOS");
    else if (searching && lv_tick_get() - s_kit_since > KIT_SEARCH_MS)
        snprintf(t, sizeof(t), "ACCENDI L'HUB (CREA KIT) E RESTA VICINO");
    else
        snprintf(t, sizeof(t), "%s", s_kit_last == 1 ? "ULTIMO RUOLO: HUB" : s_kit_last == 2 ? "ULTIMO RUOLO: MEMBRO" : "");
    sk_set_text(k_note, t);
    sk_set_color(k_note, s_kit_err != ESP_OK || (l->role && !l->peers_paired) ? TK_RED : TK_TEXT_3);
    int r = l->role;
    sk_strip_set(k_actions, 0, "CREA KIT", TK_CYAN,
                 r == SOLCO_LINK_HUB ? SK_ON : r ? SK_OFF : s_kit_last == 1 ? SK_ARMED : SK_IDLE);
    sk_strip_set(k_actions, 1, "UNISCITI", TK_MAGENTA,
                 r == SOLCO_LINK_MEMBER ? SK_ON : r ? SK_OFF : s_kit_last == 2 ? SK_ARMED : SK_IDLE);
    sk_strip_set(k_actions, 2, "SCOLLEGA", TK_RED, r ? SK_IDLE : SK_OFF);
    sk_strip_tick(k_actions);
}

void solco_master_refresh(void)
{
    if (!s_open)
        return;
    solco_status_t st;
    solco_status(&st);
    if (st.volume != s_shown_volume)
    {
        s_shown_volume = st.volume;
        lv_label_set_text_fmt(s_value, "%d", st.volume);
        sk_set_color(s_value, st.volume ? TK_TEXT : TK_AMBER);
        sk_set_text(s_caption, st.volume ? "VOLUME %" : "MUTO // TOCCA RIATTIVA");
        sk_set_color(s_caption, st.volume ? TK_TEXT_2 : TK_AMBER);
        sk_strip_set(s_steps, 1, st.volume ? "MUTE" : "RIATTIVA", st.volume ? TK_TEXT : TK_AMBER,
                     st.volume ? SK_IDLE : SK_ARMED);
        for (int i = 0; i < 3; ++i)
            sk_strip_set(s_out, i, k_out_text[i], TK_CYAN, i == s_output ? SK_ON : SK_IDLE);
        char note[64];
        const char *tag = solco_kit_tag();
        snprintf(note, sizeof(note), "%s // %s", k_out_note[s_output], tag[0] ? tag : "OFFLINE");
        sk_set_text(s_note, note);
    }
    sk_fader_set(s_fader, st.volume);
    sk_meter_set(s_meter, st.peak);
    kit_refresh();
}

static void slide(void *obj, int32_t value) { lv_obj_set_y(obj, value); }

void solco_master_toggle(void)
{
    if (s_open)
    {
        solco_master_close();
        return;
    }
    s_open = true;
    s_shown_volume = -1;
    lv_obj_move_foreground(s_sheet);
    lv_obj_set_hidden(s_sheet, false);
    solco_master_refresh();
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_sheet);
    lv_anim_set_exec_cb(&a, slide);
    lv_anim_set_values(&a, -lv_obj_get_height(s_sheet) / 3, 0);
    lv_anim_set_duration(&a, TK_T_PAGE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void solco_master_close(void)
{
    if (!s_open)
        return;
    hold_stop();
    kit_show(false);
    lv_anim_delete(s_sheet, slide);
    lv_obj_set_y(s_sheet, 0);
    lv_obj_set_hidden(s_sheet, true);
    s_open = false;
}

void solco_master_knob(int delta) { step(delta, "knob"); }
