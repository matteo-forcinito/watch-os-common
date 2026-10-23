#ifndef MINIGAME_H
#define MINIGAME_H

/* A game only supplies rules and drawing. The host owns time, memory, input,
 * pause, retry, exit and the one-shot result. No LVGL or ESP-IDF is needed here. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MG_STEP_MS 16
#define MG_MAX_STATE_BYTES 65536
#define MG_MAX_DURATION_MS 60000
#define MG_HUD_BYTES 80
#define MG_CHECKPOINT_BYTES 1536

typedef struct mg_canvas mg_canvas_t;
typedef enum { MG_PRESS, MG_RELEASE, MG_TURN, MG_CLICK, MG_MOVE } mg_input_kind_t;
typedef struct
{
    mg_input_kind_t kind;
    int x, y;                 /* Touch position, 0..1000 in the play area. */
    int direction;            /* -1 left, +1 right; zero for release/click. */
    bool held;
    bool cancelled;           /* Synthetic release on pause: cancel gestures. */
} mg_input_t;
typedef struct { int score; bool won; } mg_result_t;
typedef struct
{
    const char *id, *title, *instructions, *lose_text, *win_text;
    bool actor_walk;
    uint32_t sky, ground;     /* HTML-style RGB: 0xRRGGBB. */
    size_t state_size;
    void (*start)(void *state, uint32_t seed);
    void (*step)(void *state, int milliseconds);
    void (*input)(void *state, const mg_input_t *input);
    void (*draw)(void *state, mg_canvas_t *canvas);
    void (*hud)(const void *state, char text[MG_HUD_BYTES]);
    bool (*result)(const void *state, mg_result_t *result);
    const char *(*feedback)(const void *state);
    /* Optional continuing adventure. Persist only logical state, never pixels
     * or pointers. A new format requires a new checkpoint_version. */
    bool continuous;
    uint16_t checkpoint_version, checkpoint_bytes;
    bool (*save)(const void *state, void *data, size_t size);
    bool (*load)(void *state, const void *data, size_t size);
    const char *(*help)(const void *state);
    int (*actor_variant)(const void *state);
} mg_game_t;

typedef struct mg_session mg_session_t;
mg_session_t *mg_create(const mg_game_t *game, uint32_t seed, uint64_t now_ms);
void mg_destroy(mg_session_t *session);
void mg_restart(mg_session_t *session, uint32_t seed, uint64_t now_ms);
/* Call with active=false when hidden/asleep: paused time is never credited. */
void mg_advance(mg_session_t *session, uint64_t now_ms, bool active);
void mg_input(mg_session_t *session, mg_input_kind_t kind, int x, int y, int direction);
bool mg_take_result(mg_session_t *session, mg_result_t *result);
bool mg_done(const mg_session_t *session);
int mg_elapsed(const mg_session_t *session);
void *mg_state(mg_session_t *session);
uint32_t mg_random(uint32_t *state);
#endif
