#ifndef DECK_CORE_H
#define DECK_CORE_H
/*
 * Deck core: PC touchbar profiles, the PC agent's state and its protocol
 * (deck_core.c). Used by Remote's PC page and by touchbar devices.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DECK_CMD 0x80 /* modifier: Ctrl, or Cmd on macOS */

typedef struct
{
    const char *label;
    uint8_t modifiers; /* HIDL_MOD_*, DECK_CMD */
    uint8_t key;       /* HID usage */
    const char *action; /* agent action instead of a key, or NULL */
} deck_key_t;

typedef struct
{
    const char *apps;  /* comma separated process names, lower case, no .exe; NULL = any */
    const char *title; /* part of the window title; NULL = any */
    const char *name;  /* "" for the generic profile */
    const char *icon;  /* LV_SYMBOL_* */
    uint32_t color;    /* UI_COLOR_* */
    const deck_key_t *keys;
    uint8_t count;
} deck_profile_t;

typedef struct
{
    char app[24];
    char title[64];
    char track[64];
    char artist[48];
    char message[48];
    int8_t cpu, ram, mic; /* -1 unknown */
    bool playing;
    uint32_t version; /* grows with every change */
} deck_state_t;

/* Listen on hid_link's Deck channel; `command` gets {"t":"cmd"} messages (link task). */
void deck_core_init(void (*command)(const char *json));
/* The agent spoke within the last seconds over a subscribed channel. */
bool deck_agent_alive(void);
/* Copy of the state; its message is cleared, so it is shown once. */
void deck_state_take(deck_state_t *out);

const deck_profile_t *deck_profile_for(const char *app, const char *title);
const char *deck_profile_name(const char *app, const char *title);
/* Keys every profile ends with: lock, display off, task manager... */
const deck_key_t *deck_system_keys(int *count);

bool deck_send_key(const deck_key_t *key, bool mac);
bool deck_send_action(const char *action); /* false: the agent is not listening */

/* Flat JSON readers for the agent's messages. */
void deck_json_text(const char *json, const char *key, char *out, size_t size);
int deck_json_number(const char *json, const char *key, int fallback);

#endif
