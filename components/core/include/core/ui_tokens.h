#ifndef CORE_UI_TOKENS_H
#define CORE_UI_TOKENS_H

/*
 * NEON NOIR design tokens: the only colours, spacings, radii, line widths
 * and motion timings the system UI uses. Plain #defines (no LVGL types), so
 * core (lv_port, app_manager) and ui share them without a dependency loop.
 *
 *   Black is the canvas (AMOLED: off pixels). Thin neon lines, tracked
 *   uppercase captions, large light digits. Glow is drawn with precomputed
 *   dark tints of the neon (the _G1/_G2/_G3 ramps), never with shadows,
 *   blur or animated opacity.
 *
 * Colours are 0xRRGGBB and chosen to stay clean in RGB565.
 */

/* ---- surfaces (few and very dark) */
#define TK_VOID          0x000000   /* background of everything */
#define TK_SURFACE_1     0x0A0E16   /* rare cards, dialogs, keyboard */
#define TK_SURFACE_2     0x131A26   /* pressed flash, slider/switch tracks */
#define TK_LINE          0x1C2636   /* hairlines, separators, ring tracks */
#define TK_LINE_HI       0x2C3B52   /* inactive chip borders, minor ticks */

/* ---- text */
#define TK_TEXT          0xE6F1FF   /* primary: cold white, softer than 0xFFFFFF */
#define TK_TEXT_2        0x8394AB   /* values, captions */
#define TK_TEXT_3        0x46536A   /* disabled, boot log */

/* ---- neon accents (one dominant per screen) */
#define TK_CYAN          0x00E5FF   /* system, selection, focus */
#define TK_MAGENTA       0xFF2BD6   /* media, social, AI, glitch */
#define TK_AMBER         0xFFB000   /* time, alarms, soft warnings, edit focus */
#define TK_LIME          0x7CFF4F   /* success, charge, health */
#define TK_VIOLET        0x8F6BFF   /* external apps, market */
#define TK_RED           0xFF3355   /* error, destructive, critical battery */

/* ---- glow ramps: neon over black at 35 % (G1), 18 % (G2), 8 % (G3) */
#define TK_CYAN_G1       0x00505A
#define TK_CYAN_G2       0x00292E
#define TK_CYAN_G3       0x001214
#define TK_MAGENTA_G1    0x590F4B
#define TK_MAGENTA_G2    0x2E0826
#define TK_MAGENTA_G3    0x140311
#define TK_AMBER_G1      0x593D00
#define TK_AMBER_G2      0x2E1F00
#define TK_AMBER_G3      0x140E00
#define TK_LIME_G1       0x2B591C
#define TK_LIME_G2       0x162E0E
#define TK_LIME_G3       0x0A1406
#define TK_VIOLET_G1     0x322559
#define TK_VIOLET_G2     0x1A132E
#define TK_VIOLET_G3     0x0B0914
#define TK_RED_G1        0x59121E
#define TK_RED_G2        0x2E090F
#define TK_RED_G3        0x140407

/* Glow levels as mix ratios (0-255 of the neon), for colours outside the
 * table (app colours): computed once at creation, never per frame. */
#define TK_GLOW_G1       89
#define TK_GLOW_G2       46
#define TK_GLOW_G3       20

/* ---- spacing (4 px grid) */
#define TK_SP_1          4
#define TK_SP_2          8
#define TK_SP_3          12
#define TK_SP_4          16
#define TK_SP_6          24
#define TK_SP_8          32
#define TK_TOUCH         48         /* minimum touch target */
#define TK_ROW_H         64         /* minimum row height */

/* Round panels: the circle is narrow at top and bottom. */
#define TK_ROUND_PAD_TOP     34
#define TK_ROUND_PAD_X       40
#define TK_ROUND_PAD_BOTTOM  140

/* ---- radii (four values) */
#define TK_R_XS          4          /* chips, bars, switch knobs */
#define TK_R_S           10         /* rows, fields */
#define TK_R_M           16         /* dialogs, cards */
#define TK_R_PILL        0x7FFF     /* = LV_RADIUS_CIRCLE */

/* ---- line widths */
#define TK_LW_HAIR       1          /* separators */
#define TK_LW            2          /* chip borders, brackets */
#define TK_LW_ACCENT     3          /* focus, row notch */
#define TK_LW_RING       4          /* battery and progress rings */

/* ---- motion (ms). Enter with ease_out, leave with ease_in. */
#define TK_T_INSTANT     60         /* pressed colour */
#define TK_T_FAST        120        /* switch, selector, value reveal, seconds glide */
#define TK_T_PAGE        180        /* screen slide */
#define TK_T_OVERLAY     240        /* toast, sheets, dialogs */
#define TK_T_TOAST_HOLD  2000
#define TK_T_BREATH      2400       /* breathing of live elements */
#define TK_T_BLINK       1000       /* clock colon, cursor */
#define TK_T_GLITCH      33         /* one glitch frame (two are shown) */
#define TK_T_LIVE_TICK   40         /* period of "live" timers (25 fps) */

#endif
