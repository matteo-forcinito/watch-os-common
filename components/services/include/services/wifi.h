#ifndef SERVICES_WIFI_H
#define SERVICES_WIFI_H

/*
 * Wi-Fi, switched on only while someone needs it.
 *
 * Wi-Fi is the most power-hungry part of the chip (~80-100 mA while on), so
 * it is reference counted: the radio, the weather, the time sync and the web
 * page "acquire" it and "release" it when done. A few seconds after the last
 * release the radio is switched off.
 *
 *     if (wifi_acquire(15000) == ESP_OK)     // joins the best known network
 *     {
 *         ... HTTP requests ...
 *         wifi_release();
 *     }
 *
 * Known networks are kept in NVS (up to 8). At boot they are merged with
 * config/wifi.txt in the available file storage ("ssid = password" lines).
 * On SD-equipped boards, /networks.json from the old Arduino launcher is also imported.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#define WIFI_KNOWN_MAX  8
#define WIFI_SCAN_MAX   20

typedef struct
{
    char ssid[33];
    int8_t rssi;     /* dBm: -50 great, -80 weak */
    bool open;       /* no password */
    bool known;
} wifi_ap_t;

typedef struct
{
    char ssid[33];
    char password[65];
} wifi_known_t;

esp_err_t wifi_service_init(void);

/* Connect to a known network (blocking, up to timeout_ms) and hold Wi-Fi on. */
esp_err_t wifi_acquire(uint32_t timeout_ms);

/* Done with the network: Wi-Fi goes off shortly after the last release. */
void wifi_release(void);

/* The radio only (ESP-NOW between devices, no network, no internet): driver on
 * in station mode, not joined. Same reference count as wifi_acquire(): pair
 * it with wifi_radio_release(). Immediate (no scan, no join). */
esp_err_t wifi_radio_acquire(void);
void wifi_radio_release(void);
/* Current radio channel (1..13), 0 if the radio is off. */
uint8_t wifi_radio_channel(void);
/* Move the radio to `channel`; refused (false) while joined to a network or
 * serving the access point: then everyone must use the network's channel. */
bool wifi_radio_set_channel(uint8_t channel);
/* Channel of the last network joined since boot (0 = none yet): devices that
 * listen without a network wait there, where the joined ones are. */
uint8_t wifi_home_channel(void);

/*
 * Big transfers (firmware update): no modem sleep while held, several times
 * faster. Balanced calls: wifi_set_fast(true) ... wifi_set_fast(false).
 */
void wifi_set_fast(bool fast);

bool wifi_is_connected(void);
typedef struct {
    bool driver, started, access_point;
    unsigned holders, radio_holders, fast_holders, scans;
} wifi_activity_t;
/* Nonblocking: false while another operation owns the radio mutex. */
bool wifi_activity_snapshot(wifi_activity_t *out);
#if CONFIG_WATCH_DEV_CONSOLE
/* Local QA only: drop STA without changing saved networks or owners. */
void wifi_diagnostic_drop(void);
#endif

/* Name of the network we are on ("" if none). */
void wifi_current_ssid(char *out, size_t size);

/* Our IP address as text, false if none. */
bool wifi_get_ip(char *out, size_t size);

/* Signal of the current network in dBm (0 if not connected). */
int wifi_rssi(void);

/* Scan (blocks ~2 s). Strongest first, one entry per name. Returns the count. */
int wifi_scan(wifi_ap_t *out, int max);

/* Every access point heard (one entry per radio, hidden ones included), strongest first. */
typedef struct
{
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    uint8_t auth;    /* wifi_auth_mode_t */
} wifi_ap_detail_t;
int wifi_scan_detail(wifi_ap_detail_t *out, int max);

/* Try a network; on success it is saved as known. Holds Wi-Fi like wifi_acquire(). */
esp_err_t wifi_connect_new(const char *ssid, const char *password, uint32_t timeout_ms);

/* Select an existing profile; holds Wi-Fi on success, release with wifi_release(). */
esp_err_t wifi_connect_saved(const char *ssid, uint32_t timeout_ms);
void wifi_preferred_ssid(char *out, size_t size);
/* Return to strongest-known selection on the next reconnect, retaining the current link. */
esp_err_t wifi_select_automatic(void);
/* Persist a known SSID (or empty for automatic) without changing the live link. */
esp_err_t wifi_set_preferred(const char *ssid);

/* Known networks. */
int wifi_known_list(wifi_known_t *out, int max);
esp_err_t wifi_known_add(const char *ssid, const char *password);
esp_err_t wifi_known_remove(const char *ssid);
bool wifi_known_any(void);

/* Reason (wifi_err_reason_t) of the last failed / lost connection, 0 if none. */
int wifi_last_reason(void);

/* That reason in words: "password errata", "rete non trovata (solo 2,4 GHz)"... */
const char *wifi_reason_text(int reason);

/*
 * Setup access point ("<product>-XXXX", e.g. "AMOLED-Watch-XXXX"): lets a phone open the setup page
 * when no known network is around. Counts as an acquire.
 */
esp_err_t wifi_start_access_point(char *ssid_out, size_t size);
void wifi_stop_access_point(void);

#endif
