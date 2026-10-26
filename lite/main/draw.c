#include "core/clock.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hardware/board.h"
#include "hardware/display.h"
#include "hardware/pmu.h"
#include "lite.h"
#include "content.h"
#include "core/settings.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

/* Original small 5x7 bitmap alphabet. No font engine, scene graph or framebuffer. */
static const uint8_t glyphs[][7] = {
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},      {14, 17, 1, 2, 4, 8, 31},
    {30, 1, 1, 14, 1, 1, 30},     {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},
    {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},       {14, 17, 17, 14, 17, 17, 14},
    {14, 17, 17, 15, 1, 1, 14},   {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30},
    {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31},
    {31, 16, 16, 30, 16, 16, 16}, {14, 17, 16, 23, 17, 17, 14}, {17, 17, 17, 31, 17, 17, 17},
    {14, 4, 4, 4, 4, 4, 14},      {7, 2, 2, 2, 18, 18, 12},     {17, 18, 20, 24, 20, 18, 17},
    {16, 16, 16, 16, 16, 16, 31}, {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17},
    {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13},
    {30, 17, 17, 30, 20, 18, 17}, {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},
    {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10},
    {17, 17, 10, 4, 10, 17, 17},  {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},
    {0, 4, 4, 0, 4, 4, 0},        {0, 0, 0, 0, 0, 4, 4},        {0, 0, 0, 31, 0, 0, 0},
    {1, 2, 4, 8, 16, 0, 0},       {17, 2, 4, 8, 17, 0, 0},      {0, 4, 14, 31, 14, 4, 0}};
static const uint8_t *glyph(char c)
{
    c = (char)toupper((unsigned char)c);
    if (c >= '0' && c <= '9')
        return glyphs[c - '0'];
    if (c >= 'A' && c <= 'Z')
        return glyphs[10 + c - 'A'];
    switch (c)
    {
    case ':':
        return glyphs[36];
    case '.':
        return glyphs[37];
    case '-':
        return glyphs[38];
    case '/':
        return glyphs[39];
    case '%':
        return glyphs[40];
    case '+':
        return glyphs[41];
    default:
        return NULL;
    }
}
typedef struct
{
    int x, y, scale;
    uint16_t color;
    char text[80];
} line_t;
static line_t lines[14];
static int line_count;
typedef struct { int x, y, w, h; uint16_t color; } card_t;
static card_t cards[5];
static int card_count;
static void card(int x, int y, int w, int h, uint16_t color)
{
    if (card_count < 5) cards[card_count++] = (card_t){x, y, w, h, (uint16_t)((color << 8) | (color >> 8))};
}
static SemaphoreHandle_t transferred;
static uint16_t *strip;
static atomic_uint frames;
uint32_t lite_draw_frames(void){return atomic_load(&frames);}
static void done(void *ctx)
{
    (void)ctx;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(transferred, &wake);
    if (wake)
        portYIELD_FROM_ISR();
}
bool lite_draw_init(void)
{
    transferred = xSemaphoreCreateBinary();
    strip = heap_caps_malloc((size_t)board_info()->width * 16 * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    return transferred && strip && display_init(done, NULL) == ESP_OK;
}
static void line(int y, int scale, uint16_t color, const char *text)
{
    if (line_count >= 14)
        return;
    line_t *l = &lines[line_count++];
    snprintf(l->text, sizeof(l->text), "%s", text);
    int max = (board_info()->width * 3 / 4) / (6 * scale);
    if ((int)strlen(l->text) > max)
        l->text[max] = 0;
    l->x = (board_info()->width - (int)strlen(l->text) * 6 * scale) / 2;
    l->y = y;
    l->scale = scale;
    l->color = (uint16_t)((color << 8) | (color >> 8));
}
void lite_draw(const lite_navigation_t *nav, bool idle, const char *address, const char *code)
{
    if (!strip)
        return;
    line_count = 0;
    card_count = 0;
    const int w = board_info()->width, h = board_info()->height;
    struct tm tm;
    clock_local(&tm);
    char s[96];
    bool alert = lite_ringing();
    int shift = idle ? (tm.tm_min % 5) - 2 : 0;
    if (alert)
    {
        line(h / 2 - 65, 3, 0xFD20, "PROMEMORIA");
        lite_alert_text(s, sizeof(s));
        line(h / 2 - 20, 2, 0xffff, s);
        line(h / 2 + 35, 2, 0xC618, "TOCCA: STOP");
        line(h / 2 + 65, 1, 0xC618, "SINISTRA: RINVIA 5 MIN");
    }
    else if (nav->page == LITE_CONFIRM_EXIT && !idle)
    {
        line(h / 2 - 88, w >= 360 ? 3 : 2, 0xffff, "ESCI DA LITE");
        line(h / 2 - 42, 2, 0x8410, "USCIRE DA LITE");
        card(w / 8, h / 2, w * 3 / 4, 44, 0x1124);
        line(h / 2 + 15, 1, 0x07F0, "SCORRI DA SINISTRA A DESTRA");
        line(h / 2 + 65, 1, 0x8410, "IN BASSO: ANNULLA");
        line(h / 2 + 84, 1, 0x8410, "O TIENI BOOT DI NUOVO");
    }
    else if (nav->page == LITE_CONNECTION && !idle)
    {
        line(h / 2 - 96, w >= 360 ? 3 : 2, 0x07F0, "GESTIONE WEB");
        if (lite_web_running()) {
            line(h / 2 - 42, 1, 0xffff, address);
            snprintf(s, sizeof(s), "CODICE %s", code);
            line(h / 2 - 12, 2, 0xffff, s);
        } else {
            line(h / 2 - 42, 2, 0xffff, "WI-FI SPENTO");
            line(h / 2 - 12, 1, 0x8410, "SI ATTIVA SOLO SU RICHIESTA");
        }
        card(w / 5, h / 2 + 40, w * 3 / 5, 40, 0x1124);
        line(h / 2 + 52, 2, 0x07F0, lite_web_running() ? "SPEGNI WI-FI" : "ACCENDI WI-FI");
        line(h / 2 + 98, 1, 0x8410, "IN ALTO: OROLOGIO");
    }
    else if ((nav->page == LITE_SETTINGS || nav->page == LITE_APPS || nav->page == LITE_PREFERENCES) && !idle)
    {
        int top = lite_navigation_row_top(nav), row = lite_navigation_row_height(nav);
        line(top - 38, w >= 360 ? 3 : 2, 0x07F0, nav->page == LITE_APPS ? "LE TUE APP" : "IMPOSTAZIONI");
        const char *labels[] = {lite_settings.aod ? "ALWAYS ON: SI" : "ALWAYS ON: NO",
                                lite_web_running() ? "WI-FI: ACCESO" : "WI-FI: SPENTO", NULL, "ALTRE OPZIONI", "ESCI DA LITE"};
        const char *apps[] = {"SVEGLIE", "AGENDA", "RACCOLTA", "ZAMPE"};
        for (int i = 0; i < lite_navigation_rows(nav); i++)
        {
            char b[48];
            if (nav->page == LITE_APPS) snprintf(b, sizeof(b), "%s", apps[i]);
            else if (nav->page == LITE_PREFERENCES) {
                const settings_t *st = settings_get();
                if (i == 0) snprintf(b,sizeof(b),"TOCCO: %s",st->tap_to_wake?"ATTIVO":"SPENTO");
                if (i == 1) snprintf(b,sizeof(b),"ORA: %s",st->time_24h?"24 ORE":"12 ORE");
                if (i == 2) snprintf(b,sizeof(b),"VOLUME: %u",st->volume);
                if (i == 3) snprintf(b,sizeof(b),"TIMEOUT: %u S",st->screen_timeout_s);
            }
            else if (i == 2)
                snprintf(b, sizeof(b), "LUCE: %u", lite_settings.brightness);
            else
                snprintf(b, sizeof(b), "%s", labels[i]);
            card(w / 8, top + i * row, w * 3 / 4, row - 4, nav->selected == i ? 0x1124 : 0x1082);
            line(top + i * row + (row - 4 - 14) / 2, 2, nav->selected == i ? 0x07F0 : 0xffff, b);
        }
        line(top + lite_navigation_rows(nav) * row + 12, 1, 0x8410, "SCORRI PER TORNARE ALL ORA");
    }
    else if (nav->page >= LITE_ALARMS && nav->page <= LITE_DOGS && !idle)
    {
        lite_content_t content; lite_content_load(nav, &content);
        line(h / 2 - 100, w >= 360 ? 3 : 2, 0x07F0, content.heading);
        int chars = (w * 3 / 4) / 12;
        char title[80]; snprintf(title,sizeof(title),"%s",content.title);
        if ((int)strlen(title) > chars) {
            char rest[80]; snprintf(rest,sizeof(rest),"%s",title+chars); title[chars]=0;
            line(h / 2 - 55,2,0xffff,title); line(h / 2 - 35,2,0xffff,rest);
        } else line(h / 2 - 48, 2, 0xffff, title);
        for (int i = 0; i < 3; i++) line(h / 2 - 4 + i * 18, 1, 0xC618, content.detail[i]);
        if (content.button[0]) {
            card(w / 5, h / 2 + 40, w * 3 / 5, 40, 0x1124);
            line(h / 2 + 52, 2, 0x07F0, content.button);
        }
        line(h / 2 + 98, 1, 0x8410, "SU/GIU: ALTRO - DESTRA: ORA");
    }
    else
    {
        if (clock_is_valid())
            clock_format_hm(&tm, s, sizeof(s));
        else
            snprintf(s, sizeof(s), "--:--");
        line(h / 2 - 64 + shift, w >= 400 ? 8 : 6, idle ? 0x8410 : 0xffff, s);
        static const char *week[] = {"DOMENICA","LUNEDI","MARTEDI","MERCOLEDI","GIOVEDI","VENERDI","SABATO"};
        static const char *months[] = {"GENNAIO","FEBBRAIO","MARZO","APRILE","MAGGIO","GIUGNO","LUGLIO","AGOSTO","SETTEMBRE","OTTOBRE","NOVEMBRE","DICEMBRE"};
        line(h / 2 + 4 + shift, w >= 360 ? 2 : 1, idle ? 0xBDF7 : 0xC618, week[tm.tm_wday]);
        snprintf(s,sizeof(s),"%d %s %d",tm.tm_mday,months[tm.tm_mon],tm.tm_year+1900);
        line(h / 2 + 24 + shift, w >= 360 ? 2 : 1, idle ? 0xBDF7 : 0xC618, s);
        int battery = pmu_battery_percent();
        if (battery >= 0)
            snprintf(s, sizeof(s), "BAT %d%% %s", battery, pmu_is_charging() ? "+" : "");
        else
            snprintf(s, sizeof(s), "BAT --");
        line(h / 2 + 52 + shift, 2, idle ? 0xBDF7 : 0x07F0, s);
        alarm_t a;
        time_t when;
        if (alarm_next(&a, &when))
        {
            struct tm next;
            localtime_r(&when, &next);
            snprintf(s, sizeof(s), "SVEGLIA %02d:%02d", next.tm_hour, next.tm_min);
            line(h / 2 + 78 + shift, 2, 0xBDF7, s);
        }
        if (!idle)
        {
            line(h / 2 - 108, 2, 0x07F0, "WATCH LITE");
            line(h / 2 + 110, 1, 0x8410, "SU: SETTINGS - SINISTRA: APP");
            line(h / 2 + 126, 1, 0x8410, "GIU: WEB - BACK: ALWAYS ON");
        }
    }
    for (int y = 0; y < h; y += 8)
    {
        int x1 = 0, x2 = w - 1, y1 = y, y2 = y + 7 < h ? y + 7 : h - 1;
        display_round_area(&x1, &y1, &x2, &y2);
        int width = x2 - x1 + 1, height = y2 - y1 + 1;
        if (width <= 0 || width > w || height <= 0 || height > 16)
            return;
        memset(strip, 0, (size_t)width * height * 2);
        for (int c = 0; c < card_count; c++) {
            const card_t *b = &cards[c];
            for (int py = y1; py <= y2; py++) {
                if (py < b->y || py >= b->y + b->h) continue;
                int edge = py - b->y;
                if (b->y + b->h - 1 - py < edge) edge = b->y + b->h - 1 - py;
                int inset = edge < 7 ? 7 - edge : 0;
                for (int px = b->x + inset; px < b->x + b->w - inset; px++)
                    if (px >= x1 && px <= x2) strip[(py-y1)*width+px-x1] = b->color;
            }
        }
        for (int n = 0; n < line_count; n++)
        {
            line_t *l = &lines[n];
            for (int py = y1; py <= y2; py++)
            {
                int gy = (py - l->y) / l->scale;
                if (py < l->y || gy >= 7)
                    continue;
                for (unsigned c = 0; c < strlen(l->text); c++)
                {
                    const uint8_t *g = glyph(l->text[c]);
                    if (!g)
                        continue;
                    for (int gx = 0; gx < 5; gx++)
                        if (g[gy] & (1 << (4 - gx)))
                        {
                            for (int k = 0; k < l->scale; k++)
                            {
                                int px = l->x + ((int)c * 6 + gx) * l->scale + k;
                                if (px >= x1 && px <= x2)
                                    strip[(py - y1) * width + px - x1] = l->color;
                            }
                        }
                }
            }
        }
        xSemaphoreTake(transferred, 0);
        if (display_draw(x1, y1, x2, y2, strip) != ESP_OK)
            return;
        if (xSemaphoreTake(transferred, pdMS_TO_TICKS(1500)) != pdTRUE)
            return;
    }
    atomic_fetch_add(&frames,1);
}
