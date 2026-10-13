/* Two independent, checksummed SD snapshots. A torn save leaves the other valid. */
#include "sentinel.h"
#include "packet.h"
#include "services/storage.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>

static atomic_bool s_corrupt;
static const char *s_folder = "sentinel";
bool sn_store_corrupt(void) { return s_corrupt; }

static bool identifier(const char *text)
{
    if (text == NULL || strlen(text) != 32) return false;
    for (int i = 0; i < 32; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
}

static bool text_field(cJSON *object, const char *key, char *out, size_t capacity)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || !sn_text_valid(value->valuestring, capacity - 1)) return false;
    memcpy(out, value->valuestring, strlen(value->valuestring) + 1);
    return true;
}

static bool optional_text(cJSON *object, const char *key, char *out, size_t capacity)
{
    out[0] = 0;
    return cJSON_GetObjectItemCaseSensitive(object, key) == NULL || text_field(object, key, out, capacity);
}

static bool number(cJSON *object, const char *key, uint64_t maximum, uint64_t *out)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < 0 ||
        value->valuedouble > (double)maximum || floor(value->valuedouble) != value->valuedouble) return false;
    *out = (uint64_t)value->valuedouble;
    return true;
}

static bool ip_valid(const char *text)
{
    struct in_addr ip;
    if (inet_pton(AF_INET, text, &ip) != 1) return false;
    uint32_t host = ntohl(ip.s_addr);
    return host != 0 && host != UINT32_MAX && (host >> 24) != 127 && (host >> 28) < 14;
}

static bool mac_valid(const char *text)
{
    if (*text == 0) return true;
    if (strlen(text) != 17) return false;
    for (int i = 0; i < 17; i++)
    {
        if (i % 3 == 2) { if (text[i] != ':') return false; }
        else if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    }
    return true;
}

cJSON *sn_device_json(const sn_device_t *d)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) return NULL;
#define S(key, member) if (!cJSON_AddStringToObject(o, key, d->member)) goto failure
    S("id", id); S("network", network); S("ip", ip); S("mac", mac); S("hostname", hostname);
    S("services", services); S("label", label); S("category", category); S("note", note);
    S("dns", dns); S("netbios", netbios); S("model", model);
#undef S
    if (!cJSON_AddNumberToObject(o, "first_seen", d->first_seen) ||
        !cJSON_AddNumberToObject(o, "last_seen", d->last_seen) ||
        !cJSON_AddNumberToObject(o, "sources", d->sources) ||
        !cJSON_AddNumberToObject(o, "seen_cycle", d->seen_cycle) ||
        !cJSON_AddNumberToObject(o, "rtt_ms", d->rtt_ms) ||
        !cJSON_AddBoolToObject(o, "trusted", d->trusted)) goto failure;
    return o;
failure:
    cJSON_Delete(o);
    return NULL;
}

cJSON *sn_network_json(const sn_network_t *n)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) return NULL;
#define S(key, member) if (!cJSON_AddStringToObject(o, key, n->member)) goto failure
    S("id", id); S("label", label); S("ssid", ssid); S("subnet", subnet);
    S("gateway", gateway); S("gateway_mac", gateway_mac);
#undef S
    if (!cJSON_AddNumberToObject(o, "last_seen", n->last_seen)) goto failure;
    return o;
failure:
    cJSON_Delete(o);
    return NULL;
}

static char *serialize(const sn_model_t *model)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    cJSON *devices = cJSON_AddArrayToObject(root, "devices");
    bool ok = networks && devices && cJSON_AddNumberToObject(root, "schema", 1) &&
              cJSON_AddNumberToObject(root, "generation", model->generation) &&
              cJSON_AddNumberToObject(root, "interval", model->interval);
    for (int i = 0; ok && i < model->network_count; i++)
    {
        cJSON *item = sn_network_json(&model->networks[i]);
        ok = item != NULL && cJSON_AddItemToArray(networks, item);
        if (!ok) cJSON_Delete(item);
    }
    for (int i = 0; ok && i < model->device_count; i++)
    {
        cJSON *item = sn_device_json(&model->devices[i]);
        ok = item != NULL && cJSON_AddItemToArray(devices, item);
        if (!ok) cJSON_Delete(item);
    }
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (json != NULL && strlen(json) > SN_FILE_LIMIT) { free(json); json = NULL; }
    return json;
}

char *sn_export(void)
{
    sn_model_t *copy = heap_caps_malloc(sizeof(*copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) return NULL;
    xSemaphoreTake(sn_mutex, portMAX_DELAY);
    *copy = *sn_model;
    xSemaphoreGive(sn_mutex);
    char *json = serialize(copy);
    free(copy);
    return json;
}

sn_model_t *sn_parse(const char *json, size_t length, char *error, size_t capacity)
{
    snprintf(error, capacity, "Archivio non valido o incompatibile");
    if (json == NULL || length == 0 || length > SN_FILE_LIMIT || strlen(json) != length || !sn_json_safe(json,length)) return NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length + 1, NULL, true);
    sn_model_t *model = heap_caps_calloc(1, sizeof(*model), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (root == NULL || model == NULL) goto failure;
    uint64_t value;
    if (!number(root, "schema", 1, &value) || value != 1 ||
        !number(root, "generation", UINT32_MAX, &value)) goto failure;
    model->generation = value;
    if (!number(root, "interval", 3600, &value) || (value != 0 && value < 30)) goto failure;
    model->interval = value;
    cJSON *networks = cJSON_GetObjectItemCaseSensitive(root, "networks");
    cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!cJSON_IsArray(networks) || !cJSON_IsArray(devices) ||
        cJSON_GetArraySize(networks) > SN_NETWORKS || cJSON_GetArraySize(devices) > SN_DEVICES) goto failure;
    cJSON *item;
    cJSON_ArrayForEach(item, networks)
    {
        sn_network_t *n = &model->networks[model->network_count];
        if (!text_field(item,"id",n->id,sizeof(n->id)) || !identifier(n->id) ||
            !text_field(item,"label",n->label,sizeof(n->label)) ||
            !text_field(item,"ssid",n->ssid,sizeof(n->ssid)) ||
            !text_field(item,"subnet",n->subnet,sizeof(n->subnet)) ||
            !text_field(item,"gateway",n->gateway,sizeof(n->gateway)) || !ip_valid(n->gateway) ||
            !text_field(item,"gateway_mac",n->gateway_mac,sizeof(n->gateway_mac)) || !mac_valid(n->gateway_mac) ||
            !number(item,"last_seen",4102444800ULL,&value)) goto failure;
        n->last_seen = value;
        for (int j = 0; j < model->network_count; j++)
            if (strcmp(n->id, model->networks[j].id) == 0) goto failure;
        model->network_count++;
    }
    cJSON_ArrayForEach(item, devices)
    {
        sn_device_t *d = &model->devices[model->device_count];
#define T(key, member) if (!text_field(item, key, d->member, sizeof(d->member))) goto failure
        T("id",id); T("network",network); T("ip",ip); T("mac",mac); T("hostname",hostname);
        T("services",services); T("label",label); T("category",category); T("note",note);
#undef T
        if (!identifier(d->id) || !identifier(d->network) || !ip_valid(d->ip) || !mac_valid(d->mac)) goto failure;
        bool network_exists = false;
        for (int j = 0; j < model->network_count; j++)
            if (strcmp(d->network, model->networks[j].id) == 0) network_exists = true;
        if (!network_exists) goto failure;
        for (int j = 0; j < model->device_count; j++)
            if (strcmp(d->id, model->devices[j].id) == 0) goto failure;
        if (!number(item,"first_seen",4102444800ULL,&value)) goto failure;
        d->first_seen = value;
        if (!number(item,"last_seen",4102444800ULL,&value)) goto failure;
        d->last_seen = value;
        if (d->last_seen < d->first_seen || !number(item,"sources",63,&value)) goto failure;
        d->sources = value;
        cJSON *trusted = cJSON_GetObjectItemCaseSensitive(item,"trusted");
        if (!cJSON_IsBool(trusted)) goto failure;
        d->trusted = cJSON_IsTrue(trusted);
        cJSON *rtt = cJSON_GetObjectItemCaseSensitive(item,"rtt_ms");
        if (!cJSON_IsNumber(rtt) || rtt->valuedouble < -1 || rtt->valuedouble > 60000 ||
            floor(rtt->valuedouble) != rtt->valuedouble) goto failure;
        d->rtt_ms = (int)rtt->valuedouble;
        /* Identification fields arrived with Sentinel 0.2: optional for older archives. */
        if (!optional_text(item,"dns",d->dns,sizeof(d->dns)) ||
            !optional_text(item,"netbios",d->netbios,sizeof(d->netbios)) ||
            !optional_text(item,"model",d->model,sizeof(d->model))) goto failure;
        /* Online observations never survive restart/import as current presence. */
        d->seen_cycle = 0;
        model->device_count++;
    }
    cJSON_Delete(root);
    *error = 0;
    return model;
failure:
    cJSON_Delete(root);
    free(model);
    return NULL;
}

static bool checksum(const char *data, char out[65])
{
    unsigned char digest[32];
    if (!sn_digest(data,strlen(data),digest)) return false;
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
    return true;
}

static bool path(char *out, size_t capacity, int slot, const char *suffix)
{
    const char *directory = storage_dir();
    if (directory == NULL) return false;
    char folder[96];
    snprintf(folder, sizeof(folder), "%s/%s", directory, s_folder);
    struct stat info;
    if (stat(folder, &info) != 0 && mkdir(folder, 0775) != 0) return false;
    return snprintf(out, capacity, "%s/state-%d.%s", folder, slot, suffix) < (int)capacity;
}

static bool write_atomic(const char *filename, const char *data)
{
    char temporary[144];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", filename) >= (int)sizeof(temporary)) return false;
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) return false;
    size_t length = strlen(data);
    bool ok = fwrite(data, 1, length, file) == length && fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    /* FatFs cannot replace an existing destination. Only the older slot is
     * replaced; the other checksummed snapshot stays intact throughout. */
    if (ok && (unlink(filename) == 0 || errno == ENOENT) && rename(temporary, filename) == 0) return true;
    /* Only the exact temporary file owned by this app is removed. */
    unlink(temporary);
    return false;
}

static bool store_save(const sn_model_t *snapshot, bool restore)
{
    if (s_corrupt && !restore) return false;
    if (!storage_files_acquire()) return false;
    char filename[128], hashfile[128];
    bool ok = path(filename,sizeof(filename),snapshot->generation & 1,"json") &&
              path(hashfile,sizeof(hashfile),snapshot->generation & 1,"sha256");
    char *json = ok ? serialize(snapshot) : NULL;
    if (json == NULL) ok = false;
    char hash[65];
    if (ok)
    {
        ok = checksum(json, hash) && write_atomic(filename, json) && write_atomic(hashfile, hash);
    }
    free(json);
    storage_files_release();
    if (ok && restore) s_corrupt = false;
    return ok;
}

bool sn_store_save(const sn_model_t *snapshot) { return store_save(snapshot,false); }
/* Only explicit, validated, confirmed imports may recover an invalid archive. */
bool sn_store_restore(const sn_model_t *snapshot) { return store_save(snapshot,true); }

bool sn_store_load(sn_model_t *out)
{
    if (!storage_files_acquire()) return false;
    bool found = false, present = false;
    for (int slot = 0; slot < 2; slot++)
    {
        char filename[128], hashfile[128];
        struct stat info;
        if (!path(filename,sizeof(filename),slot,"json") || !path(hashfile,sizeof(hashfile),slot,"sha256")) continue;
        struct stat hashinfo;
        bool json_present = stat(filename,&info) == 0;
        present |= json_present || stat(hashfile,&hashinfo) == 0;
        if (!json_present || info.st_size < 2 || info.st_size > SN_FILE_LIMIT) continue;
        char *json = heap_caps_malloc(info.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (json == NULL) continue;
        FILE *file = fopen(filename,"rb");
        bool ok = file != NULL && fread(json,1,info.st_size,file) == (size_t)info.st_size;
        if (file != NULL) fclose(file);
        json[info.st_size] = 0;
        char expected[66] = {0}, actual[65];
        file = fopen(hashfile,"rb");
        ok = ok && file != NULL;
        if (file != NULL) { ok = fread(expected,1,65,file) == 64 && ok; fclose(file); }
        if (ok) ok = checksum(json,actual) && strcmp(expected,actual) == 0;
        char error[64];
        sn_model_t *candidate = ok ? sn_parse(json,info.st_size,error,sizeof(error)) : NULL;
        if (candidate != NULL && (!found || candidate->generation > out->generation))
        {
            *out = *candidate;
            found = true;
        }
        free(candidate);
        free(json);
    }
    storage_files_release();
    s_corrupt = present && !found;
    return found;
}

#if CONFIG_WATCH_DEV_CONSOLE
/* Uses an exclusively created random folder. Never modifies live inventory. */
bool sn_store_selftest(void)
{
    const char *base = storage_dir();
    if (!base || !storage_files_acquire()) return false;
    char name[40], folder[96];
    snprintf(name,sizeof(name),"sentinel-test-%08lx",(unsigned long)esp_random());
    snprintf(folder,sizeof(folder),"%s/%s",base,name);
    bool created = mkdir(folder,0775) == 0;
    storage_files_release();
    if (!created) return false;
    const char *old_folder = s_folder;
    bool old_corrupt = s_corrupt;
    s_folder = name; s_corrupt = false;
    sn_model_t *fixture = heap_caps_calloc(1,sizeof(*fixture),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    sn_model_t *loaded = heap_caps_calloc(1,sizeof(*loaded),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = fixture && loaded;
    char *json = NULL;
    if (ok)
    {
        fixture->network_count = 1; fixture->device_count = SN_DEVICES;
        snprintf(fixture->networks[0].id,33,"%032x",1);
        snprintf(fixture->networks[0].label,65,"Test isolato");
        snprintf(fixture->networks[0].ssid,33,"Fixture");
        snprintf(fixture->networks[0].gateway,16,"10.31.0.1");
        snprintf(fixture->networks[0].subnet,24,"10.31.0.0/22");
        for (int i = 0; i < SN_DEVICES; i++)
        {
            sn_device_t *d = &fixture->devices[i];
            snprintf(d->id,33,"%032x",i+1); snprintf(d->network,33,"%032x",1);
            snprintf(d->ip,16,"10.31.%d.%d",i/254,i%254+1);
            snprintf(d->mac,18,"02:00:00:00:%02x:%02x",i/256,i%256);
            snprintf(d->label,65,"Dispositivo %d - caffè",i);
            snprintf(d->category,33,"Casa"); d->rtt_ms = -1;
        }
        fixture->generation = 1;
        json = serialize(fixture);
        char error[80];
        sn_model_t *parsed = json ? sn_parse(json,strlen(json),error,sizeof(error)) : NULL;
        ok = parsed && parsed->device_count == SN_DEVICES &&
             strcmp(parsed->devices[SN_DEVICES-1].label,fixture->devices[SN_DEVICES-1].label) == 0;
        free(parsed);
        free(json); json = NULL;
    }
    if (ok) ok = sn_store_save(fixture);
    if (ok)
    {
        fixture->generation = 2;
        snprintf(fixture->devices[0].label,65,"Nuova generazione");
        ok = sn_store_save(fixture) && sn_store_load(loaded) && loaded->generation == 2;
    }
    if (ok && storage_files_acquire())
    {
        char file[128];
        ok = path(file,sizeof(file),0,"json") && write_atomic(file,"{\"torn\":true}");
        storage_files_release();
    }
    else ok = false;
    if (ok) ok = sn_store_load(loaded) && loaded->generation == 1 && loaded->device_count == SN_DEVICES;
    if (ok)
    {
        snprintf(fixture->devices[1].id,33,"%s",fixture->devices[0].id);
        json = serialize(fixture);
        char error[80];
        sn_model_t *invalid = json ? sn_parse(json,strlen(json),error,sizeof(error)) : NULL;
        ok = json && !invalid;
        free(invalid); free(json);
    }
    free(fixture); free(loaded);
    bool clean = storage_files_acquire();
    if (clean)
    {
        const char *suffixes[] = {"json","sha256","json.tmp","sha256.tmp"};
        for (int slot = 0; slot < 2; slot++)
            for (unsigned i = 0; i < 4; i++)
            {
                char file[128];
                if (!path(file,sizeof(file),slot,suffixes[i]) || (unlink(file) != 0 && errno != ENOENT)) clean = false;
            }
        if (rmdir(folder) != 0) clean = false;
        storage_files_release();
    }
    s_folder = old_folder; s_corrupt = old_corrupt;
    return ok && clean;
}
#endif
