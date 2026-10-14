/*
 * Deck actions: what one key does, written as text (pure C, host-tested).
 *
 *   ctrl+shift+m                 a key with modifiers
 *   ctrl+k ctrl+c                chords one after the other (space separated)
 *   win+r; wait:300; type:wt; enter
 *                                steps separated by ';'
 *   type:any text                types the text (host keyboard layout aware)
 *   media:mute                   play next prev stop mute volup voldown calc mail home search
 *   wheel:-3                     mouse wheel
 *   wait:250                     pause in milliseconds (max 5000)
 *
 * Modifiers: ctrl shift alt win (gui, cmd) altgr. Keys: a-z 0-9 f1-f24 enter
 * esc tab space backspace delete insert home end pageup pagedown up down left
 * right printscreen pause capslock menu minus equal comma period slash
 * semicolon quote backslash grave lbracket rbracket kpplus kpminus.
 */
#ifndef DECK_ACTION_H
#define DECK_ACTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DECK_STEPS 24

typedef enum
{
    DECK_STEP_KEY,   /* modifiers + key (key 0 = modifiers only) */
    DECK_STEP_TEXT,  /* text at offset/length in the action string */
    DECK_STEP_MEDIA, /* consumer usage */
    DECK_STEP_WHEEL, /* wheel clicks */
    DECK_STEP_WAIT,  /* milliseconds */
} deck_step_kind_t;

typedef struct
{
    uint8_t kind;
    uint8_t modifiers;
    uint8_t key;
    int8_t wheel;
    uint16_t value;   /* media usage or wait ms */
    uint16_t offset;  /* text */
    uint16_t length;
} deck_step_t;

typedef struct
{
    deck_step_t steps[DECK_STEPS];
    int count;
    int error_at;      /* offset of the first error, -1 = valid */
    const char *error; /* short Italian message */
} deck_action_t;

/* Parse; returns true when valid. On error, out->error_at/error explain. */
bool deck_action_parse(const char *text, deck_action_t *out);

#endif
