/*
 * Names and trust shared by Sentinel (external) and Sentinel Lite (internal):
 * <storage>/sentinel/names.txt, one device per line, keyed by MAC:
 *
 *     aabbccddeeff<TAB>1<TAB>Telefono di Anna
 *
 * A name given in one app shows in the other. Plain text on purpose: no cJSON
 * tree, a few KB in PSRAM while it is read. Single header: include it in
 * exactly one .c of each app (the functions are static, unused ones are fine).
 */
#ifndef SENTINEL_NAMES_H
#define SENTINEL_NAMES_H

#include "services/storage.h"

#include "esp_heap_caps.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SN_NAMES_MAX 320
#define SN_NAME_LEN 65               /* the external app's labels; Lite keeps 31 bytes */

typedef struct
{
    uint8_t mac[6];
    bool trusted;
    char label[SN_NAME_LEN];
} sn_name_t;

static bool sn_names_path(char *out, size_t size, const char *suffix)
{
    const char *root = storage_dir();
    if (root == NULL)
    {
        return false;
    }
    char folder[48];
    snprintf(folder, sizeof(folder), "%s/sentinel", root);
    mkdir(folder, 0775);
    return snprintf(out, size, "%s/names.%s", folder, suffix) < (int)size;
}

/* Copy cut at a UTF-8 boundary, tabs and newlines turned into spaces. */
__attribute__((unused)) static void sn_names_copy(char *out, size_t size, const char *in)
{
    size_t n = 0;
    for (; in != NULL && in[n] && n < size - 1; n++)
    {
        out[n] = in[n] == '\t' || in[n] == '\n' || in[n] == '\r' ? ' ' : in[n];
    }
    while (n > 0 && in[n] && ((uint8_t)in[n] & 0xC0) == 0x80)
    {
        n--;   /* do not leave half a character */
    }
    out[n] = '\0';
}

/* The list in a PSRAM array (free() it), NULL without a file or memory. */
static sn_name_t *sn_names_load(int *count)
{
    *count = 0;
    char path[72];
    FILE *f = sn_names_path(path, sizeof(path), "txt") ? fopen(path, "r") : NULL;
    if (f == NULL)
    {
        return NULL;
    }
    sn_name_t *list = heap_caps_calloc(SN_NAMES_MAX, sizeof(sn_name_t), MALLOC_CAP_SPIRAM);
    char line[128];
    while (list != NULL && *count < SN_NAMES_MAX && fgets(line, sizeof(line), f) != NULL)
    {
        unsigned v[6];
        char flag;
        int used = 0;
        if (sscanf(line, "%2x%2x%2x%2x%2x%2x\t%c\t%n", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &flag, &used) < 7 ||
            used == 0)
        {
            continue;
        }
        sn_name_t *e = &list[*count];
        for (int i = 0; i < 6; i++)
        {
            e->mac[i] = (uint8_t)v[i];
        }
        e->trusted = flag == '1';
        line[strcspn(line, "\r\n")] = '\0';
        sn_names_copy(e->label, sizeof(e->label), line + used);
        (*count)++;
    }
    fclose(f);
    return list;
}

static int sn_names_find(const sn_name_t *list, int count, const uint8_t mac[6])
{
    for (int i = 0; list != NULL && i < count; i++)
    {
        if (memcmp(list[i].mac, mac, 6) == 0)
        {
            return i;
        }
    }
    return -1;
}

static bool sn_names_save(const sn_name_t *list, int count)
{
    char path[72], temp[72];
    if (!sn_names_path(path, sizeof(path), "txt") || !sn_names_path(temp, sizeof(temp), "tmp"))
    {
        return false;
    }
    FILE *f = fopen(temp, "w");
    if (f == NULL)
    {
        return false;
    }
    bool ok = true;
    for (int i = 0; i < count && ok; i++)
    {
        const uint8_t *m = list[i].mac;
        ok = fprintf(f, "%02x%02x%02x%02x%02x%02x\t%c\t%s\n", m[0], m[1], m[2], m[3], m[4], m[5],
                     list[i].trusted ? '1' : '0', list[i].label) > 0;
    }
    ok &= fclose(f) == 0;
    if (ok)
    {
        unlink(path);   /* FatFs cannot rename over a file */
        ok = rename(temp, path) == 0;
    }
    if (!ok)
    {
        unlink(temp);
    }
    return ok;
}

/* Set one entry in `list` (capacity SN_NAMES_MAX); no name and not trusted removes it. true if it changed. */
static bool sn_names_set(sn_name_t *list, int *count, const uint8_t mac[6], const char *label, bool trusted)
{
    int i = sn_names_find(list, *count, mac);
    char clean[SN_NAME_LEN];
    sn_names_copy(clean, sizeof(clean), label);
    if (clean[0] == '\0' && !trusted)
    {
        if (i < 0)
        {
            return false;
        }
        list[i] = list[--(*count)];
        return true;
    }
    if (i < 0)
    {
        if (*count == SN_NAMES_MAX)
        {
            return false;
        }
        i = (*count)++;
        memcpy(list[i].mac, mac, 6);
    }
    else if (list[i].trusted == trusted && strcmp(list[i].label, clean) == 0)
    {
        return false;
    }
    list[i].trusted = trusted;
    snprintf(list[i].label, sizeof(list[i].label), "%s", clean);
    return true;
}

/* Read, change one device, write back. */
__attribute__((unused)) static bool sn_names_put(const uint8_t mac[6], const char *label, bool trusted)
{
    int count = 0;
    sn_name_t *list = sn_names_load(&count);
    if (list == NULL)
    {
        list = heap_caps_calloc(SN_NAMES_MAX, sizeof(sn_name_t), MALLOC_CAP_SPIRAM);
        if (list == NULL)
        {
            return false;
        }
    }
    bool ok = !sn_names_set(list, &count, mac, label, trusted) || sn_names_save(list, count);
    free(list);
    return ok;
}

#endif
