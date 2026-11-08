#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef struct { char from[13], type[12], text[16]; int value; } input_job_t;
enum { SCENE_NONE, SCENE_DIAL, SCENE_TEXT, SCENE_LIGHT, SCENE_ORBI };
static int s_current, s_dial_value, clears, dials, pokes, received, allocations, frees;
static void *s_orbi;
static bool alloc_fail, post_fail;
static void (*pending)(void *);
static void *pending_arg;
static char last_from[13], last_text[16];
static void *heap_caps_calloc(size_t n, size_t size, int caps)
{ assert(caps == 3); if (alloc_fail) return NULL; ++allocations; return calloc(n, size); }
static void tracked_free(void *p) { if (p) { ++frees; free(p); } }
static bool ui_async(void (*fn)(void *), void *arg)
{ if (post_fail) return false; pending = fn; pending_arg = arg; return true; }
static void dial_show(void) { ++dials; }
static void clear_scene(void) { ++clears; s_current = SCENE_NONE; }
static void orbi_view_poke(void *orbi) { assert(orbi == s_orbi); ++pokes; }
static void link_event_received(const char *from, const char *type, const char *text, int value)
{ (void)type; (void)value; ++received; strcpy(last_from, from); snprintf(last_text, sizeof(last_text), "%s", text ? text : ""); }
#define free tracked_free
/* PRODUCTION */
#undef free
static void drain(void) { assert(pending); void (*fn)(void *) = pending; pending = NULL; fn(pending_arg); assert(allocations == frees); }
int main(void)
{
    assert(!link_scene_input(NULL, "dial", NULL, 1));
    assert(!link_scene_input("001122334455", "unknown", NULL, 1));
    alloc_fail = true; assert(!link_scene_input("001122334455", "dial", NULL, 1)); alloc_fail = false;
    post_fail = true; assert(!link_scene_input("001122334455", "dial", NULL, 1)); post_fail = false;
    assert(allocations == frees && received == 0);
    s_current = SCENE_DIAL; s_dial_value = 50;
    assert(link_scene_input("001122334455", "dial", NULL, INT_MAX)); assert(s_dial_value == 50 && received == 0);
    drain(); assert(s_dial_value == 100 && dials == 1 && received == 1);
    assert(link_scene_input("001122334455", "dial", NULL, INT_MIN)); drain(); assert(s_dial_value == 0);
    char text[] = "light";
    s_current = SCENE_LIGHT; assert(link_scene_input("001122334455", "tap", text, 0)); text[0] = 'X';
    drain(); assert(clears == 1 && strcmp(last_text, "light") == 0 && strcmp(last_from, "001122334455") == 0);
    s_current = SCENE_DIAL; assert(link_scene_input("001122334455", "dial_done", NULL, 80)); drain(); assert(clears == 2);
    s_current = SCENE_ORBI; s_orbi = &s_current;
    assert(link_scene_input("001122334455", "orbi", "pet", 0)); drain(); assert(pokes == 1);
    assert(received == 5 && allocations == frees);
    puts("Queued peer input, source identity, no re-emission and bounds passed");
    return 0;
}
