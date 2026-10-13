/* Sentinel: bounded local-network inventory, all slow work in one worker. */
#ifndef SENTINEL_H
#define SENTINEL_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "lvgl.h"
#include "cJSON.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SN_DEVICES 1024
#define SN_NETWORKS 16
#define SN_FILE_LIMIT (768 * 1024)
#define SN_ARP 1
#define SN_ICMP 2
#define SN_MDNS 4
#define SN_SSDP 8
#define SN_TCP 16
#define SN_PROXY 32

typedef struct
{
    char id[33], label[65], ssid[33], subnet[24], gateway[16], gateway_mac[18];
    int64_t last_seen;
} sn_network_t;

typedef struct
{
    char id[33], network[33], ip[16], mac[18], hostname[65], services[160];
    char label[65], category[33], note[257];
    int64_t first_seen, last_seen;
    uint32_t sources, seen_cycle, new_cycle;
    bool trusted;
    int rtt_ms;
} sn_device_t;

typedef struct
{
    uint32_t generation, cycle, interval;
    int network_count, device_count;
    sn_network_t networks[SN_NETWORKS];
    sn_device_t devices[SN_DEVICES];
} sn_model_t;

typedef struct
{
    bool busy, connected, persistent, dirty;
    int percent, devices, networks, observed, new_devices, rssi, channel;
    uint32_t revision, cycle, interval, dropped;
    char ip[16], ssid[33], subnet[24], range[24], network[33], message[128], code[9];
    char diagnostic[384];
    char security[40];
    char gateway[16], dns[16];
} sn_status_t;

extern SemaphoreHandle_t sn_mutex;
extern sn_model_t *sn_model;
static inline bool sn_digest(const void *data, size_t length, unsigned char digest[32])
{
    size_t written = 0;
    return psa_hash_compute(PSA_ALG_SHA_256,data,length,digest,32,&written) == PSA_SUCCESS && written == 32;
}
bool sn_status(sn_status_t *out);
bool sn_device_at(int index, sn_device_t *out);
esp_err_t sn_init(void);
esp_err_t sn_scan(const char *cidr);
esp_err_t sn_diagnose(const char *ip);
esp_err_t sn_edit(const sn_device_t *device);
esp_err_t sn_settings(uint32_t interval);
esp_err_t sn_import(sn_model_t *incoming); /* Ownership transfers only on ESP_OK. */
esp_err_t sn_exit(bool discard);
void sn_cancel(void);
bool sn_confirmed(const sn_device_t *device);
void sn_ui_create(lv_obj_t *screen);
bool sn_ui_back(void);
void sn_alarm_prepare(void);
void sn_alarm_start(void);
esp_err_t sn_web_start(void);
void sn_web_stop(void);
bool sn_store_load(sn_model_t *out);
bool sn_store_save(const sn_model_t *snapshot);
bool sn_store_restore(const sn_model_t *snapshot);
bool sn_store_corrupt(void);
#if CONFIG_WATCH_DEV_CONSOLE
bool sn_store_selftest(void);
esp_err_t sn_selftest(void);
#endif
char *sn_export(void);
sn_model_t *sn_parse(const char *json, size_t length, char *error, size_t capacity);
cJSON *sn_device_json(const sn_device_t *device);
cJSON *sn_network_json(const sn_network_t *network);
#endif
