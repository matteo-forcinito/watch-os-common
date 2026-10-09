#include "web_files.h"

#include "hardware/sdcard.h"

#include "cJSON.h"
#include "esp_log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "web_files";

#define PATH_MAX_LEN   200
#define QUERY_MAX      512
#define CHUNK          8192
#define MAX_DEPTH      12

/* ------------------------------------------------------------ helpers */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }

    c = (char)tolower((unsigned char)c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/* Decode %XX and '+' in place. */
static void url_decode(char *text)
{
    char *out = text;

    for (char *in = text; *in != '\0'; in++)
    {
        if (*in == '%' && hex_value(in[1]) >= 0 && hex_value(in[2]) >= 0)
        {
            *out++ = (char)(hex_value(in[1]) * 16 + hex_value(in[2]));
            in += 2;
        }
        else
        {
            *out++ = *in == '+' ? ' ' : *in;
        }
    }

    *out = '\0';
}

/*
 * Check a card path ("/config/wifi.txt") and build the real one
 * ("/sdcard/config/wifi.txt"). Refuses "..", backslashes and control
 * characters. A trailing '/' is removed.
 */
static bool make_path(const char *path, char *out, size_t size)
{
    size_t length = strlen(path);

    if (path[0] != '/' || length >= PATH_MAX_LEN)
    {
        return false;
    }

    for (const char *p = path; *p != '\0'; p++)
    {
        if ((unsigned char)*p < 0x20 || *p == '\\' || *p == ':' || *p == '*' || *p == '?' || *p == '"' ||
            *p == '<' || *p == '>' || *p == '|')
        {
            return false;
        }

        if (p[0] == '/' && p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == '\0'))
        {
            return false;
        }
    }

    int written = snprintf(out, size, SDCARD_MOUNT "%s", path);

    if (written <= 0 || (size_t)written >= size)
    {
        return false;
    }

    /* "/sdcard/x/" -> "/sdcard/x", "/sdcard/" -> "/sdcard" (the root is always "/sdcard"). */
    while (written > (int)strlen(SDCARD_MOUNT) && out[written - 1] == '/')
    {
        out[--written] = '\0';
    }

    return true;
}

/* Read query parameter `key` (decoded) into `out`. */
static bool query_param(httpd_req_t *req, const char *key, char *out, size_t size)
{
    char *query = malloc(QUERY_MAX);
    bool ok = false;

    if (query != NULL && httpd_req_get_url_query_str(req, query, QUERY_MAX) == ESP_OK &&
        httpd_query_key_value(query, key, out, size) == ESP_OK)
    {
        url_decode(out);
        ok = true;
    }

    free(query);

    return ok;
}

/* Query parameter `key` checked and turned into a real path. */
static bool query_path(httpd_req_t *req, const char *key, char *out, size_t size)
{
    char raw[PATH_MAX_LEN];
    return query_param(req, key, raw, sizeof(raw)) && make_path(raw, out, size);
}

static esp_err_t reply(httpd_req_t *req, bool ok, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", ok);

    if (message != NULL)
    {
        cJSON_AddStringToObject(json, "message", message);
    }

    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text != NULL ? text : "{}");
    cJSON_free(text);

    return err;
}

static bool card_ready(httpd_req_t *req)
{
    if (sdcard_is_mounted())
    {
        return true;
    }

    reply(req, false, "Scheda SD non disponibile");
    return false;
}

static const char *content_type(const char *path)
{
    const char *dot = strrchr(path, '.');

    if (dot == NULL)
    {
        return "application/octet-stream";
    }

    static const struct
    {
        const char *ext;
        const char *type;
    } types[] = {
        {".txt", "text/plain; charset=utf-8"}, {".ini", "text/plain; charset=utf-8"},
        {".json", "application/json"},         {".html", "text/html; charset=utf-8"},
        {".png", "image/png"},                 {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},               {".bmp", "image/bmp"},
        {".wav", "audio/wav"},                 {".mp3", "audio/mpeg"},
        {".csv", "text/csv; charset=utf-8"},   {".md", "text/plain; charset=utf-8"},
    };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
    {
        if (strcasecmp(dot, types[i].ext) == 0)
        {
            return types[i].type;
        }
    }

    return "application/octet-stream";
}

/* Delete a file or a folder with everything inside (`path` is modified and restored). */
static bool remove_tree(char *path, size_t size, int depth)
{
    struct stat st;

    if (stat(path, &st) != 0)
    {
        return false;
    }

    if (!S_ISDIR(st.st_mode))
    {
        return unlink(path) == 0;
    }

    if (depth > MAX_DEPTH)
    {
        return false;
    }

    DIR *dir = opendir(path);

    if (dir == NULL)
    {
        return false;
    }

    size_t base = strlen(path);
    bool ok = true;
    struct dirent *entry;

    while (ok && (entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        if (base + 1 + strlen(entry->d_name) + 1 > size)
        {
            ok = false;
            break;
        }

        snprintf(path + base, size - base, "/%s", entry->d_name);
        ok = remove_tree(path, size, depth + 1);
        path[base] = '\0';
    }

    closedir(dir);

    return ok && rmdir(path) == 0;
}

/* ----------------------------------------------------------- handlers */

static esp_err_t handle_list(httpd_req_t *req)
{
    char path[PATH_MAX_LEN + 16];
    char shown[PATH_MAX_LEN] = "/";

    if (!card_ready(req))
    {
        return ESP_OK;
    }

    query_param(req, "path", shown, sizeof(shown));

    if (!make_path(shown, path, sizeof(path)))
    {
        return reply(req, false, "Percorso non valido");
    }

    DIR *dir = opendir(path);

    if (dir == NULL)
    {
        return reply(req, false, "Cartella non trovata");
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddStringToObject(json, "path", shown);
    cJSON *entries = cJSON_AddArrayToObject(json, "entries");

    char *full = malloc(PATH_MAX_LEN + 300);
    struct dirent *entry;

    while (full != NULL && (entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }

        struct stat st = {0};
        snprintf(full, PATH_MAX_LEN + 300, "%s/%s", path, entry->d_name);
        stat(full, &st);

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "n", entry->d_name);
        cJSON_AddBoolToObject(item, "d", S_ISDIR(st.st_mode));
        cJSON_AddNumberToObject(item, "s", (double)st.st_size);
        cJSON_AddNumberToObject(item, "t", (double)st.st_mtime);
        cJSON_AddItemToArray(entries, item);
    }

    free(full);
    closedir(dir);

    uint32_t total_mb = 0;
    uint32_t free_mb = 0;
    sdcard_space(&total_mb, &free_mb);
    cJSON_AddNumberToObject(json, "total_mb", total_mb);
    cJSON_AddNumberToObject(json, "free_mb", free_mb);

    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text != NULL ? text : "{}");
    cJSON_free(text);

    return err;
}

static esp_err_t handle_get(httpd_req_t *req)
{
    char path[PATH_MAX_LEN + 16];
    char download[4] = "";

    if (!card_ready(req))
    {
        return ESP_OK;
    }

    if (!query_path(req, "path", path, sizeof(path)))
    {
        return reply(req, false, "Percorso non valido");
    }

    FILE *file = fopen(path, "rb");

    if (file == NULL)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File non trovato");
        return ESP_OK;
    }

    httpd_resp_set_type(req, content_type(path));

    char disposition[96];

    if (query_param(req, "dl", download, sizeof(download)))
    {
        const char *name = strrchr(path, '/');
        snprintf(disposition, sizeof(disposition), "attachment; filename=\"%.60s\"", name != NULL ? name + 1 : "file");
        httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    }

    char *buffer = malloc(CHUNK);
    esp_err_t err = buffer != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    size_t got;

    while (err == ESP_OK && (got = fread(buffer, 1, CHUNK, file)) > 0)
    {
        err = httpd_resp_send_chunk(req, buffer, (ssize_t)got);
    }

    fclose(file);
    free(buffer);

    if (err == ESP_OK)
    {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }

    return err;
}

static esp_err_t handle_put(httpd_req_t *req)
{
    char path[PATH_MAX_LEN + 16];
    char part[PATH_MAX_LEN + 24];

    if (!card_ready(req))
    {
        return ESP_OK;
    }

    if (!query_path(req, "path", path, sizeof(path)) || strcmp(path, SDCARD_MOUNT) == 0)
    {
        return reply(req, false, "Percorso non valido");
    }

    snprintf(part, sizeof(part), "%s.part", path);
    FILE *file = fopen(part, "wb");

    if (file == NULL)
    {
        return reply(req, false, "Impossibile scrivere (cartella esistente?)");
    }

    char *buffer = malloc(CHUNK);
    size_t left = req->content_len;
    bool ok = buffer != NULL;
    int timeouts = 0;

    while (ok && left > 0)
    {
        int n = httpd_req_recv(req, buffer, left < CHUNK ? left : CHUNK);

        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5)
        {
            continue;
        }

        ok = n > 0 && fwrite(buffer, 1, (size_t)n, file) == (size_t)n;
        left -= n > 0 ? (size_t)n : 0;
    }

    free(buffer);
    ok = (fclose(file) == 0) && ok;

    if (ok)
    {
        remove(path);   /* FAT cannot rename onto an existing file */
        ok = rename(part, path) == 0;
    }

    if (!ok)
    {
        remove(part);
    }

    ESP_LOGI(TAG, "PUT %s (%u bytes): %s", path, (unsigned)req->content_len, ok ? "ok" : "failed");

    return reply(req, ok, ok ? "Salvato" : "Scrittura interrotta");
}

static esp_err_t handle_mkdir(httpd_req_t *req)
{
    char path[PATH_MAX_LEN + 16];

    if (!card_ready(req))
    {
        return ESP_OK;
    }

    if (!query_path(req, "path", path, sizeof(path)))
    {
        return reply(req, false, "Percorso non valido");
    }

    bool ok = mkdir(path, 0775) == 0 || errno == EEXIST;

    return reply(req, ok, ok ? "Cartella creata" : "Impossibile creare la cartella");
}

static esp_err_t handle_rename(httpd_req_t *req)
{
    char from[PATH_MAX_LEN + 16];
    char to[PATH_MAX_LEN + 16];

    if (!card_ready(req))
    {
        return ESP_OK;
    }

    if (!query_path(req, "from", from, sizeof(from)) || !query_path(req, "to", to, sizeof(to)) ||
        strcmp(from, SDCARD_MOUNT) == 0)
    {
        return reply(req, false, "Percorso non valido");
    }

    struct stat st;

    if (stat(to, &st) == 0)
    {
        return reply(req, false, "Esiste già un file con quel nome");
    }

    bool ok = rename(from, to) == 0;

    return reply(req, ok, ok ? "Rinominato" : "Impossibile rinominare");
}

static esp_err_t handle_delete(httpd_req_t *req)
{
    char path[PATH_MAX_LEN + 300];

    if (!card_ready(req))
    {
        return ESP_OK;
    }

    if (!query_path(req, "path", path, sizeof(path)) || strcmp(path, SDCARD_MOUNT) == 0)
    {
        return reply(req, false, "Percorso non valido");
    }

    bool ok = remove_tree(path, sizeof(path), 0);
    ESP_LOGI(TAG, "DELETE %s: %s", path, ok ? "ok" : "failed");

    return reply(req, ok, ok ? "Eliminato" : "Impossibile eliminare");
}

void web_files_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/fs/list", .method = HTTP_GET, .handler = handle_list},
        {.uri = "/api/fs/get", .method = HTTP_GET, .handler = handle_get},
        {.uri = "/api/fs/put", .method = HTTP_POST, .handler = handle_put},
        {.uri = "/api/fs/mkdir", .method = HTTP_POST, .handler = handle_mkdir},
        {.uri = "/api/fs/rename", .method = HTTP_POST, .handler = handle_rename},
        {.uri = "/api/fs/delete", .method = HTTP_POST, .handler = handle_delete},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
    {
        httpd_register_uri_handler(server, &routes[i]);
    }
}
