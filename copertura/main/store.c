/*
 * Copertura data (see copertura.h): rooms, floors and measured spots in one
 * JSON file (copertura/data.json), saved a moment after a change by the probe
 * task; floor plan images as plan<N>.img next to it (uploaded from the web).
 */
#include "copertura.h"

#include "services/storage.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "copertura_store";

static cp_point_t *s_points;
static int s_point_count;
static char s_rooms[CP_MAX_ROOMS][CP_ROOM];
static int s_room_count;
static char s_floors[CP_MAX_FLOORS][CP_ROOM];
static int s_floor_count;
static uint32_t s_next_id = 1;
static uint32_t s_revision;
static bool s_dirty;
static int64_t s_dirty_since;

static const char *const DEFAULT_ROOMS[] = {"Soggiorno", "Cucina", "Camera", "Cameretta", "Studio", "Bagno",
                                            "Ingresso", "Corridoio", "Balcone", "Garage", "Giardino"};

uint32_t cp_revision(void)
{
    return __atomic_load_n(&s_revision, __ATOMIC_RELAXED);
}

/* Locked. */
void cp_changed(bool save)
{
    __atomic_add_fetch(&s_revision, 1, __ATOMIC_RELAXED);
    if (save && !s_dirty)
    {
        s_dirty = true;
        s_dirty_since = time(NULL);
    }
}

const cp_point_t *cp_points(int *count)
{
    *count = s_point_count;
    return s_points;
}

uint32_t cp_point_add(const cp_point_t *point)
{
    cp_lock();
    if (s_point_count == CP_MAX_POINTS)
    {
        memmove(s_points, s_points + 1, sizeof(cp_point_t) * (CP_MAX_POINTS - 1));   /* the oldest goes */
        s_point_count--;
    }
    cp_point_t *p = &s_points[s_point_count++];
    *p = *point;
    p->id = s_next_id++;
    uint32_t id = p->id;
    cp_changed(true);
    cp_unlock();
    return id;
}

bool cp_point_place(uint32_t id, int x, int y, int floor)
{
    cp_lock();
    bool found = false;
    for (int i = 0; i < s_point_count; i++)
    {
        if (s_points[i].id == id)
        {
            s_points[i].x = (int16_t)(x < -1 ? -1 : x > 1000 ? 1000 : x);
            s_points[i].y = (int16_t)(y < -1 ? -1 : y > 1000 ? 1000 : y);
            if (floor >= 0 && floor < CP_MAX_FLOORS)
            {
                s_points[i].floor = (uint8_t)floor;
            }
            found = true;
        }
    }
    if (found)
    {
        cp_changed(true);
    }
    cp_unlock();
    return found;
}

bool cp_point_delete(uint32_t id)
{
    cp_lock();
    bool found = false;
    for (int i = 0; i < s_point_count; i++)
    {
        if (s_points[i].id == id)
        {
            memmove(&s_points[i], &s_points[i + 1], sizeof(cp_point_t) * (s_point_count - i - 1));
            s_point_count--;
            found = true;
            break;
        }
    }
    if (found)
    {
        cp_changed(true);
    }
    cp_unlock();
    return found;
}

void cp_points_clear(void)
{
    cp_lock();
    s_point_count = 0;
    cp_changed(true);
    cp_unlock();
}

int cp_rooms(char (*out)[CP_ROOM], int max)
{
    cp_lock();
    int n = s_room_count < max ? s_room_count : max;
    memcpy(out, s_rooms, sizeof(s_rooms[0]) * n);
    cp_unlock();
    return n;
}

void cp_rooms_set(const char (*rooms)[CP_ROOM], int count)
{
    cp_lock();
    s_room_count = 0;
    for (int i = 0; i < count && s_room_count < CP_MAX_ROOMS; i++)
    {
        if (rooms[i][0] != '\0')
        {
            snprintf(s_rooms[s_room_count++], CP_ROOM, "%s", rooms[i]);
        }
    }
    cp_changed(true);
    cp_unlock();
}

int cp_floors(char (*out)[CP_ROOM], int max)
{
    cp_lock();
    int n = s_floor_count < max ? s_floor_count : max;
    memcpy(out, s_floors, sizeof(s_floors[0]) * n);
    cp_unlock();
    return n;
}

void cp_floors_set(const char (*floors)[CP_ROOM], int count)
{
    cp_lock();
    s_floor_count = 0;
    for (int i = 0; i < count && s_floor_count < CP_MAX_FLOORS; i++)
    {
        snprintf(s_floors[s_floor_count++], CP_ROOM, "%s", floors[i][0] ? floors[i] : "Piano");
    }
    if (s_floor_count == 0)
    {
        snprintf(s_floors[s_floor_count++], CP_ROOM, "Casa");
    }
    cp_changed(true);
    cp_unlock();
}

static bool path_of(const char *name, char *out, size_t size)
{
    const char *root = storage_dir();
    if (root == NULL)
    {
        return false;
    }
    char folder[48];
    snprintf(folder, sizeof(folder), "%s/copertura", root);
    mkdir(folder, 0775);
    snprintf(out, size, "%s/%s", folder, name);
    return true;
}

bool cp_plan_path(int floor, char *out, size_t size)
{
    char name[16];
    snprintf(name, sizeof(name), "plan%d.img", floor);
    return floor >= 0 && floor < CP_MAX_FLOORS && path_of(name, out, size);
}

/* ------------------------------------------------------------ persistence */

static char *to_text(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddNumberToObject(root, "next_id", s_next_id);
    cJSON *rooms = cJSON_AddArrayToObject(root, "rooms");
    for (int i = 0; i < s_room_count; i++)
    {
        cJSON_AddItemToArray(rooms, cJSON_CreateString(s_rooms[i]));
    }
    cJSON *floors = cJSON_AddArrayToObject(root, "floors");
    for (int i = 0; i < s_floor_count; i++)
    {
        cJSON_AddItemToArray(floors, cJSON_CreateString(s_floors[i]));
    }
    cJSON *points = cJSON_AddArrayToObject(root, "points");
    for (int i = 0; i < s_point_count; i++)
    {
        const cp_point_t *p = &s_points[i];
        cJSON *j = cJSON_CreateObject();
        char bssid[18];
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x", p->bssid[0], p->bssid[1], p->bssid[2],
                 p->bssid[3], p->bssid[4], p->bssid[5]);
        cJSON_AddNumberToObject(j, "id", p->id);
        cJSON_AddStringToObject(j, "room", p->room);
        cJSON_AddNumberToObject(j, "floor", p->floor);
        cJSON_AddNumberToObject(j, "x", p->x);
        cJSON_AddNumberToObject(j, "y", p->y);
        cJSON_AddNumberToObject(j, "rssi", p->rssi);
        cJSON_AddNumberToObject(j, "min", p->rssi_min);
        cJSON_AddNumberToObject(j, "max", p->rssi_max);
        cJSON_AddNumberToObject(j, "ch", p->channel);
        cJSON_AddStringToObject(j, "bssid", bssid);
        cJSON_AddNumberToObject(j, "ping", p->ping);
        cJSON_AddNumberToObject(j, "ping_max", p->ping_max);
        cJSON_AddNumberToObject(j, "loss", p->loss);
        cJSON_AddNumberToObject(j, "radios", p->radios);
        cJSON_AddNumberToObject(j, "alt", p->alt_rssi);
        cJSON_AddNumberToObject(j, "when", (double)p->when);
        cJSON_AddItemToArray(points, j);
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

static double num(const cJSON *o, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

void cp_store_load(void)
{
    s_points = heap_caps_calloc(CP_MAX_POINTS, sizeof(cp_point_t), MALLOC_CAP_SPIRAM);
    for (size_t i = 0; i < sizeof(DEFAULT_ROOMS) / sizeof(DEFAULT_ROOMS[0]); i++)
    {
        snprintf(s_rooms[s_room_count++], CP_ROOM, "%s", DEFAULT_ROOMS[i]);
    }
    snprintf(s_floors[0], CP_ROOM, "Casa");
    s_floor_count = 1;

    char path[80];
    if (s_points == NULL || !path_of("data.json", path, sizeof(path)) || !storage_files_acquire())
    {
        return;
    }
    FILE *file = fopen(path, "rb");
    char *text = NULL;
    if (file != NULL)
    {
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        text = size > 0 && size < 256 * 1024 ? heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM) : NULL;
        if (text != NULL && fread(text, 1, size, file) == (size_t)size)
        {
            text[size] = '\0';
        }
        else
        {
            free(text);
            text = NULL;
        }
        fclose(file);
    }
    storage_files_release();
    cJSON *root = text != NULL ? cJSON_Parse(text) : NULL;
    free(text);
    if (root == NULL)
    {
        return;
    }
    s_next_id = (uint32_t)num(root, "next_id", 1);
    const cJSON *item;
    const cJSON *rooms = cJSON_GetObjectItemCaseSensitive(root, "rooms");
    if (cJSON_IsArray(rooms) && cJSON_GetArraySize(rooms) > 0)
    {
        s_room_count = 0;
        cJSON_ArrayForEach(item, rooms)
        {
            if (cJSON_IsString(item) && s_room_count < CP_MAX_ROOMS)
            {
                snprintf(s_rooms[s_room_count++], CP_ROOM, "%s", item->valuestring);
            }
        }
    }
    const cJSON *floors = cJSON_GetObjectItemCaseSensitive(root, "floors");
    if (cJSON_IsArray(floors) && cJSON_GetArraySize(floors) > 0)
    {
        s_floor_count = 0;
        cJSON_ArrayForEach(item, floors)
        {
            if (cJSON_IsString(item) && s_floor_count < CP_MAX_FLOORS)
            {
                snprintf(s_floors[s_floor_count++], CP_ROOM, "%s", item->valuestring);
            }
        }
    }
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "points"))
    {
        if (s_point_count == CP_MAX_POINTS)
        {
            break;
        }
        cp_point_t *p = &s_points[s_point_count++];
        memset(p, 0, sizeof(*p));
        p->id = (uint32_t)num(item, "id", 0);
        const cJSON *room = cJSON_GetObjectItemCaseSensitive(item, "room");
        snprintf(p->room, sizeof(p->room), "%s", cJSON_IsString(room) ? room->valuestring : "?");
        p->floor = (uint8_t)num(item, "floor", 0);
        p->x = (int16_t)num(item, "x", -1);
        p->y = (int16_t)num(item, "y", -1);
        p->rssi = (int8_t)num(item, "rssi", -90);
        p->rssi_min = (int8_t)num(item, "min", p->rssi);
        p->rssi_max = (int8_t)num(item, "max", p->rssi);
        p->channel = (uint8_t)num(item, "ch", 0);
        const cJSON *bssid = cJSON_GetObjectItemCaseSensitive(item, "bssid");
        if (cJSON_IsString(bssid))
        {
            unsigned b[6];
            if (sscanf(bssid->valuestring, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6)
            {
                for (int k = 0; k < 6; k++)
                {
                    p->bssid[k] = (uint8_t)b[k];
                }
            }
        }
        p->ping = (uint16_t)num(item, "ping", 0);
        p->ping_max = (uint16_t)num(item, "ping_max", 0);
        p->loss = (uint8_t)num(item, "loss", 0);
        p->radios = (uint8_t)num(item, "radios", 0);
        p->alt_rssi = (int8_t)num(item, "alt", 0);
        p->when = (int64_t)num(item, "when", 0);
        if (p->id >= s_next_id)
        {
            s_next_id = p->id + 1;
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "%d spots, %d rooms, %d floors", s_point_count, s_room_count, s_floor_count);
}

/* Probe task: save 2 s after the last change. */
void cp_store_flush(void)
{
    cp_lock();
    char *text = NULL;
    if (s_dirty && time(NULL) - s_dirty_since >= 2)
    {
        text = to_text();
        s_dirty = false;
    }
    cp_unlock();
    if (text == NULL)
    {
        return;
    }
    char path[80], temporary[88];
    if (path_of("data.json", path, sizeof(path)) && storage_files_acquire())
    {
        snprintf(temporary, sizeof(temporary), "%s.new", path);
        FILE *file = fopen(temporary, "wb");
        bool ok = file != NULL && fwrite(text, 1, strlen(text), file) == strlen(text);
        if (file != NULL)
        {
            ok = fclose(file) == 0 && ok;
        }
        if (ok)
        {
            unlink(path);
            ok = rename(temporary, path) == 0;
        }
        storage_files_release();
        if (!ok)
        {
            cp_lock();
            cp_changed(true);
            cp_unlock();
        }
    }
    cJSON_free(text);
}
