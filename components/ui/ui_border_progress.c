/* A passive perimeter progress indicator: no input device, timer or task.
 * Geometry is cached per instance; caller updates it under the LVGL lock.
 * Points follow the actual perimeter length, including rectangular corners. */
#include "ui/ui.h"
#include "hardware/board.h"
#include <math.h>
#include <string.h>

#define PATH_POINTS 101
#define PI_F 3.14159265358979323846f

typedef struct
{
    lv_obj_t *track;
    lv_obj_t *indicator;
    lv_point_precise_t path[PATH_POINTS];
    lv_point_precise_t active[PATH_POINTS + 1];
    float distance[PATH_POINTS];
    uint32_t count;
    int32_t value;
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

lv_obj_t *ui_border_progress_create(lv_obj_t *screen, uint32_t color)
{
    border_t *state = lv_malloc_zeroed(sizeof(*state));
    if (state == NULL)
    {
        return NULL;
    }
    lv_obj_t *root = lv_obj_create(screen);
    lv_obj_remove_style_all(root);
    lv_obj_set_user_data(root, state);
    lv_obj_add_event_cb(root, deleted, LV_EVENT_DELETE, state);
    lv_obj_set_size(root, ui_board_info()->width, ui_board_info()->height);
    lv_obj_set_ignore_layout(root, true);
    lv_obj_center(root);
    lv_obj_set_clickable(root, false);
    lv_obj_set_scrollable(root, false);

    /* Coordinates address pixel centres: the last pixel is size - 1. */
    float width = ui_board_info()->width - 1, height = ui_board_info()->height - 1;
    const float inset = 5;
    if (ui_board_info()->round)
    {
        float radius = fminf(width, height) / 2 - inset;
        for (int i = 0; i <= 96; i++)
        {
            float angle = (i * 360.0f / 96 - 90) * PI_F / 180;
            point(state, width / 2 + radius * cosf(angle), height / 2 + radius * sinf(angle));
        }
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
    }
    state->track = lv_line_create(root);
    state->indicator = lv_line_create(root);
    lv_obj_t *lines[] = {state->track, state->indicator};
    for (int i = 0; i < 2; i++)
    {
        /* Do not inherit theme padding or a position offset at the panel edge. */
        lv_obj_remove_style_all(lines[i]);
        lv_obj_set_pos(lines[i], 0, 0);
        lv_obj_set_clickable(lines[i], false);
        lv_obj_set_scrollable(lines[i], false);
        lv_obj_set_style_line_width(lines[i], 5, 0);
        lv_obj_set_style_line_rounded(lines[i], true, 0);
    }
    lv_obj_set_style_line_color(state->track, lv_color_hex(UI_COLOR_CARD_HI), 0);
    lv_obj_set_style_line_color(state->indicator, lv_color_hex(color), 0);
    lv_line_set_points(state->track, state->path, state->count);
    state->value = -1;
    ui_border_progress_set(root, 0, 100);
    return root;
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
    lv_obj_set_hidden(state->indicator, normalized == 0);
    if (normalized == 0)
    {
        return;
    }
    float end = state->distance[state->count - 1] * normalized / 100;
    uint32_t count = 0;
    for (uint32_t i = 0; i < state->count; i++)
    {
        if (state->distance[i] <= end)
        {
            state->active[count++] = state->path[i];
            continue;
        }
        float segment = state->distance[i] - state->distance[i - 1];
        float ratio = segment > 0 ? (end - state->distance[i - 1]) / segment : 0;
        state->active[count++] = (lv_point_precise_t){
            .x = lroundf(state->path[i - 1].x + ratio * (state->path[i].x - state->path[i - 1].x)),
            .y = lroundf(state->path[i - 1].y + ratio * (state->path[i].y - state->path[i - 1].y)),
        };
        break;
    }
    lv_line_set_points(state->indicator, state->active, count);
}

void ui_border_progress_color(lv_obj_t *root, uint32_t color)
{
    if (root != NULL)
    {
        border_t *state = lv_obj_get_user_data(root);
        lv_obj_set_style_line_color(state->indicator, lv_color_hex(color), 0);
    }
}
