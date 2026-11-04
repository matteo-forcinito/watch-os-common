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
    uint8_t alerts;   /* grows with every message that asks for a sound (CI finished) */
    uint32_t version; /* grows with every change */
} deck_state_t;

/* Listen on hid_link's Deck channel; `command` gets {"t":"cmd"} messages (link task). */
void deck_core_init(void (*command)(const char *json));
/* The agent spoke within the last seconds over a subscribed channel. */
bool deck_agent_alive(void);
/* Version the agent reported ("" if unknown). */
void deck_agent_version(char *out, size_t size);
/* Copy of the state; its message is cleared, so it is shown once. */
void deck_state_take(deck_state_t *out);

const deck_profile_t *deck_profile_for(const char *app, const char *title);
const char *deck_profile_name(const char *app, const char *title);
/* Keys every profile ends with: lock, display off, task manager... */
const deck_key_t *deck_system_keys(int *count);

bool deck_send_key(const deck_key_t *key, bool mac);
/*
 * Whatever a key does: a shortcut, an agent action, or a local one ("@switch":
 * Alt+Tab with Alt held while taps follow, Cmd+Tab on macOS; "@windows": the
 * task view, for devices without a window list page). False: nothing sent.
 */
bool deck_run_key(const deck_key_t *key, bool mac);

/*
 * Lists the agent sends, one message per item and then i = -1:
 *   DECK_LIST_WINDOWS  "win"  windows open on the PC (after the "windows" action); tap: "focus:N"
 *   DECK_LIST_APPS     "app"  quick launch (apps and sites chosen in the agent); tap: "launch:N"
 *   DECK_LIST_KEYS     "ck"   the user's own shortcuts for the app in front (`app` of the
 *                             items = that app); tap: "ck:N", the agent presses the keys
 *   DECK_LIST_GIT      "git"  repository of the VS Code window in front, empty otherwise:
 *                             rows "repo", "branch", "state" ("changed,ahead,behind,conflicts,
 *                             stash,untracked"), "msg" (proposed commit message), then "br"
 *                             local branches; actions "git:pull|push|fetch|commit|stash|pop|
 *                             diff|br:N"
 *   DECK_LIST_CI       "ci"   latest CI run per repository: app = "run"|"ok"|"ko"|"stop"|"off",
 *                             title = "repo · branch · 3m12s"; tap: "ci:N" restarts it
 */
#define DECK_LIST_MAX 16
typedef enum
{
    DECK_LIST_WINDOWS,
    DECK_LIST_APPS,
    DECK_LIST_KEYS,
    DECK_LIST_GIT,
    DECK_LIST_CI,
    DECK_LIST_COUNT,
} deck_list_t;
typedef struct
{
    char app[16];   /* process name (windows, keys) or kind: "app", "web" (launchers) */
    char title[48]; /* what the tile shows */
} deck_item_t;
/* Copies a list; returns its size. `version` grows with every new list. */
int deck_list(deck_list_t list, deck_item_t *out, int max, uint32_t *version);
bool deck_send_action(const char *action); /* false: the agent is not listening */

/* Flat JSON readers for the agent's messages. */
void deck_json_text(const char *json, const char *key, char *out, size_t size);
int deck_json_number(const char *json, const char *key, int fallback);

#endif
