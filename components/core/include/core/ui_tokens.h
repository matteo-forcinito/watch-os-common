#ifndef CORE_UI_TOKENS_H
#define CORE_UI_TOKENS_H

/* Every token can be overridden by a board project (compile definition), e.g. a
 * board-specific palette; the values below are the shared NEON NOIR theme. */

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
#ifndef TK_VOID
#define TK_VOID          0x000000   /* background of everything */
#endif
#ifndef TK_SURFACE_1
#define TK_SURFACE_1     0x0A0E16   /* rare cards, dialogs, keyboard */
#endif
#ifndef TK_SURFACE_2
#define TK_SURFACE_2     0x131A26   /* pressed flash, slider/switch tracks */
#endif
#ifndef TK_LINE
#define TK_LINE          0x1C2636   /* hairlines, separators, ring tracks */
#endif
#ifndef TK_LINE_HI
#define TK_LINE_HI       0x2C3B52   /* inactive chip borders, minor ticks */
#endif

/* ---- text */
#ifndef TK_TEXT
#define TK_TEXT          0xE6F1FF   /* primary: cold white, softer than 0xFFFFFF */
#endif
#ifndef TK_TEXT_2
#define TK_TEXT_2        0x8394AB   /* values, captions */
#endif
#ifndef TK_TEXT_3
#define TK_TEXT_3        0x46536A   /* disabled, boot log */
#endif

/* ---- neon accents (one dominant per screen) */
#ifndef TK_CYAN
#define TK_CYAN          0x00E5FF   /* system, selection, focus */
#endif
#ifndef TK_MAGENTA
#define TK_MAGENTA       0xFF2BD6   /* media, social, AI, glitch */
#endif
#ifndef TK_AMBER
#define TK_AMBER         0xFFB000   /* time, alarms, soft warnings, edit focus */
#endif
#ifndef TK_LIME
#define TK_LIME          0x7CFF4F   /* success, charge, health */
#endif
#ifndef TK_VIOLET
#define TK_VIOLET        0x8F6BFF   /* external apps, market */
#endif
#ifndef TK_RED
#define TK_RED           0xFF3355   /* error, destructive, critical battery */
#endif

/* ---- glow ramps: neon over black at 35 % (G1), 18 % (G2), 8 % (G3) */
#ifndef TK_CYAN_G1
#define TK_CYAN_G1       0x00505A
#endif
#ifndef TK_CYAN_G2
#define TK_CYAN_G2       0x00292E
#endif
#ifndef TK_CYAN_G3
#define TK_CYAN_G3       0x001214
#endif
#ifndef TK_MAGENTA_G1
#define TK_MAGENTA_G1    0x590F4B
#endif
#ifndef TK_MAGENTA_G2
#define TK_MAGENTA_G2    0x2E0826
#endif
#ifndef TK_MAGENTA_G3
#define TK_MAGENTA_G3    0x140311
#endif
#ifndef TK_AMBER_G1
#define TK_AMBER_G1      0x593D00
#endif
#ifndef TK_AMBER_G2
#define TK_AMBER_G2      0x2E1F00
#endif
#ifndef TK_AMBER_G3
#define TK_AMBER_G3      0x140E00
#endif
#ifndef TK_LIME_G1
#define TK_LIME_G1       0x2B591C
#endif
#ifndef TK_LIME_G2
#define TK_LIME_G2       0x162E0E
#endif
#ifndef TK_LIME_G3
#define TK_LIME_G3       0x0A1406
#endif
#ifndef TK_VIOLET_G1
#define TK_VIOLET_G1     0x322559
#endif
#ifndef TK_VIOLET_G2
#define TK_VIOLET_G2     0x1A132E
#endif
#ifndef TK_VIOLET_G3
#define TK_VIOLET_G3     0x0B0914
#endif
#ifndef TK_RED_G1
#define TK_RED_G1        0x59121E
#endif
#ifndef TK_RED_G2
#define TK_RED_G2        0x2E090F
#endif
#ifndef TK_RED_G3
#define TK_RED_G3        0x140407
#endif

/* Glow levels as mix ratios (0-255 of the neon), for colours outside the
 * table (app colours): computed once at creation, never per frame. */
#ifndef TK_GLOW_G1
#define TK_GLOW_G1       89
#endif
#ifndef TK_GLOW_G2
#define TK_GLOW_G2       46
#endif
#ifndef TK_GLOW_G3
#define TK_GLOW_G3       20
#endif

/* ---- spacing (4 px grid) */
#ifndef TK_SP_1
#define TK_SP_1          4
#endif
#ifndef TK_SP_2
#define TK_SP_2          8
#endif
#ifndef TK_SP_3
#define TK_SP_3          12
#endif
#ifndef TK_SP_4
#define TK_SP_4          16
#endif
#ifndef TK_SP_6
#define TK_SP_6          24
#endif
#ifndef TK_SP_8
#define TK_SP_8          32
#endif
#ifndef TK_TOUCH
#define TK_TOUCH         48         /* minimum touch target */
#endif
#ifndef TK_ROW_H
#define TK_ROW_H         64         /* minimum row height */
#endif

/* Round panels: the circle is narrow at top and bottom. */
#ifndef TK_ROUND_PAD_TOP
#define TK_ROUND_PAD_TOP     34
#endif
#ifndef TK_ROUND_PAD_X
#define TK_ROUND_PAD_X       40
#endif
#ifndef TK_ROUND_PAD_BOTTOM
#define TK_ROUND_PAD_BOTTOM  140
#endif

/* ---- radii (four values) */
#ifndef TK_R_XS
#define TK_R_XS          4          /* chips, bars, switch knobs */
#endif
#ifndef TK_R_S
#define TK_R_S           10         /* rows, fields */
#endif
#ifndef TK_R_M
#define TK_R_M           16         /* dialogs, cards */
#endif
#ifndef TK_R_PILL
#define TK_R_PILL        0x7FFF     /* = LV_RADIUS_CIRCLE */
#endif

/* ---- line widths */
#ifndef TK_LW_HAIR
#define TK_LW_HAIR       1          /* separators */
#endif
#ifndef TK_LW
#define TK_LW            2          /* chip borders, brackets */
#endif
#ifndef TK_LW_ACCENT
#define TK_LW_ACCENT     3          /* focus, row notch */
#endif
#ifndef TK_LW_RING
#define TK_LW_RING       4          /* battery and progress rings */
#endif

/* ---- motion (ms). Enter with ease_out, leave with ease_in. */
#ifndef TK_T_INSTANT
#define TK_T_INSTANT     60         /* pressed colour */
#endif
#ifndef TK_T_FAST
#define TK_T_FAST        120        /* switch, selector, value reveal, seconds glide */
#endif
#ifndef TK_T_PAGE
#define TK_T_PAGE        180        /* screen slide */
#endif
#ifndef TK_T_OVERLAY
#define TK_T_OVERLAY     240        /* toast, sheets, dialogs */
#endif
#ifndef TK_T_TOAST_HOLD
#define TK_T_TOAST_HOLD  2000
#endif
#ifndef TK_T_BREATH
#define TK_T_BREATH      2400       /* breathing of live elements */
#endif
#ifndef TK_T_BLINK
#define TK_T_BLINK       1000       /* clock colon, cursor */
#endif
#ifndef TK_T_GLITCH
#define TK_T_GLITCH      33         /* one glitch frame (two are shown) */
#endif
#ifndef TK_T_LIVE_TICK
#define TK_T_LIVE_TICK   40         /* period of "live" timers (25 fps) */
#endif

#endif
