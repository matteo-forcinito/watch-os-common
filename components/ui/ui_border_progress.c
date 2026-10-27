/* A passive perimeter progress indicator: no input device, timer or task.
 * Geometry is cached per instance; caller updates it under the LVGL lock.
 * Points follow the actual perimeter length, including rectangular corners.
 *
 * Drawn with rounded-rectangle (or circle) borders, not polylines: an lv_line
 * along the whole perimeter cost about 57 ms per full redraw on the AMOLED 1.8
 * (one draw task per segment, anti-aliased skew masks), a border about 5 ms.
 * The partial indicator is the same border seen through up to four clip
 * windows, one per quadrant: inside a quadrant the path (clockwise from the top
 * centre) moves monotonically in x and in y, so the part before the end point
 * is a rectangle. Two small dots give the rounded line caps. */
#include "ui/ui.h"
#include "hardware/board.h"
#include <math.h>
#include <string.h>

#define PATH_POINTS 101
#define PI_F 3.14159265358979323846f
#define STROKE 5
#define HALF 3   /* half the stroke, rounded up: margin across the direction of travel */

typedef struct
{
    lv_obj_t *track;
    lv_obj_t *full;            /* indicator at 100 %: one whole border */
    lv_obj_t *window[4];       /* quadrants: top-right, bottom-right, bottom-left, top-left */
    lv_obj_t *window_ring[4];
    lv_obj_t *dot_start;
    lv_obj_t *dot_end;
    lv_point_precise_t path[PATH_POINTS];
    float distance[PATH_POINTS];
    uint32_t count;
    int32_t value;
    int32_t cx, cy;            /* centre of the path */
    int32_t ring_x, ring_y;    /* top-left of the border objects, root coordinates */
    float left, top, right, bottom;
} border_t;

static void point(border_t *state, float x, float y)
{
    if (state->count >= PATH_POINTS)
    {
        return;
    }
    uint32_t index = state->count++;
    state->path[index] = (lv_point_precise_t){.x = lroundf(x), .y = lroundf(y)};
    if (index != 0)
    {
        float dx = state->path[index].x - state->path[index - 1].x;
        float dy = state->path[index].y - state->path[index - 1].y;
        state->distance[index] = state->distance[index - 1] + sqrtf(dx * dx + dy * dy);
    }
}

static void corner(border_t *state, float x, float y, float radius, int start)
{
    if (radius <= 0)
    {
        point(state, x, y);
        return;
    }
    for (int step = 0; step <= 8; step++)
    {
        float angle = (start + step * 90.0f / 8) * PI_F / 180;
        point(state, x + radius * cosf(angle), y + radius * sinf(angle));
    }
}

static void deleted(lv_event_t *event)
{
    lv_free(lv_event_get_user_data(event));
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
    return obj;
}

/* The stroke as a border: centred on the path, STROKE pixels wide. */
static lv_obj_t *ring(lv_obj_t *parent, const border_t *state, int32_t radius, uint32_t color, int32_t x, int32_t y)
{
    lv_obj_t *obj = plain(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, lroundf(state->right - state->left) + STROKE, lroundf(state->bottom - state->top) + STROKE);
    lv_obj_set_style_border_width(obj, STROKE, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

static lv_obj_t *dot(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *obj = plain(parent);
    lv_obj_set_size(obj, STROKE, STROKE);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

lv_obj_t *ui_border_progress_create(lv_obj_t *screen, uint32_t color)
{
    border_t *state = lv_malloc_zeroed(sizeof(*state));
    if (state == NULL)
    {
        return NULL;
    }
    lv_obj_t *root = plain(screen);
    lv_obj_set_user_data(root, state);
    lv_obj_add_event_cb(root, deleted, LV_EVENT_DELETE, state);
    lv_obj_set_size(root, ui_board_info()->width, ui_board_info()->height);
    lv_obj_set_ignore_layout(root, true);
    lv_obj_center(root);

    /* Coordinates address pixel centres: the last pixel is size - 1. */
    float width = ui_board_info()->width - 1, height = ui_board_info()->height - 1;
    const float inset = 5;
    int32_t ring_radius;
    if (ui_board_info()->round)
    {
        float radius = fminf(width, height) / 2 - inset;
        for (int i = 0; i <= 96; i++)
        {
            float angle = (i * 360.0f / 96 - 90) * PI_F / 180;
            point(state, width / 2 + radius * cosf(angle), height / 2 + radius * sinf(angle));
        }
        state->left = width / 2 - radius;
        state->right = width / 2 + radius;
        state->top = height / 2 - radius;
        state->bottom = height / 2 + radius;
        ring_radius = LV_RADIUS_CIRCLE;
    }
    else
    {
        float radius = fmaxf(0, ui_board_info()->display_corner_radius - inset);
        radius = fminf(radius, fminf(width, height) / 2 - inset);
        float left = inset, top = inset, right = width - inset, bottom = height - inset;
        point(state, width / 2, top);
        corner(state, right - radius, top + radius, radius, -90);
        corner(state, right - radius, bottom - radius, radius, 0);
        corner(state, left + radius, bottom - radius, radius, 90);
        corner(state, left + radius, top + radius, radius, 180);
        point(state, width / 2, top);
        state->left = left;
        state->right = right;
        state->top = top;
        state->bottom = bottom;
        ring_radius = lroundf(radius + STROKE / 2.0f);   /* outer edge of the round-capped line */
    }
    state->cx = lroundf(width / 2);
    state->cy = lroundf(height / 2);
    state->ring_x = lroundf(state->left) - STROKE / 2;
    state->ring_y = lroundf(state->top) - STROKE / 2;

    state->track = ring(root, state, ring_radius, TK_LINE, state->ring_x, state->ring_y);
    state->full = ring(root, state, ring_radius, color, state->ring_x, state->ring_y);
    for (int q = 0; q < 4; q++)
    {
        /* Objects clip their children: a window shows only its part of the ring. */
        state->window[q] = plain(root);
        state->window_ring[q] = ring(state->window[q], state, ring_radius, color, 0, 0);
        lv_obj_set_hidden(state->window[q], true);
    }
    state->dot_start = dot(root, color);
    state->dot_end = dot(root, color);
    lv_obj_set_pos(state->dot_start, (int32_t)state->path[0].x - STROKE / 2, (int32_t)state->path[0].y - STROKE / 2);
    state->value = -1;
    ui_border_progress_set(root, 0, 100);
    return root;
}

/* Show the window of quadrant q as the rectangle (x1, y1)-(x2, y2), root coordinates. */
static void window(border_t *state, int q, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    if (x2 < x1 || y2 < y1)
    {
        lv_obj_set_hidden(state->window[q], true);
        return;
    }
    lv_obj_set_pos(state->window[q], x1, y1);
    lv_obj_set_size(state->window[q], x2 - x1 + 1, y2 - y1 + 1);
    lv_obj_set_pos(state->window_ring[q], state->ring_x - x1, state->ring_y - y1);
    lv_obj_set_hidden(state->window[q], false);
}

void ui_border_progress_set(lv_obj_t *root, int32_t value, int32_t maximum)
{
    if (root == NULL)
    {
        return;
    }
    border_t *state = lv_obj_get_user_data(root);
    int32_t normalized = maximum > 0 ? (int32_t)((int64_t)value * 100 / maximum) : 0;
    normalized = normalized < 0 ? 0 : (normalized > 100 ? 100 : normalized);
    if (state->value == normalized)
    {
        return;
    }
    state->value = normalized;

    bool full = normalized == 100;
    lv_obj_set_hidden(state->full, !full);
    lv_obj_set_hidden(state->dot_start, normalized == 0 || full);
    lv_obj_set_hidden(state->dot_end, normalized == 0 || full);
    for (int q = 0; q < 4; q++)
    {
        lv_obj_set_hidden(state->window[q], true);
    }
    if (normalized == 0 || full)
    {
        return;
    }

    /* End point on the path, interpolated as the polyline version did. */
    float end = state->distance[state->count - 1] * normalized / 100;
    float ex = state->path[state->count - 1].x, ey = state->path[state->count - 1].y;
    for (uint32_t i = 1; i < state->count; i++)
    {
        if (state->distance[i] < end)
        {
            continue;
        }
        float segment = state->distance[i] - state->distance[i - 1];
        float ratio = segment > 0 ? (end - state->distance[i - 1]) / segment : 0;
        ex = state->path[i - 1].x + ratio * (state->path[i].x - state->path[i - 1].x);
        ey = state->path[i - 1].y + ratio * (state->path[i].y - state->path[i - 1].y);
        break;
    }
    int32_t px = lroundf(ex), py = lroundf(ey);
    lv_obj_set_pos(state->dot_end, px - STROKE / 2, py - STROKE / 2);

    /* Along a straight edge the cut is flat at the end point (the dot rounds
     * it); across the edge the window keeps the whole stroke width. */
    bool on_hor = fabsf(ey - state->top) < 0.5f || fabsf(ey - state->bottom) < 0.5f;
    bool on_ver = fabsf(ex - state->left) < 0.5f || fabsf(ex - state->right) < 0.5f;
    int32_t mx = on_hor && !on_ver ? 0 : HALF;
    int32_t my = on_ver && !on_hor ? 0 : HALF;

    int32_t w = lv_obj_get_width(root), h = lv_obj_get_height(root);
    int32_t cx = state->cx, cy = state->cy;
    int q_end = normalized * 4 / 100;   /* symmetric path: a quarter of it per quadrant */
    if (q_end > 3)
    {
        q_end = 3;
    }
    static const int8_t right_half[4] = {1, 1, 0, 0}, bottom_half[4] = {0, 1, 1, 0};
    for (int q = 0; q < q_end; q++)
    {
        window(state, q, right_half[q] ? cx : 0, bottom_half[q] ? cy : 0, right_half[q] ? w - 1 : cx - 1,
               bottom_half[q] ? h - 1 : cy - 1);
    }
    switch (q_end)
    {
    case 0:   /* top-right: x and y grow along the path */
        window(state, 0, cx, 0, px + mx, LV_MIN(py + my, cy - 1));
        break;
    case 1:   /* bottom-right: x shrinks, y grows */
        window(state, 1, LV_MAX(px - mx, cx), cy, w - 1, py + my);
        break;
    case 2:   /* bottom-left: x and y shrink */
        window(state, 2, px - mx, LV_MAX(py - my, cy), cx - 1, h - 1);
        break;
    default:  /* top-left: x grows, y shrinks */
        window(state, 3, 0, py - my, LV_MIN(px + mx, cx - 1), cy - 1);
        break;
    }
}

void ui_border_progress_color(lv_obj_t *root, uint32_t color)
{
    if (root == NULL)
    {
        return;
    }
    border_t *state = lv_obj_get_user_data(root);
    lv_color_t c = lv_color_hex(color);
    lv_obj_set_style_border_color(state->full, c, 0);
    for (int q = 0; q < 4; q++)
    {
        lv_obj_set_style_border_color(state->window_ring[q], c, 0);
    }
    lv_obj_set_style_bg_color(state->dot_start, c, 0);
    lv_obj_set_style_bg_color(state->dot_end, c, 0);
}
