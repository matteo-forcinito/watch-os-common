#ifndef SENTINEL_LITE_H
#define SENTINEL_LITE_H

/*
 * Sentinel Lite: the home network seen from the watch, built into the OS.
 *
 *   inventory   who is on the Wi-Fi: ARP sweep of the subnet (in batches, read
 *               from lwIP's ARP table), ping, names (reverse DNS, NetBIOS,
 *               mDNS, UPnP), maker from the MAC; new devices are flagged
 *   check       router and internet latency and loss, DNS time, signal, security
 *   airwaves    nearby 2.4 GHz networks and the least busy channel
 *   ports       the open services of one device (with warnings: Telnet, SMB...)
 *
 * Compared with the external Sentinel: no packet capture, no radio "find",
 * no speed test, a compact maker table. Jobs run in a short-lived task while
 * the app is open; the devices of each network are kept on the card.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#define SL_DEVICES  160
#define SL_APS      24
#define SL_PORTS    20

typedef struct
{
    uint32_t ip;                 /* network byte order, 0 = not seen in this network yet */
    uint8_t mac[6];
    bool has_mac;
    bool trusted;
    bool fresh;                  /* first seen in the last scan */
    bool online;                 /* answered the last scan */
    int16_t rtt;                 /* ms, -1 = no ping answer */
    int64_t first_seen, last_seen;
    uint32_t open_ports;         /* bit i = sl_port_list()[i], after a port check */
    bool ports_checked;
    char label[32];              /* the user's name */
    char dns[48], netbios[17], mdns[48], upnp[64];
    char vendor[24];
} sl_device_t;

typedef struct
{
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    char security[20];
    bool current;
} sl_ap_t;

typedef struct
{
    bool done;
    int rssi;
    char security[24];
    int channel;
    int gateway_avg, gateway_max, gateway_loss;      /* ms, ms, % */
    int internet_ms, dns_ms;                         /* -1 = failed */
    char verdict[160];
} sl_check_t;

typedef enum { SL_IDLE, SL_WIFI, SL_SCANNING, SL_NAMING, SL_CHECKING, SL_PORTS_RUNNING, SL_AIR, SL_FAILED } sl_phase_t;

typedef struct
{
    sl_phase_t phase;
    int percent;
    char message[64];
    char ssid[33];
    char ip[16], gateway[16];
    int devices, online, fresh;
    int64_t scanned;              /* unix time of the last full scan */
    uint32_t revision;
} sl_status_t;

esp_err_t sl_start(void);         /* tables (PSRAM) at the first open */
void sl_app_open(bool open);      /* open: a scan if the last one is older than 10 minutes */
void sl_scan(void);
void sl_check(void);
void sl_airwaves(void);
void sl_ports(uint32_t ip);

void sl_status(sl_status_t *out);
void sl_lock(void);
void sl_unlock(void);
const sl_device_t *sl_devices(int *count);   /* online first, then by IP */
const sl_ap_t *sl_aps(int *count);
void sl_check_result(sl_check_t *out);
const uint16_t *sl_port_list(int *count);
const char *sl_port_name(int index);
/* "Telnet aperto: password in chiaro" or NULL. */
const char *sl_port_warning(int index);

/* Label and trust, saved for this network. */
bool sl_edit(const uint8_t mac[6], uint32_t ip, const char *label, bool trusted);

/* The best name: the user's label, then mDNS, NetBIOS, UPnP, DNS, maker. */
void sl_display_name(const sl_device_t *d, char *out, size_t size);
/* "Telefono", "Router", "Stampante"... from the evidence, "" if unsure. */
const char *sl_kind(const sl_device_t *d, bool gateway);
void sl_ip_text(uint32_t ip, char *out, size_t size);
void sl_mac_text(const uint8_t mac[6], char *out, size_t size);

void sentinel_ui_create(lv_obj_t *screen);
bool sentinel_ui_back(void);
void sentinel_ui_destroy(void);
esp_err_t sentinel_web_start(void);

#endif
