/*
 * Sentinel Lite engine (see lite.h): jobs in a short-lived task, devices of
 * each network in PSRAM and on the card (sentinel/<network>.json).
 */
#include "lite.h"

#include "oui.h"
#include "packet.h"

#include "core/sys.h"
#include "services/storage.h"
#include "services/wifi.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/etharp.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "sentinel";

static SemaphoreHandle_t s_lock;
static sl_device_t *s_devices;          /* SL_DEVICES, PSRAM */
static int s_count;
static sl_ap_t s_aps_table[SL_APS];
static int s_ap_count;
static sl_check_t s_check_result;
static sl_status_t s_state;
static char s_network[12];              /* file key of the loaded network */
static bool s_known_before;             /* the network had devices before this scan */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_running, s_want_scan, s_want_check, s_want_air;
static uint32_t s_want_ports;
static volatile bool s_app_open, s_cancel;

static const uint16_t s_ports[SL_PORTS] = {21, 22, 23, 53, 80, 139, 443, 445, 554, 631,
                                           1883, 3389, 5000, 5900, 8008, 8009, 8080, 8123, 8443, 9100};
static const char *const s_port_names[SL_PORTS] = {
    "FTP", "SSH", "Telnet", "DNS", "Web", "NetBIOS", "Web sicuro", "Condivisione file (SMB)", "Videocamera (RTSP)",
    "Stampante (IPP)", "MQTT (domotica)", "Desktop remoto", "NAS / UPnP", "VNC", "Chromecast", "Chromecast",
    "Web alternativo", "Home Assistant", "Web sicuro alt.", "Stampante (RAW)"};

void sl_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void sl_unlock(void) { xSemaphoreGive(s_lock); }
const sl_device_t *sl_devices(int *count) { *count = s_count; return s_devices; }
const sl_ap_t *sl_aps(int *count) { *count = s_ap_count; return s_aps_table; }
const uint16_t *sl_port_list(int *count) { *count = SL_PORTS; return s_ports; }
const char *sl_port_name(int index) { return index >= 0 && index < SL_PORTS ? s_port_names[index] : ""; }

const char *sl_port_warning(int index)
{
    switch (s_ports[index])
    {
    case 21: return "FTP aperto: password in chiaro";
    case 23: return "Telnet aperto: password in chiaro, da spegnere";
    case 445: return "SMB visibile: condivisioni di file in rete";
    case 3389: return "Desktop remoto aperto";
    case 5900: return "VNC aperto: controllo remoto dello schermo";
    case 1883: return "MQTT senza cifratura";
    default: return NULL;
    }
}

void sl_status(sl_status_t *out)
{
    sl_lock();
    *out = s_state;
    sl_unlock();
}

void sl_check_result(sl_check_t *out)
{
    sl_lock();
    *out = s_check_result;
    sl_unlock();
}

void sl_ip_text(uint32_t ip, char *out, size_t size)
{
    struct in_addr a = {.s_addr = ip};
    inet_ntop(AF_INET, &a, out, size);
}

void sl_mac_text(const uint8_t mac[6], char *out, size_t size)
{
    snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void set_state(sl_phase_t phase, int percent, const char *message)
{
    sl_lock();
    s_state.phase = phase;
    s_state.percent = percent;
    if (message != NULL)
    {
        snprintf(s_state.message, sizeof(s_state.message), "%s", message);
    }
    s_state.revision++;
    sl_unlock();
}

/* ------------------------------------------------------------- names */

static void strip_local(char *name)
{
    size_t n = strlen(name);
    if (n > 6 && strcmp(name + n - 6, ".local") == 0)
    {
        name[n - 6] = '\0';
    }
    n = strlen(name);
    if (n > 4 && strcmp(name + n - 4, ".lan") == 0)
    {
        name[n - 4] = '\0';
    }
}

void sl_display_name(const sl_device_t *d, char *out, size_t size)
{
    const char *pick = d->label[0] ? d->label : d->mdns[0] ? d->mdns : d->netbios[0] ? d->netbios
                     : d->upnp[0] ? d->upnp : d->dns[0] ? d->dns : NULL;
    if (pick != NULL)
    {
        snprintf(out, size, "%s", pick);
        return;
    }
    if (d->vendor[0])
    {
        snprintf(out, size, "Dispositivo %s", d->vendor);
        return;
    }
    char ip[16];
    sl_ip_text(d->ip, ip, sizeof(ip));
    snprintf(out, size, "%s", ip);
}

static bool has(const sl_device_t *d, const char *needle)
{
    const char *fields[] = {d->label, d->mdns, d->netbios, d->upnp, d->dns, d->vendor};
    for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++)
    {
        const char *text = fields[f];
        size_t n = strlen(needle);
        for (const char *p = text; *p; p++)
        {
            if (strncasecmp(p, needle, n) == 0)
            {
                return true;
            }
        }
    }
    return false;
}

const char *sl_kind(const sl_device_t *d, bool gateway)
{
    if (gateway) return "Router";
    if (has(d, "iphone") || has(d, "android") || has(d, "galaxy") || has(d, "pixel") || has(d, "redmi")) return "Telefono";
    if (has(d, "ipad") || has(d, "tab")) return "Tablet";
    if (has(d, "macbook") || has(d, "laptop") || has(d, "desktop") || has(d, "-pc") || has(d, "imac")) return "Computer";
    if (has(d, "printer") || has(d, "epson") || has(d, "canon") || has(d, "brother") || has(d, "laserjet")) return "Stampante";
    if (has(d, "chromecast") || has(d, "tv") || has(d, "bravia") || has(d, "roku") || has(d, "fire")) return "TV / media";
    if (has(d, "sonos") || has(d, "echo") || has(d, "speaker") || has(d, "nest")) return "Altoparlante";
    if (has(d, "camera") || has(d, "hikvision") || has(d, "dahua") || has(d, "reolink") || has(d, "ezviz")) return "Videocamera";
    if (has(d, "espressif") || has(d, "shelly") || has(d, "tuya") || has(d, "sonoff") || has(d, "hue")) return "Domotica";
    if (has(d, "synology") || has(d, "qnap") || has(d, "nas")) return "NAS";
    if (has(d, "playstation") || has(d, "nintendo") || has(d, "xbox")) return "Console";
    if (d->open_ports & (1u << 19 | 1u << 9)) return "Stampante";
    return "";
}

/* -------------------------------------------------------------- store */

static void network_key(const char *ssid, char *out, size_t size)
{
    uint32_t h = 2166136261u;
    for (const char *p = ssid; *p; p++)
    {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    snprintf(out, size, "%08lx", (unsigned long)h);
}

static bool path_of(const char *key, char *out, size_t size)
{
    const char *root = storage_dir();
    if (root == NULL)
    {
        return false;
    }
    char folder[48];
    snprintf(folder, sizeof(folder), "%s/sentinel", root);
    mkdir(folder, 0775);
    snprintf(out, size, "%s/%s.json", folder, key);
    return true;
}

static void hex_mac(const uint8_t mac[6], char *out)
{
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool parse_hex_mac(const char *text, uint8_t mac[6])
{
    if (text == NULL || strlen(text) != 12)
    {
        return false;
    }
    for (int i = 0; i < 6; i++)
    {
        char pair[3] = {text[i * 2], text[i * 2 + 1], 0};
        mac[i] = (uint8_t)strtol(pair, NULL, 16);
    }
    return true;
}

static void copy_field(cJSON *o, const char *name, char *out, size_t size)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(o, name));
    snprintf(out, size, "%s", v != NULL ? v : "");
}

/* Locked. */
static void save_network(void)
{
    char path[80];
    if (s_network[0] == '\0' || !path_of(s_network, path, sizeof(path)))
    {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", s_state.ssid);
    cJSON *list = cJSON_AddArrayToObject(root, "devices");
    for (int i = 0; i < s_count; i++)
    {
        const sl_device_t *d = &s_devices[i];
        if (!d->has_mac)
        {
            continue;
        }
        char mac[13];
        hex_mac(d->mac, mac);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "mac", mac);
        cJSON_AddNumberToObject(o, "ip", d->ip);
        cJSON_AddStringToObject(o, "label", d->label);
        cJSON_AddBoolToObject(o, "trusted", d->trusted);
        cJSON_AddNumberToObject(o, "first", (double)d->first_seen);
        cJSON_AddNumberToObject(o, "last", (double)d->last_seen);
        cJSON_AddStringToObject(o, "dns", d->dns);
        cJSON_AddStringToObject(o, "netbios", d->netbios);
        cJSON_AddStringToObject(o, "mdns", d->mdns);
        cJSON_AddStringToObject(o, "upnp", d->upnp);
        cJSON_AddItemToArray(list, o);
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL)
    {
        return;
    }
    char temp[88];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE *f = fopen(temp, "w");
    if (f != NULL)
    {
        bool ok = fputs(text, f) >= 0;
        ok &= fclose(f) == 0;
        if (ok)
        {
            unlink(path);
            rename(temp, path);
        }
    }
    cJSON_free(text);
}

/* Locked. Switches to the devices saved for this network. */
static void load_network(const char *ssid)
{
    char key[12];
    network_key(ssid, key, sizeof(key));
    if (strcmp(key, s_network) == 0)
    {
        return;
    }
    if (s_network[0])
    {
        save_network();
    }
    snprintf(s_network, sizeof(s_network), "%s", key);
    s_count = 0;
    char path[80];
    if (!path_of(key, path, sizeof(path)))
    {
        return;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL)
    {
        return;
    }
    char *text = heap_caps_malloc(96 * 1024, MALLOC_CAP_SPIRAM);
    size_t n = text != NULL ? fread(text, 1, 96 * 1024 - 1, f) : 0;
    fclose(f);
    if (text == NULL)
    {
        return;
    }
    text[n] = '\0';
    cJSON *root = cJSON_Parse(text);
    free(text);
    cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(root, "devices"))
    {
        if (s_count == SL_DEVICES)
        {
            break;
        }
        sl_device_t *d = &s_devices[s_count];
        memset(d, 0, sizeof(*d));
        if (!parse_hex_mac(cJSON_GetStringValue(cJSON_GetObjectItem(o, "mac")), d->mac))
        {
            continue;
        }
        d->has_mac = true;
        d->ip = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "ip"));
        d->trusted = cJSON_IsTrue(cJSON_GetObjectItem(o, "trusted"));
        d->first_seen = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "first"));
        d->last_seen = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "last"));
        d->rtt = -1;
        copy_field(o, "label", d->label, sizeof(d->label));
        copy_field(o, "dns", d->dns, sizeof(d->dns));
        copy_field(o, "netbios", d->netbios, sizeof(d->netbios));
        copy_field(o, "mdns", d->mdns, sizeof(d->mdns));
        copy_field(o, "upnp", d->upnp, sizeof(d->upnp));
        const char *vendor = (d->mac[0] & 0x02) ? "MAC privato" : sn_oui_vendor(d->mac);
        snprintf(d->vendor, sizeof(d->vendor), "%s", vendor != NULL ? vendor : "");
        s_count++;
    }
    cJSON_Delete(root);
}

bool sl_edit(const uint8_t mac[6], uint32_t ip, const char *label, bool trusted)
{
    bool found = false;
    sl_lock();
    for (int i = 0; i < s_count; i++)
    {
        sl_device_t *d = &s_devices[i];
        if ((mac != NULL && d->has_mac && memcmp(d->mac, mac, 6) == 0) || (mac == NULL && d->ip == ip))
        {
            snprintf(d->label, sizeof(d->label), "%s", label != NULL ? label : "");
            d->trusted = trusted;
            d->fresh = false;
            found = true;
        }
    }
    if (found)
    {
        save_network();
        s_state.revision++;
    }
    sl_unlock();
    return found;
}

/* Locked. Seen with `mac` (may be NULL) at `ip`. */
static sl_device_t *observe(uint32_t ip, const uint8_t *mac, int rtt)
{
    sl_device_t *d = NULL;
    for (int i = 0; i < s_count && d == NULL; i++)
    {
        if (mac != NULL && s_devices[i].has_mac && memcmp(s_devices[i].mac, mac, 6) == 0)
        {
            d = &s_devices[i];
        }
    }
    for (int i = 0; i < s_count && d == NULL; i++)
    {
        if (s_devices[i].ip == ip && (!s_devices[i].has_mac || mac == NULL))
        {
            d = &s_devices[i];
        }
    }
    int64_t now = time(NULL);
    if (d == NULL)
    {
        if (s_count == SL_DEVICES)
        {
            return NULL;
        }
        d = &s_devices[s_count++];
        memset(d, 0, sizeof(*d));
        d->rtt = -1;
        d->first_seen = now;
        d->fresh = s_known_before;   /* the first scan of a network: everything is "known" */
    }
    d->ip = ip;
    d->online = true;
    d->last_seen = now;
    if (rtt >= 0)
    {
        d->rtt = (int16_t)rtt;
    }
    if (mac != NULL && !d->has_mac)
    {
        memcpy(d->mac, mac, 6);
        d->has_mac = true;
        const char *vendor = (mac[0] & 0x02) ? "MAC privato" : sn_oui_vendor(mac);
        snprintf(d->vendor, sizeof(d->vendor), "%s", vendor != NULL ? vendor : "");
    }
    return d;
}

static sl_device_t *by_ip(uint32_t ip)
{
    for (int i = 0; i < s_count; i++)
    {
        if (s_devices[i].ip == ip && s_devices[i].online)
        {
            return &s_devices[i];
        }
    }
    return NULL;
}

/* Online first, then by IP (host order). Locked. */
static void sort_devices(void)
{
    for (int i = 1; i < s_count; i++)
    {
        sl_device_t moving = s_devices[i];
        uint32_t key = ntohl(moving.ip);
        int j = i - 1;
        while (j >= 0 && (s_devices[j].online < moving.online ||
                          (s_devices[j].online == moving.online && ntohl(s_devices[j].ip) > key)))
        {
            s_devices[j + 1] = s_devices[j];
            j--;
        }
        s_devices[j + 1] = moving;
    }
}

/* ---------------------------------------------------------------- ARP */

typedef struct
{
    struct netif *netif;
    uint32_t first;                  /* host order */
    uint32_t count;
    SemaphoreHandle_t done;
    struct
    {
        uint32_t ip;
        uint8_t mac[6];
    } seen[ARP_TABLE_SIZE];
    int seen_count;
} arp_job_t;

static void arp_send_on_tcpip(void *arg)
{
    arp_job_t *job = arg;
    for (uint32_t i = 0; i < job->count; i++)
    {
        ip4_addr_t ip = {.addr = htonl(job->first + i)};
        etharp_request(job->netif, &ip);
    }
    xSemaphoreGive(job->done);
}

static void arp_read_on_tcpip(void *arg)
{
    arp_job_t *job = arg;
    job->seen_count = 0;
    for (size_t i = 0; i < ARP_TABLE_SIZE; i++)
    {
        ip4_addr_t *ip;
        struct netif *netif;
        struct eth_addr *eth;
        if (etharp_get_entry(i, &ip, &netif, &eth) == 1 && netif == job->netif)
        {
            job->seen[job->seen_count].ip = ip->addr;
            memcpy(job->seen[job->seen_count].mac, eth->addr, 6);
            job->seen_count++;
        }
    }
    xSemaphoreGive(job->done);
}

static void arp_collect(arp_job_t *job)
{
    if (tcpip_callback(arp_read_on_tcpip, job) == ERR_OK)
    {
        xSemaphoreTake(job->done, portMAX_DELAY);
        sl_lock();
        for (int i = 0; i < job->seen_count; i++)
        {
            observe(job->seen[i].ip, job->seen[i].mac, -1);
        }
        sl_unlock();
    }
}

/* --------------------------------------------------------------- ICMP */

static uint16_t s_ping_id;

static uint16_t checksum16(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < length; i += 2)
    {
        sum += ((uint16_t)data[i] << 8) | data[i + 1];
    }
    if (length & 1)
    {
        sum += (uint16_t)data[length - 1] << 8;
    }
    while (sum >> 16)
    {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static bool echo_send(int fd, uint32_t ip, uint16_t sequence)
{
    uint8_t packet[16] = {8, 0};
    packet[4] = (uint8_t)(s_ping_id >> 8);
    packet[5] = (uint8_t)s_ping_id;
    packet[6] = (uint8_t)(sequence >> 8);
    packet[7] = (uint8_t)sequence;
    int64_t stamp = esp_timer_get_time();
    memcpy(packet + 8, &stamp, 8);
    uint16_t sum = checksum16(packet, sizeof(packet));
    packet[2] = (uint8_t)(sum >> 8);
    packet[3] = (uint8_t)sum;
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_addr.s_addr = ip};
    return sendto(fd, packet, sizeof(packet), 0, (struct sockaddr *)&to, sizeof(to)) == sizeof(packet);
}

/* Reads every reply waiting: calls back (ip, rtt). */
static void echo_drain(int fd, void (*cb)(uint32_t ip, int rtt, void *ctx), void *ctx)
{
    uint8_t buffer[96];
    struct sockaddr_in from;
    socklen_t size = sizeof(from);
    int length;
    while ((length = recvfrom(fd, buffer, sizeof(buffer), MSG_DONTWAIT, (struct sockaddr *)&from, &size)) > 0)
    {
        size_t offset = (size_t)(buffer[0] & 15) * 4;
        if ((buffer[0] >> 4) != 4 || offset < 20 || length < (int)(offset + 16))
        {
            size = sizeof(from);
            continue;
        }
        uint8_t *p = buffer + offset;
        if (p[0] != 0 || p[4] != (uint8_t)(s_ping_id >> 8) || p[5] != (uint8_t)s_ping_id)
        {
            size = sizeof(from);
            continue;
        }
        int64_t stamp;
        memcpy(&stamp, p + 8, 8);
        int64_t elapsed = esp_timer_get_time() - stamp;
        if (elapsed >= 0 && elapsed < 10000000)
        {
            cb(from.sin_addr.s_addr, (int)((elapsed + 500) / 1000), ctx);
        }
        size = sizeof(from);
    }
}

typedef struct
{
    uint32_t ip;
    int rtt;
} one_ping_t;

static void one_reply(uint32_t ip, int rtt, void *ctx)
{
    one_ping_t *p = ctx;
    if (ip == p->ip && p->rtt < 0)
    {
        p->rtt = rtt;
    }
}

static int ping_once(uint32_t ip, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0)
    {
        return -1;
    }
    static uint16_t sequence;
    one_ping_t p = {.ip = ip, .rtt = -1};
    if (echo_send(fd, ip, ++sequence))
    {
        int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        while (p.rtt < 0 && esp_timer_get_time() < until)
        {
            echo_drain(fd, one_reply, &p);
            if (p.rtt < 0)
            {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
    }
    close(fd);
    return p.rtt;
}

static void sweep_reply(uint32_t ip, int rtt, void *ctx)
{
    (void)ctx;
    sl_lock();
    observe(ip, NULL, rtt);
    sl_unlock();
}

/* -------------------------------------------------------------- names */

typedef struct
{
    uint32_t ip;
    char dns[48], netbios[17], mdns[48];
} lookup_t;

static void names_drain(int fd, lookup_t *t, int count)
{
    uint8_t buffer[600];
    struct sockaddr_in from;
    socklen_t size = sizeof(from);
    int length;
    while ((length = recvfrom(fd, buffer, sizeof(buffer), MSG_DONTWAIT, (struct sockaddr *)&from, &size)) > 0)
    {
        uint16_t port = ntohs(from.sin_port), id = 0;
        char name[65];
        if (port == 53 || port == 5353)
        {
            if (sn_dns_ptr_answer(buffer, (size_t)length, &id, name, sizeof(name)))
            {
                int i = id & 0x3fff;
                if (i < count && (id & 0xc000) == 0x4000 && port == 53 && strstr(name, "in-addr.arpa") == NULL)
                {
                    strip_local(name);
                    snprintf(t[i].dns, sizeof(t[i].dns), "%.47s", name);
                }
                else if (i < count && (id & 0xc000) == 0x8000 && port == 5353 && from.sin_addr.s_addr == t[i].ip && name[0] != '(')
                {
                    strip_local(name);
                    snprintf(t[i].mdns, sizeof(t[i].mdns), "%.47s", name);
                }
            }
        }
        else if (port == 137)
        {
            for (int i = 0; i < count; i++)
            {
                if (t[i].ip == from.sin_addr.s_addr && sn_nbstat_name(buffer, (size_t)length, (uint16_t)(0xc000 | i), name, sizeof(t[i].netbios)))
                {
                    snprintf(t[i].netbios, sizeof(t[i].netbios), "%.16s", name);
                }
            }
        }
        size = sizeof(from);
    }
}

static void lookup_names(uint32_t dns_server)
{
    lookup_t *t = heap_caps_calloc(SL_DEVICES, sizeof(lookup_t), MALLOC_CAP_SPIRAM);
    if (t == NULL)
    {
        return;
    }
    int count = 0;
    sl_lock();
    for (int i = 0; i < s_count; i++)
    {
        if (s_devices[i].online)
        {
            t[count++].ip = s_devices[i].ip;
        }
    }
    sl_unlock();
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd >= 0)
    {
        uint8_t packet[64];
        for (int i = 0; i < count && !s_cancel; i++)
        {
            struct sockaddr_in to = {.sin_family = AF_INET};
            size_t length = sn_dns_ptr_query(packet, sizeof(packet), (uint16_t)(0x4000 | i), t[i].ip);
            to.sin_addr.s_addr = dns_server;
            to.sin_port = htons(53);
            sendto(fd, packet, length, 0, (struct sockaddr *)&to, sizeof(to));
            length = sn_nbstat_query(packet, sizeof(packet), (uint16_t)(0xc000 | i));
            to.sin_addr.s_addr = t[i].ip;
            to.sin_port = htons(137);
            sendto(fd, packet, length, 0, (struct sockaddr *)&to, sizeof(to));
            length = sn_dns_ptr_query(packet, sizeof(packet), (uint16_t)(0x8000 | i), t[i].ip);
            to.sin_port = htons(5353);
            sendto(fd, packet, length, 0, (struct sockaddr *)&to, sizeof(to));
            if ((i & 7) == 7)
            {
                names_drain(fd, t, count);
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        int64_t until = esp_timer_get_time() + 1500000;
        while (esp_timer_get_time() < until && !s_cancel)
        {
            names_drain(fd, t, count);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        names_drain(fd, t, count);
        close(fd);
    }
    sl_lock();
    for (int i = 0; i < count; i++)
    {
        sl_device_t *d = by_ip(t[i].ip);
        if (d == NULL)
        {
            continue;
        }
        if (t[i].dns[0]) snprintf(d->dns, sizeof(d->dns), "%s", t[i].dns);
        if (t[i].netbios[0]) snprintf(d->netbios, sizeof(d->netbios), "%s", t[i].netbios);
        if (t[i].mdns[0]) snprintf(d->mdns, sizeof(d->mdns), "%s", t[i].mdns);
    }
    sl_unlock();
    free(t);
}

static int connect_timeout(uint32_t ip, uint16_t port, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
    {
        return -1;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip};
    if (connect(fd, (struct sockaddr *)&to, sizeof(to)) != 0)
    {
        bool ok = false;
        if (errno == EINPROGRESS)
        {
            fd_set writes;
            FD_ZERO(&writes);
            FD_SET(fd, &writes);
            struct timeval wait = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
            int error = 0;
            socklen_t length = sizeof(error);
            ok = select(fd + 1, NULL, &writes, NULL, &wait) > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                 error == 0;
        }
        if (!ok)
        {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static int http_get(uint32_t ip, uint16_t port, const char *path, char *buffer, size_t capacity)
{
    int fd = connect_timeout(ip, port, 800);
    if (fd < 0)
    {
        return -1;
    }
    char request[256], host[16];
    sl_ip_text(ip, host, sizeof(host));
    int length = snprintf(request, sizeof(request),
                          "GET %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: Sentinel\r\nConnection: close\r\n\r\n", path, host, port);
    int used = -1;
    if (length > 0 && length < (int)sizeof(request) && send(fd, request, (size_t)length, 0) == length)
    {
        used = 0;
        int64_t until = esp_timer_get_time() + 1500000;
        while (used < (int)capacity - 1 && esp_timer_get_time() < until)
        {
            fd_set reads;
            FD_ZERO(&reads);
            FD_SET(fd, &reads);
            struct timeval wait = {.tv_usec = 100000};
            if (select(fd + 1, &reads, NULL, NULL, &wait) <= 0)
            {
                continue;
            }
            int got = recv(fd, buffer + used, capacity - 1 - (size_t)used, 0);
            if (got <= 0)
            {
                break;
            }
            used += got;
        }
        buffer[used] = '\0';
    }
    close(fd);
    return used;
}

/* SSDP: UPnP devices announce a description; its friendly name and model. */
static void discover_upnp(uint32_t own_ip)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
    {
        return;
    }
    struct in_addr own = {.s_addr = own_ip};
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &own, sizeof(own));
    int ttl = 2;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    struct timeval timeout = {.tv_usec = 100000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in target = {.sin_family = AF_INET, .sin_port = htons(1900)};
    inet_pton(AF_INET, "239.255.255.250", &target.sin_addr);
    const char query[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ssdp:all\r\n\r\n";
    sendto(fd, query, sizeof(query) - 1, 0, (struct sockaddr *)&target, sizeof(target));
    typedef struct
    {
        uint32_t ip;
        uint16_t port;
        char path[128];
    } upnp_t;
    upnp_t *list = heap_caps_calloc(12, sizeof(upnp_t), MALLOC_CAP_SPIRAM);
    char *buffer = heap_caps_malloc(1200, MALLOC_CAP_SPIRAM);
    int described = 0;
    int64_t until = esp_timer_get_time() + 1500000;
    while (list != NULL && buffer != NULL && esp_timer_get_time() < until && !s_cancel)
    {
        struct sockaddr_in from;
        socklen_t size = sizeof(from);
        int length = recvfrom(fd, buffer, 1199, 0, (struct sockaddr *)&from, &size);
        if (length < 12)
        {
            continue;
        }
        buffer[length] = '\0';
        if (strncmp(buffer, "HTTP/1.1 200", 12) != 0 && strncmp(buffer, "HTTP/1.0 200", 12) != 0)
        {
            continue;
        }
        bool known = false;
        for (int i = 0; i < described; i++)
        {
            known |= list[i].ip == from.sin_addr.s_addr;
        }
        if (!known && described < 12 &&
            sn_location(buffer, from.sin_addr.s_addr, &list[described].port, list[described].path, sizeof(list[described].path)))
        {
            list[described++].ip = from.sin_addr.s_addr;
        }
    }
    close(fd);
    char *xml = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    for (int i = 0; xml != NULL && i < described && !s_cancel; i++)
    {
        if (http_get(list[i].ip, list[i].port, list[i].path, xml, 8192) <= 0)
        {
            continue;
        }
        char friendly[65] = "", model[49] = "", text[72];
        sn_xml_value(xml, "friendlyName", friendly, sizeof(friendly));
        sn_xml_value(xml, "modelName", model, sizeof(model));
        snprintf(text, sizeof(text), "%.40s%s%.22s", friendly, friendly[0] && model[0] ? " - " : "", model);
        if (!text[0] || !sn_text_valid(text, 63))
        {
            continue;
        }
        sl_lock();
        sl_device_t *d = by_ip(list[i].ip);
        if (d != NULL)
        {
            snprintf(d->upnp, sizeof(d->upnp), "%.63s", text);
        }
        sl_unlock();
    }
    free(xml);
    free(list);
    free(buffer);
}

/* --------------------------------------------------------------- jobs */

static esp_netif_t *station(esp_netif_ip_info_t *info)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL || esp_netif_get_ip_info(sta, info) != ESP_OK || info->ip.addr == 0)
    {
        return NULL;
    }
    return sta;
}

static void run_scan(void)
{
    set_state(SL_WIFI, 0, "Connessione al Wi-Fi...");
    if (wifi_acquire(20000) != ESP_OK)
    {
        set_state(SL_FAILED, 0, "Wi-Fi non raggiungibile");
        return;
    }
    esp_netif_ip_info_t info;
    esp_netif_t *sta = station(&info);
    struct netif *netif = sta != NULL ? esp_netif_get_netif_impl(sta) : NULL;
    if (netif == NULL)
    {
        wifi_release();
        set_state(SL_FAILED, 0, "Rete non pronta");
        return;
    }
    wifi_set_fast(true);
    char ssid[33] = "";
    wifi_current_ssid(ssid, sizeof(ssid));
    sl_lock();
    load_network(ssid);
    s_known_before = s_count > 0;
    snprintf(s_state.ssid, sizeof(s_state.ssid), "%s", ssid);
    sl_ip_text(info.ip.addr, s_state.ip, sizeof(s_state.ip));
    sl_ip_text(info.gw.addr, s_state.gateway, sizeof(s_state.gateway));
    for (int i = 0; i < s_count; i++)
    {
        s_devices[i].online = false;
        s_devices[i].fresh = false;
    }
    sl_unlock();

    /* The subnet, at most the 254 hosts around us. */
    uint32_t mask = ntohl(info.netmask.addr), own = ntohl(info.ip.addr);
    if (mask < 0xFFFFFF00u)
    {
        mask = 0xFFFFFF00u;
    }
    uint32_t first = (own & mask) + 1, last = (own | ~mask) - 1;
    uint32_t total = last >= first ? last - first + 1 : 0;

    arp_job_t *job = heap_caps_calloc(1, sizeof(arp_job_t), MALLOC_CAP_SPIRAM);
    if (job != NULL)
    {
        job->netif = netif;
        job->done = xSemaphoreCreateBinary();
    }
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s_ping_id == 0)
    {
        s_ping_id = (uint16_t)esp_random();
    }
    for (uint32_t at = 0; job != NULL && job->done != NULL && at < total && !s_cancel; at += 6)
    {
        job->first = first + at;
        job->count = total - at < 6 ? total - at : 6;
        if (tcpip_callback(arp_send_on_tcpip, job) == ERR_OK)
        {
            xSemaphoreTake(job->done, portMAX_DELAY);
        }
        for (uint32_t k = 0; fd >= 0 && k < job->count; k++)
        {
            echo_send(fd, htonl(job->first + k), (uint16_t)(job->first + k));
        }
        vTaskDelay(pdMS_TO_TICKS(180));
        arp_collect(job);
        if (fd >= 0)
        {
            echo_drain(fd, sweep_reply, NULL);
        }
        set_state(SL_SCANNING, (int)(at * 70 / (total ? total : 1)), "Cerco i dispositivi...");
    }
    /* Late answers. */
    for (int i = 0; i < 6 && !s_cancel; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(150));
        if (job != NULL && job->done != NULL)
        {
            arp_collect(job);
        }
        if (fd >= 0)
        {
            echo_drain(fd, sweep_reply, NULL);
        }
    }
    if (fd >= 0)
    {
        close(fd);
    }
    if (job != NULL && job->done != NULL)
    {
        vSemaphoreDelete(job->done);
    }
    free(job);

    if (!s_cancel)
    {
        set_state(SL_NAMING, 75, "Leggo i nomi dei dispositivi...");
        esp_netif_dns_info_t dns;
        uint32_t server = info.gw.addr;
        if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4 &&
            dns.ip.u_addr.ip4.addr != 0)
        {
            server = dns.ip.u_addr.ip4.addr;
        }
        lookup_names(server);
        set_state(SL_NAMING, 90, "Chiedo ai dispositivi UPnP...");
        discover_upnp(info.ip.addr);
    }
    wifi_set_fast(false);
    wifi_release();

    sl_lock();
    sort_devices();
    int online = 0, fresh = 0;
    for (int i = 0; i < s_count; i++)
    {
        online += s_devices[i].online;
        fresh += s_devices[i].online && s_devices[i].fresh;
    }
    s_state.devices = s_count;
    s_state.online = online;
    s_state.fresh = fresh;
    s_state.scanned = time(NULL);
    save_network();
    sl_unlock();
    char message[64];
    snprintf(message, sizeof(message), s_cancel ? "Scansione interrotta" : "%d dispositivi in rete%s", online,
             fresh ? ", alcuni nuovi" : "");
    set_state(SL_IDLE, 100, message);
    ESP_LOGI(TAG, "Scan: %d online, %d new, %d known", online, fresh, s_count);
}

static const char *auth_text(wifi_auth_mode_t mode)
{
    switch (mode)
    {
    case WIFI_AUTH_OPEN: return "Aperta";
    case WIFI_AUTH_WEP: return "WEP (obsoleta)";
    case WIFI_AUTH_WPA_PSK: return "WPA (obsoleta)";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_ENTERPRISE: return "Enterprise";
    default: return "Altra";
    }
}

static void run_check(void)
{
    set_state(SL_WIFI, 0, "Connessione al Wi-Fi...");
    if (wifi_acquire(20000) != ESP_OK)
    {
        set_state(SL_FAILED, 0, "Wi-Fi non raggiungibile");
        return;
    }
    wifi_set_fast(true);
    sl_check_t c = {.internet_ms = -1, .dns_ms = -1};
    esp_netif_ip_info_t info;
    if (station(&info) != NULL)
    {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        {
            c.rssi = ap.rssi;
            c.channel = ap.primary;
            snprintf(c.security, sizeof(c.security), "%s", auth_text(ap.authmode));
        }
        int sum = 0, answered = 0;
        for (int i = 0; i < 10 && !s_cancel; i++)
        {
            set_state(SL_CHECKING, i * 6, "Misuro il router...");
            int rtt = ping_once(info.gw.addr, 800);
            if (rtt >= 0)
            {
                sum += rtt;
                answered++;
                c.gateway_max = rtt > c.gateway_max ? rtt : c.gateway_max;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        c.gateway_avg = answered ? sum / answered : -1;
        c.gateway_loss = (10 - answered) * 10;
        set_state(SL_CHECKING, 70, "Misuro internet...");
        int best = -1;
        uint32_t cloudflare = 0;
        inet_pton(AF_INET, "1.1.1.1", &cloudflare);
        for (int i = 0; i < 3 && !s_cancel; i++)
        {
            int64_t start = esp_timer_get_time();
            int fd = connect_timeout(cloudflare, 443, 2000);
            if (fd >= 0)
            {
                int ms = (int)((esp_timer_get_time() - start) / 1000);
                best = best < 0 || ms < best ? ms : best;
                close(fd);
            }
        }
        c.internet_ms = best;
        set_state(SL_CHECKING, 90, "Misuro il DNS...");
        int64_t start = esp_timer_get_time();
        struct addrinfo hints = {.ai_family = AF_INET}, *result = NULL;
        if (getaddrinfo("www.wikipedia.org", NULL, &hints, &result) == 0)
        {
            c.dns_ms = (int)((esp_timer_get_time() - start) / 1000);
            freeaddrinfo(result);
        }
    }
    wifi_set_fast(false);
    wifi_release();

    const char *quality = c.rssi >= -60 ? "segnale ottimo" : c.rssi >= -70 ? "segnale buono" : c.rssi >= -78 ? "segnale debole"
                                                                                                   : "segnale molto debole";
    if (c.gateway_avg < 0)
    {
        snprintf(c.verdict, sizeof(c.verdict), "Il router non risponde: rete in difficoltà.");
    }
    else if (c.internet_ms < 0)
    {
        snprintf(c.verdict, sizeof(c.verdict), "Il router c'è ma internet no: guasto della linea o del modem.");
    }
    else if (c.gateway_loss >= 20 || c.gateway_avg > 60)
    {
        snprintf(c.verdict, sizeof(c.verdict), "Rete instabile (%s): avvicinati al router o cambia canale.", quality);
    }
    else
    {
        snprintf(c.verdict, sizeof(c.verdict), "Rete in salute: %s, router %d ms, internet %d ms.", quality, c.gateway_avg,
                 c.internet_ms);
    }
    c.done = true;
    sl_lock();
    s_check_result = c;
    sl_unlock();
    set_state(SL_IDLE, 100, "Verifica completata");
}

static void run_airwaves(void)
{
    set_state(SL_AIR, 0, "Ascolto le reti vicine...");
    wifi_ap_detail_t *list = heap_caps_calloc(SL_APS, sizeof(wifi_ap_detail_t), MALLOC_CAP_SPIRAM);
    int n = list != NULL ? wifi_scan_detail(list, SL_APS) : 0;
    char current[33] = "";
    wifi_current_ssid(current, sizeof(current));
    sl_lock();
    s_ap_count = 0;
    for (int i = 0; i < n && s_ap_count < SL_APS; i++)
    {
        sl_ap_t *a = &s_aps_table[s_ap_count++];
        snprintf(a->ssid, sizeof(a->ssid), "%s", list[i].ssid);
        a->channel = list[i].channel;
        a->rssi = list[i].rssi;
        snprintf(a->security, sizeof(a->security), "%s", auth_text((wifi_auth_mode_t)list[i].auth));
        a->current = current[0] && strcmp(current, list[i].ssid) == 0;
    }
    sl_unlock();
    free(list);
    char message[48];
    snprintf(message, sizeof(message), "%d reti vicine", n);
    set_state(SL_IDLE, 100, message);
}

static void run_ports(uint32_t ip)
{
    set_state(SL_PORTS_RUNNING, 0, "Controllo i servizi aperti...");
    if (wifi_acquire(20000) != ESP_OK)
    {
        set_state(SL_FAILED, 0, "Wi-Fi non raggiungibile");
        return;
    }
    uint32_t open = 0;
    for (int i = 0; i < SL_PORTS && !s_cancel; i++)
    {
        int fd = connect_timeout(ip, s_ports[i], 400);
        if (fd >= 0)
        {
            open |= 1u << i;
            close(fd);
        }
        set_state(SL_PORTS_RUNNING, (i + 1) * 100 / SL_PORTS, NULL);
    }
    wifi_release();
    sl_lock();
    for (int i = 0; i < s_count; i++)
    {
        if (s_devices[i].ip == ip)
        {
            s_devices[i].open_ports = open;
            s_devices[i].ports_checked = true;
        }
    }
    sl_unlock();
    set_state(SL_IDLE, 100, "Servizi controllati");
}

static void job_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        enum { NONE, SCAN, CHECK, AIR, PORTS } job = NONE;
        uint32_t ip = 0;
        taskENTER_CRITICAL(&s_mux);
        if (s_want_ports)
        {
            job = PORTS;
            ip = s_want_ports;
            s_want_ports = 0;
        }
        else if (s_want_scan)
        {
            job = SCAN;
            s_want_scan = false;
        }
        else if (s_want_check)
        {
            job = CHECK;
            s_want_check = false;
        }
        else if (s_want_air)
        {
            job = AIR;
            s_want_air = false;
        }
        else
        {
            s_running = false;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (job == NONE)
        {
            vTaskDelete(NULL);
            return;
        }
        if (!s_app_open)
        {
            continue;   /* closed meanwhile: drop it */
        }
        s_cancel = false;
        if (job == SCAN) run_scan();
        else if (job == CHECK) run_check();
        else if (job == AIR) run_airwaves();
        else run_ports(ip);
    }
}

static void kick(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool start = !s_running;
    s_running = true;
    taskEXIT_CRITICAL(&s_mux);
    if (start && !sys_task_create(job_task, "sentinel", 6144, NULL, 3, SYS_CORE_ANY))
    {
        taskENTER_CRITICAL(&s_mux);
        s_running = false;
        s_want_scan = s_want_check = s_want_air = false;
        s_want_ports = 0;
        taskEXIT_CRITICAL(&s_mux);
        set_state(SL_FAILED, 0, "Memoria piena: riprova tra poco");
    }
}

void sl_scan(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_want_scan = true;
    taskEXIT_CRITICAL(&s_mux);
    kick();
}

void sl_check(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_want_check = true;
    taskEXIT_CRITICAL(&s_mux);
    kick();
}

void sl_airwaves(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_want_air = true;
    taskEXIT_CRITICAL(&s_mux);
    kick();
}

void sl_ports(uint32_t ip)
{
    taskENTER_CRITICAL(&s_mux);
    s_want_ports = ip;
    taskEXIT_CRITICAL(&s_mux);
    kick();
}

void sl_app_open(bool open)
{
    s_app_open = open;
    if (!open)
    {
        s_cancel = true;
        return;
    }
    sl_status_t st;
    sl_status(&st);
    time_t now = time(NULL);
    if (st.scanned == 0 || now - st.scanned > 600)
    {
        sl_scan();
    }
}

esp_err_t sl_start(void)
{
    if (s_lock != NULL)
    {
        return ESP_OK;
    }
    s_devices = heap_caps_calloc(SL_DEVICES, sizeof(sl_device_t), MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    if (s_devices == NULL || s_lock == NULL)
    {
        free(s_devices);
        s_devices = NULL;
        if (s_lock != NULL)
        {
            vSemaphoreDelete(s_lock);
            s_lock = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_state.message, sizeof(s_state.message), "Pronto");
    return ESP_OK;
}
