/* Private to the Sentinel worker: probe.c owns the network state, identify.c and
 * radio.c run their jobs on the same worker task through these helpers. */
#ifndef SENTINEL_INTERNAL_H
#define SENTINEL_INTERNAL_H
#include "sentinel.h"
#include "esp_netif.h"

/* probe.c */
bool sn_connect(void);                       /* join/refresh the network, start the dashboard */
void sn_message(const char *text, int percent);
bool sn_save(void);
bool sn_cancelled(void);
const esp_netif_ip_info_t *sn_ip_info(void); /* valid after sn_connect() */
bool sn_local(uint32_t address);             /* inside the connected subnet */
int sn_ping(uint32_t ip, int timeout_ms);    /* one ICMP echo: RTT ms or -1 */
uint32_t sn_dns_server(void);                /* network order, 0 if unknown */
/* Device of the current network with this IP (caller holds sn_mutex), or NULL. */
sn_device_t *sn_device_by_ip_locked(uint32_t ip);
void sn_touch_locked(void);                  /* mark dirty and bump the revision */
void sn_observe(uint32_t ip, uint32_t source, int rtt, const char *service);
void sn_report(const char *text);            /* text of the Diagnostics page */
void sn_wifi_adopt(void);                    /* wifi_connect_new() succeeded: keep its reference */

/* identify.c */
typedef struct { uint32_t ip; uint16_t port; char path[128]; } sn_upnp_t;
void sn_lookup_names(void);                  /* DNS/NetBIOS/mDNS names of this scan's devices */
void sn_upnp_describe(const sn_upnp_t *list, int count);
void sn_run_identify(const char *ip);

/* radio.c */
void sn_radio_init(void);
void sn_run_find(const char *device_id);
void sn_run_quality(int seconds);
void sn_run_speed(unsigned megabytes, bool upload);
void sn_run_wifi_scan(void);
bool sn_run_wifi_join(const char *ssid, const char *password, bool saved);
#endif
