#ifndef SOLCO_UI_KIT_H
#define SOLCO_UI_KIT_H
/*
 * Solco UI kit: NEON NOIR building blocks drawn by hand so a whole deck costs
 * a handful of LVGL objects.
 *
 *   sk_strip  one object, N touch cells (text drawn, no child labels);
 *             a press invalidates only its own cell
 *   sk_wave   scrolling waveform rendered into a native RGB565 image by
 *             columns (no lines), with history behind the playhead
 *   sk_fader  relative-drag fader (a tap never jumps the value)
 *   sk_meter  segmented peak meter, invalidated only when a segment changes
 *
 * Semantic colours are aliases of core/ui_tokens.h, nothing else.
 */
#include "core/ui_tokens.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/* ---- Solco semantic tokens (aliases only) */
#define SK_DECK_A        TK_CYAN     /* deck identity */
#define SK_DECK_B        TK_MAGENTA
#define SK_CUE           TK_AMBER    /* cue, tempo/pitch offset */
#define SK_LOOP          TK_LIME     /* loop region and loop controls */
#define SK_FX            TK_VIOLET   /* performance effects (REV, future FX 8x8) */
#define SK_ALERT         TK_RED      /* error, REC, destructive */
#define SK_CELL_GAP      TK_SP_1     /* gap between strip cells */
#define SK_WAVE_COL      4           /* waveform column: 3 lit px + 1 gap */
#define SK_WAVE_BIN      512         /* source frames per column (engine preview bin) */
#define SK_WAVE_PAST_PCT 40          /* playhead position, % of the width */
#define SK_TICK_MS       TK_T_LIVE_TICK
#define SK_HOLD_MS       1000        /* hold-to-confirm for destructive cells */
#define SK_FLASH_MS      600         /* error flash of a cell */
#define SK_DOUBLE_TAP_MS 320         /* fader reset */
#define SK_PITCH_PPM     80000       /* pitch fader range: +/- 8 % */

static inline uint32_t sk_deck_color(int deck) { return deck ? SK_DECK_B : SK_DECK_A; }

/* Precomputed glow ramp of a token neon (table lookup, never mixed per frame). */
typedef struct
{
    uint32_t neon, g1, g2, g3;
} sk_tone_t;
sk_tone_t sk_tone(uint32_t neon);

/* Native 16-bit pixel for the display (RGB565 or RGB565 byte-swapped). */
uint16_t sk_px(uint32_t rgb);
void sk_init(void);

/* Plain label: no wrap, dot overflow; `font` NULL = 16 px. */
lv_obj_t *sk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y, int w,
                   lv_text_align_t align);
/* HUD caption: tracked uppercase 12/14 px (ASCII only). */
lv_obj_t *sk_hud(lv_obj_t *parent, const char *text, uint32_t color, int x, int y, int w,
                 lv_text_align_t align, bool micro);
/* Transparent full-size container without scrolling. */
lv_obj_t *sk_panel(lv_obj_t *parent, int x, int y, int w, int h, bool opaque);
/* Change a label text only when it differs (no invalidation otherwise). */
void sk_set_text(lv_obj_t *label, const char *text);
void sk_set_color(lv_obj_t *obj, uint32_t color);

/* ---- strip: one object, up to SK_STRIP_MAX touch cells */
#define SK_STRIP_MAX 8
enum
{
    SK_IDLE,    /* outline, text in TEXT */
    SK_ON,      /* filled G2, neon border and text: active/engaged */
    SK_ARMED,   /* neon border, text neon, no fill: ready/next action */
    SK_OFF,     /* disabled: TEXT_3, hairline */
    SK_ERR,     /* red flash */
};
typedef struct sk_strip sk_strip_t;
/* pressed = true on PRESSED (lowest latency for live actions), false on release. */
typedef void (*sk_strip_cb_t)(sk_strip_t *strip, int cell, bool pressed);
struct sk_strip
{
    lv_obj_t *obj;
    sk_strip_cb_t cb;
    void *user;
    int tag, n;
    int8_t pressed;
    uint8_t weight[SK_STRIP_MAX];
    uint8_t state[SK_STRIP_MAX];
    uint8_t progress[SK_STRIP_MAX]; /* 0-255 hold fill */
    uint32_t neon[SK_STRIP_MAX];
    uint32_t flash_until[SK_STRIP_MAX];
    char text[SK_STRIP_MAX][20];
    const lv_font_t *font;
};
sk_strip_t *sk_strip_create(lv_obj_t *parent, int x, int y, int w, int h, int n,
                            const uint8_t *weights, sk_strip_cb_t cb, int tag);
void sk_strip_set(sk_strip_t *strip, int cell, const char *text, uint32_t neon, int state);
void sk_strip_progress(sk_strip_t *strip, int cell, int progress);
void sk_strip_flash(sk_strip_t *strip, int cell);
/* Change the number and widths of the cells (e.g. manual loop row <-> beat loop row). */
void sk_strip_layout(sk_strip_t *strip, int n, const uint8_t *weights);
/* Ends expired error flashes; call from the live tick. */
void sk_strip_tick(sk_strip_t *strip);

/* ---- waveform
 *
 * Two motions:
 *   SCROLL (FOCUS)  the audio moves under a fixed playhead: whole image per frame
 *   PAGED  (DECKS)  the page stands still and the playhead sweeps it: per frame only
 *                   the columns the playhead crossed and the freshly read preview
 *                   columns are drawn and invalidated; the page flips near the edge
 *                   (at most one full flip per live tick across all waveforms).
 * Colour per column from the engine's 3 bands: low = deck G1 (body), mid = deck
 * neon, high = TK_TEXT (tips); the past side uses the next darker ramp step. */
#define SK_WAVE_HIST 512 /* engine bins of history ring (~6 s at 1x) */
typedef struct
{
    lv_obj_t *img;
    lv_image_dsc_t dsc;
    uint16_t *px;
    int w, h;
    bool paged;
    uint32_t fpc;              /* source frames per column (multiple of SK_WAVE_BIN) */
    sk_tone_t tone;
    uint16_t hist[SK_WAVE_HIST];
    uint8_t band[SK_WAVE_HIST][3];
    uint32_t tag[SK_WAVE_HIST];
    uint32_t norm, bnorm;
    int64_t origin;            /* absolute pixel shown at x = 0 */
    int play_x;                /* playhead x of the last render */
    uint32_t data_lo, data_hi; /* engine bins changed since the last render */
    bool data;
    /* last rendered inputs */
    uint32_t key_frame, key_in, key_out, key_cue[4];
    uint8_t key_mode, key_loop, key_percent, key_marked, key_cues;
    bool drawn, dirty;
} sk_wave_t;
enum
{
    SK_WAVE_PLAY,
    SK_WAVE_TOUCH,   /* finger/scratch holds the platter: amber playhead */
    SK_WAVE_LOADING, /* skeleton with progress */
    SK_WAVE_EMPTY,
};
typedef struct
{
    int mode, percent;
    bool loop_on, marked;
    uint32_t loop_in, loop_out;
    uint8_t cues;         /* bit mask of hot cues */
    uint32_t cue[4];      /* hot cue frames */
} sk_wave_state_t;
sk_wave_t *sk_wave_create(lv_obj_t *parent, int x, int y, int w, int h, uint32_t neon, bool paged,
                          uint32_t frames_per_col);
/* Store the engine preview (64 bins of 512 frames from `frame`) into the history;
 * bands may be NULL. */
void sk_wave_feed(sk_wave_t *wave, const uint16_t peaks[64], const uint8_t bands[64][3], uint32_t frame);
/* Draw what changed and invalidate only that. */
void sk_wave_render(sk_wave_t *wave, uint32_t frame, const sk_wave_state_t *state);
void sk_wave_reset(sk_wave_t *wave);
/* Start of a live tick: grants one full page flip to the paged waveforms. */
void sk_wave_tick(void);

/* ---- fader */
enum
{
    SK_FADER_CROSS,  /* A/B ends, centre detent */
    SK_FADER_LEVEL,  /* fill from the left */
    SK_FADER_PITCH,  /* fill from the centre, centre detent */
};
typedef struct sk_fader sk_fader_t;
typedef void (*sk_fader_cb_t)(sk_fader_t *fader, int value);
struct sk_fader
{
    lv_obj_t *obj;
    sk_fader_cb_t cb;
    int tag, kind, min, max, value, home;
    int press_x, press_value;
    uint32_t last_release;
    uint32_t neon;
    bool dragging;
};
sk_fader_t *sk_fader_create(lv_obj_t *parent, int x, int y, int w, int h, int kind, int min,
                            int max, int home, uint32_t neon, sk_fader_cb_t cb, int tag);
void sk_fader_set(sk_fader_t *fader, int value); /* from the engine; ignored while dragging */

/* ---- meter */
typedef struct
{
    lv_obj_t *obj;
    int lit;
} sk_meter_t;
sk_meter_t *sk_meter_create(lv_obj_t *parent, int x, int y, int w, int h);
/* peak: 0-32767 digital peak */
void sk_meter_set(sk_meter_t *meter, uint16_t peak);

#endif
