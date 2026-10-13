/* Network worker: fresh ARP replies, ICMP, mDNS and SSDP; bounded local scopes. */
#include "sentinel.h"
#include "packet.h"
#include "core/clock.h"
#include "core/sys.h"
#include "extapp_sdk.h"
#include "services/storage.h"
#include "services/wifi.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mdns.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/queue.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>

SemaphoreHandle_t sn_mutex;
sn_model_t *sn_model;
static sn_status_t s_status;
static QueueHandle_t s_commands, s_replies;
static atomic_bool s_busy, s_cancel, s_capture;
static atomic_uint s_dropped, s_own;
static _Atomic(esp_netif_t *) s_station;
static bool s_wifi_held, s_mdns;
static esp_netif_ip_info_t s_info;
static char s_current_network[33];
static uint8_t s_gateway_mac[6];
static uint16_t s_ping_id;
static uint32_t s_ping_nonce;
static int64_t s_next_scan;

typedef struct { uint32_t ip; uint8_t mac[6]; } reply_t;
typedef enum { SCAN, DIAGNOSE, EDIT, SETTINGS, IMPORT, EXIT, SELFTEST } command_kind_t;
typedef struct
{
    command_kind_t kind;
    char target[24];
    sn_device_t *device;
    sn_model_t *incoming;
    uint32_t interval;
    bool discard;
} command_t;

/* Linker wrapping is local to this external firmware. Always forward the
 * original frame, even if our bounded observation queue is full. No sniffing,
 * altered Wi-Fi callback, raw payload storage or extra packet ownership. */
esp_err_t __real_esp_netif_receive(esp_netif_t *netif, void *buffer, size_t length, void *eb);
esp_err_t __wrap_esp_netif_receive(esp_netif_t *netif, void *buffer, size_t length, void *eb)
{
    if (netif == atomic_load(&s_station) && atomic_load(&s_capture))
    {
        reply_t reply;
        if (sn_arp_reply(buffer, length, atomic_load(&s_own), &reply.ip, reply.mac) &&
            xQueueSend(s_replies, &reply, 0) != pdTRUE) atomic_fetch_add(&s_dropped, 1);
    }
    return __real_esp_netif_receive(netif, buffer, length, eb);
}

static void message(const char *text, int percent)
{
    xSemaphoreTake(sn_mutex, portMAX_DELAY);
    snprintf(s_status.message, sizeof(s_status.message), "%s", text);
    if (percent >= 0) s_status.percent = percent;
    s_status.revision++;
    xSemaphoreGive(sn_mutex);
}

bool sn_confirmed(const sn_device_t *device)
{
    return (device->sources & (SN_ICMP | SN_MDNS | SN_SSDP | SN_TCP)) != 0 ||
           ((device->sources & SN_ARP) != 0 && (device->sources & SN_PROXY) == 0);
}

bool sn_status(sn_status_t *out)
{
    if (sn_mutex == NULL || xSemaphoreTake(sn_mutex, 0) != pdTRUE) return false;
    *out = s_status;
    out->busy = atomic_load(&s_busy);
    out->devices = sn_model->device_count;
    out->observed = 0;
    for (int i = 0; i < sn_model->device_count; i++)
        if (sn_model->cycle && sn_model->devices[i].seen_cycle == sn_model->cycle &&
            sn_confirmed(&sn_model->devices[i])) out->observed++;
    out->networks = sn_model->network_count;
    out->interval = sn_model->interval;
    out->dropped = atomic_load(&s_dropped);
    out->connected = wifi_is_connected();
    xSemaphoreGive(sn_mutex);
    return true;
}

bool sn_device_at(int index, sn_device_t *out)
{
    if (sn_mutex == NULL || xSemaphoreTake(sn_mutex, 0) != pdTRUE) return false;
    bool ok = index >= 0 && index < sn_model->device_count;
    if (ok) *out = sn_model->devices[index];
    xSemaphoreGive(sn_mutex);
    return ok;
}

static int64_t now(void) { return clock_is_valid() ? time(NULL) : 0; }

static void random_id(char out[33])
{
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", bytes[i]);
}

static bool local_ip(uint32_t address)
{
    uint32_t host = ntohl(address), mask = ntohl(s_info.netmask.addr);
    uint32_t base = ntohl(s_info.ip.addr) & mask;
    return mask != 0 && (host & mask) == base && host != base && host != (base | ~mask);
}

static void format_mac(const uint8_t mac[6], char out[18])
{
    snprintf(out,18,"%02x:%02x:%02x:%02x:%02x:%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

static void observe(uint32_t address, const uint8_t *mac, uint32_t source,
                    int rtt, const char *hostname, const char *service)
{
    if (!local_ip(address) || address == s_info.ip.addr || s_current_network[0] == 0) return;
    char ip[16], mac_text[18] = "", gateway_mac[18] = "";
    struct in_addr a = {.s_addr=address};
    inet_ntop(AF_INET,&a,ip,sizeof(ip));
    bool proxy = sn_proxy_arp(address,s_info.gw.addr,mac,s_gateway_mac);
    if (mac != NULL && !proxy) format_mac(mac,mac_text);
    if (memcmp(s_gateway_mac,"\0\0\0\0\0\0",6) != 0) format_mac(s_gateway_mac,gateway_mac);
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    sn_device_t *device = NULL;
    for (int i = 0; i < sn_model->device_count; i++)
    {
        sn_device_t *d = &sn_model->devices[i];
        if (strcmp(d->network,s_current_network) != 0) continue;
        if ((mac_text[0] && !proxy && strcmp(d->mac,mac_text) == 0) ||
            (strcmp(d->ip,ip) == 0 && (!mac_text[0] || !d->mac[0] || strcmp(d->mac,mac_text) == 0)))
        { device = d; break; }
    }
    if (device == NULL && sn_model->device_count < SN_DEVICES)
    {
        device = &sn_model->devices[sn_model->device_count++];
        memset(device,0,sizeof(*device));
        random_id(device->id);
        snprintf(device->network,sizeof(device->network),"%s",s_current_network);
        device->first_seen = now();
        device->new_cycle = sn_model->cycle;
        device->rtt_ms = -1;
        s_status.new_devices++;
    }
    if (device != NULL)
    {
        snprintf(device->ip,sizeof(device->ip),"%s",ip);
        if (mac_text[0]) snprintf(device->mac,sizeof(device->mac),"%s",mac_text);
        else if (address != s_info.gw.addr && gateway_mac[0] && strcmp(device->mac,gateway_mac) == 0)
            device->mac[0] = 0; /* Repair an older proxy MAC without changing identity or metadata. */
        if (device->seen_cycle != sn_model->cycle)
        {
            device->sources = 0;
            device->rtt_ms = -1;
        }
        device->sources |= source | (proxy ? SN_PROXY : 0);
        device->seen_cycle = sn_model->cycle;
        device->last_seen = now();
        if (device->first_seen == 0) device->first_seen = device->last_seen;
        if (rtt >= 0) device->rtt_ms = rtt;
        if (hostname != NULL && sn_text_valid(hostname,sizeof(device->hostname)-1))
            snprintf(device->hostname,sizeof(device->hostname),"%s",hostname);
        if (service != NULL && sn_text_valid(service,80) && strstr(device->services,service) == NULL)
        {
            size_t used = strlen(device->services), length = strlen(service);
            if (used + length + 3 < sizeof(device->services))
                snprintf(device->services + used,sizeof(device->services)-used,"%s%s",used ? ", " : "",service);
        }
        s_status.dirty = true;
    }
    else snprintf(s_status.message,sizeof(s_status.message),"Archivio pieno: limite %d dispositivi",SN_DEVICES);
    s_status.revision++;
    xSemaphoreGive(sn_mutex);
}

typedef struct
{
    uint32_t first, count;
    struct netif *netif;
    SemaphoreHandle_t done;
} arp_request_t;

static void arp_on_tcpip(void *argument)
{
    arp_request_t *request = argument;
    for (uint32_t i = 0; i < request->count; i++)
    {
        ip4_addr_t ip = {.addr=htonl(request->first+i)};
        etharp_request(request->netif,&ip);
    }
    xSemaphoreGive(request->done);
}

static bool arp_request(uint32_t first, uint32_t count)
{
    esp_netif_t *station = atomic_load(&s_station);
    arp_request_t request = {.first=first,.count=count,
        .netif=esp_netif_get_netif_impl(station),.done=xSemaphoreCreateBinary()};
    if (request.netif == NULL || request.done == NULL)
    { if (request.done) vSemaphoreDelete(request.done); return false; }
    err_t result = tcpip_callback(arp_on_tcpip,&request);
    if (result == ERR_OK) xSemaphoreTake(request.done,portMAX_DELAY);
    vSemaphoreDelete(request.done);
    return result == ERR_OK;
}

static void drain_arp(void)
{
    reply_t reply;
    while (xQueueReceive(s_replies,&reply,0) == pdTRUE)
        observe(reply.ip,reply.mac,SN_ARP,-1,NULL,NULL);
}

static uint16_t checksum16(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < length; i += 2) sum += ((uint16_t)data[i] << 8) | data[i+1];
    if (length & 1) sum += (uint16_t)data[length-1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static bool echo_send(int socket_fd, uint32_t ip, uint16_t sequence)
{
    uint8_t packet[24] = {8,0};
    packet[4] = s_ping_id >> 8; packet[5] = s_ping_id;
    packet[6] = sequence >> 8; packet[7] = sequence;
    memcpy(packet + 8,&s_ping_nonce,4);
    int64_t stamp = esp_timer_get_time();
    memcpy(packet + 12,&stamp,8);
    uint16_t sum = checksum16(packet,sizeof(packet));
    packet[2] = sum >> 8; packet[3] = sum;
    struct sockaddr_in target = {.sin_family=AF_INET,.sin_addr.s_addr=ip};
    return sendto(socket_fd,packet,sizeof(packet),0,(struct sockaddr *)&target,sizeof(target)) == sizeof(packet);
}

static int echo_read(int socket_fd, uint32_t expected_ip, int sequence, bool inventory)
{
    int found = -1;
    uint8_t buffer[128];
    struct sockaddr_in source;
    socklen_t size = sizeof(source);
    int length;
    while ((length = recvfrom(socket_fd,buffer,sizeof(buffer),MSG_DONTWAIT,(struct sockaddr *)&source,&size)) > 0)
    {
        size_t offset = (buffer[0] & 15) * 4;
        if ((buffer[0] >> 4) != 4 || offset < 20 || length < (int)(offset+24)) continue;
        uint8_t *p = buffer + offset;
        if (p[0] != 0 || p[1] != 0 || p[4] != (s_ping_id >> 8) || p[5] != (s_ping_id & 255) ||
            memcmp(p+8,&s_ping_nonce,4) != 0) continue;
        if (sequence >= 0 && (((int)p[6] << 8) | p[7]) != sequence) continue;
        int64_t stamp;
        memcpy(&stamp,p+12,8);
        int64_t elapsed = esp_timer_get_time()-stamp;
        if (elapsed < 0 || elapsed > 10000000) continue;
        int rtt = (int)((elapsed+500)/1000);
        if (inventory) observe(source.sin_addr.s_addr,NULL,SN_ICMP,rtt,NULL,NULL);
        if (source.sin_addr.s_addr == expected_ip) found = rtt;
    }
    return found;
}

static bool save(void)
{
    sn_model_t *copy = heap_caps_malloc(sizeof(*copy),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) { message("RAM insufficiente per salvare",-1); return false; }
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    *copy = *sn_model;
    copy->generation++;
    xSemaphoreGive(sn_mutex);
    bool ok = sn_store_save(copy);
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    s_status.persistent = ok;
    if (ok) { sn_model->generation = copy->generation; s_status.dirty = false; }
    else snprintf(s_status.message,sizeof(s_status.message),"Non salvato: controlla SD. Dati ancora in RAM.");
    s_status.revision++;
    xSemaphoreGive(sn_mutex);
    free(copy);
    return ok;
}

static bool connect_network(void)
{
    if (!s_wifi_held || !wifi_is_connected())
    {
        message("Connessione alla rete salvata...",0);
        if (s_wifi_held) { wifi_release(); s_wifi_held = false; }
        if (wifi_acquire(15000) != ESP_OK)
        { message("Wi-Fi assente: configura una rete nel launcher",-1); return false; }
        s_wifi_held = true;
    }
    esp_netif_t *station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station == NULL || esp_netif_get_ip_info(station,&s_info) != ESP_OK) return false;
    atomic_store(&s_station,station);
    atomic_store(&s_own,s_info.ip.addr);
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    wifi_get_ip(s_status.ip,sizeof(s_status.ip));
    wifi_current_ssid(s_status.ssid,sizeof(s_status.ssid));
    struct in_addr gateway = {.s_addr=s_info.gw.addr};
    inet_ntop(AF_INET,&gateway,s_status.gateway,sizeof(s_status.gateway));
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(station,ESP_NETIF_DNS_MAIN,&dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4)
    {
        struct in_addr server = {.s_addr=dns.ip.u_addr.ip4.addr};
        inet_ntop(AF_INET,&server,s_status.dns,sizeof(s_status.dns));
    }
    s_status.rssi = wifi_rssi();
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        s_status.channel = ap.primary;
        const char *security = "Non riconosciuta";
        switch (ap.authmode)
        {
            case WIFI_AUTH_OPEN: security = "Aperta - senza cifratura"; break;
            case WIFI_AUTH_WEP: security = "WEP - obsoleta"; break;
            case WIFI_AUTH_WPA_PSK: security = "WPA - obsoleta"; break;
            case WIFI_AUTH_WPA2_PSK: security = "WPA2-Personal"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: security = "WPA/WPA2 misto"; break;
            case WIFI_AUTH_WPA3_PSK: security = "WPA3-Personal"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: security = "WPA2/WPA3 misto"; break;
            case WIFI_AUTH_ENTERPRISE: security = "WPA2-Enterprise"; break;
            case WIFI_AUTH_OWE: security = "OWE"; break;
            default: break;
        }
        snprintf(s_status.security,sizeof(s_status.security),"%s",security);
    }
    xSemaphoreGive(sn_mutex);
    if (!s_mdns && mdns_init() == ESP_OK)
    { s_mdns = true; mdns_hostname_set("sentinel"); }
    if (sn_web_start() != ESP_OK) message("Rete connessa; dashboard non disponibile",-1);
    return true;
}

static bool prepare_profile(void)
{
    memset(s_gateway_mac,0,sizeof(s_gateway_mac));
    xQueueReset(s_replies);
    atomic_store(&s_capture,true);
    arp_request(ntohl(s_info.gw.addr),1);
    int64_t until = esp_timer_get_time()+500000;
    reply_t reply;
    while (esp_timer_get_time() < until)
    {
        if (xQueueReceive(s_replies,&reply,pdMS_TO_TICKS(25)) == pdTRUE && reply.ip == s_info.gw.addr)
            memcpy(s_gateway_mac,reply.mac,6);
    }
    char ssid[33], gateway[16], mac[18] = "", subnet[24], key[160];
    wifi_current_ssid(ssid,sizeof(ssid));
    struct in_addr ip = {.s_addr=s_info.gw.addr};
    inet_ntop(AF_INET,&ip,gateway,sizeof(gateway));
    uint32_t mask = ntohl(s_info.netmask.addr);
    int prefix = __builtin_popcount(mask);
    ip.s_addr = s_info.ip.addr & s_info.netmask.addr;
    char base[16]; inet_ntop(AF_INET,&ip,base,sizeof(base));
    snprintf(subnet,sizeof(subnet),"%s/%d",base,prefix);
    bool gateway_known = memcmp(s_gateway_mac,"\0\0\0\0\0\0",6) != 0;
    if (gateway_known) format_mac(s_gateway_mac,mac);
    else
    {
        wifi_ap_record_t access_point;
        if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) format_mac(access_point.bssid,mac);
    }
    snprintf(key,sizeof(key),"%s|%s|%s|%s",ssid,subnet,gateway_known ? "gw" : "ap",mac);
    unsigned char digest[32];
    if (!sn_digest(key,strlen(key),digest)) return false;
    for (int i = 0; i < 16; i++) snprintf(s_current_network+i*2,3,"%02x",digest[i]);
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    sn_network_t *network = NULL;
    for (int i = 0; i < sn_model->network_count; i++)
        if (strcmp(sn_model->networks[i].id,s_current_network) == 0) network = &sn_model->networks[i];
    if (network == NULL && sn_model->network_count < SN_NETWORKS)
    {
        network = &sn_model->networks[sn_model->network_count++];
        memset(network,0,sizeof(*network));
        snprintf(network->id,sizeof(network->id),"%s",s_current_network);
        snprintf(network->label,sizeof(network->label),"%s",ssid);
    }
    if (network != NULL)
    {
        snprintf(network->ssid,sizeof(network->ssid),"%s",ssid);
        snprintf(network->subnet,sizeof(network->subnet),"%s",subnet);
        snprintf(network->gateway,sizeof(network->gateway),"%s",gateway);
        snprintf(network->gateway_mac,sizeof(network->gateway_mac),"%s",gateway_known ? mac : "");
        network->last_seen = now();
        snprintf(s_status.network,sizeof(s_status.network),"%s",s_current_network);
        snprintf(s_status.subnet,sizeof(s_status.subnet),"%s",subnet);
        s_status.dirty = true;
    }
    xSemaphoreGive(sn_mutex);
    if (gateway_known) observe(s_info.gw.addr,s_gateway_mac,SN_ARP,-1,NULL,"Gateway");
    return network != NULL;
}

static bool scope(const char *cidr, uint32_t *first, uint32_t *count)
{
    uint32_t mask = ntohl(s_info.netmask.addr), own = ntohl(s_info.ip.addr);
    int prefix = __builtin_popcount(mask);
    uint32_t base;
    if (prefix < 1 || prefix > 30 || mask != (UINT32_MAX << (32-prefix))) return false;
    if (cidr == NULL || *cidr == 0)
    {
        if (prefix < 24) prefix = 24;
        base = own & (UINT32_MAX << (32-prefix));
    }
    else
    {
        char address[24]; snprintf(address,sizeof(address),"%s",cidr);
        char *slash = strchr(address,'/'), *end;
        if (slash == NULL) return false;
        *slash++ = 0;
        long parsed = strtol(slash,&end,10);
        if (*end || parsed < 22 || parsed > 30) return false;
        prefix = parsed;
        struct in_addr ip;
        if (inet_pton(AF_INET,address,&ip) != 1) return false;
        base = ntohl(ip.s_addr) & (UINT32_MAX << (32-prefix));
    }
    uint32_t hosts = (1u << (32-prefix))-2;
    if ((base & mask) != (own & mask) || ((base+hosts+1) & mask) != (own & mask)) return false;
    *first = base+1; *count = hosts;
    struct in_addr ip = {.s_addr=htonl(base)};
    char address[16]; inet_ntop(AF_INET,&ip,address,sizeof(address));
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    snprintf(s_status.range,sizeof(s_status.range),"%s/%d",address,prefix);
    xSemaphoreGive(sn_mutex);
    return true;
}

static void discover_mdns(void)
{
    if (!s_mdns) return;
    static const char *types[] = {"_http","_https","_ipp","_smb","_googlecast","_hap","_mqtt","_workstation"};
    for (unsigned i = 0; i < sizeof(types)/sizeof(types[0]) && !atomic_load(&s_cancel); i++)
    {
        mdns_result_t *results = NULL;
        if (mdns_query_ptr(types[i],"_tcp",400,16,&results) != ESP_OK) continue;
        for (mdns_result_t *r = results; r != NULL; r = r->next)
        {
            if (r->esp_netif != atomic_load(&s_station)) continue;
            char service[64]; snprintf(service,sizeof(service),"%s:%u",types[i],r->port);
            for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next)
                if (a->addr.type == ESP_IPADDR_TYPE_V4)
                    observe(a->addr.u_addr.ip4.addr,NULL,SN_MDNS,-1,r->hostname,service);
        }
        mdns_query_results_free(results);
        drain_arp();
    }
}

static void discover_ssdp(void)
{
    int fd = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (fd < 0) return;
    struct in_addr own = {.s_addr=s_info.ip.addr};
    setsockopt(fd,IPPROTO_IP,IP_MULTICAST_IF,&own,sizeof(own));
    int ttl = 2; setsockopt(fd,IPPROTO_IP,IP_MULTICAST_TTL,&ttl,sizeof(ttl));
    struct timeval timeout = {.tv_usec=100000};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    struct sockaddr_in target = {.sin_family=AF_INET,.sin_port=htons(1900)};
    inet_pton(AF_INET,"239.255.255.250",&target.sin_addr);
    const char query[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ssdp:all\r\n\r\n";
    sendto(fd,query,sizeof(query)-1,0,(struct sockaddr *)&target,sizeof(target));
    int64_t until = esp_timer_get_time()+1500000;
    int responses = 0;
    while (esp_timer_get_time() < until && responses < 128 && !atomic_load(&s_cancel))
    {
        char buffer[1200]; struct sockaddr_in source; socklen_t size = sizeof(source);
        int length = recvfrom(fd,buffer,sizeof(buffer)-1,0,(struct sockaddr *)&source,&size);
        if (length < 12) continue;
        buffer[length] = 0;
        if (strncmp(buffer,"HTTP/1.1 200",12) != 0 && strncmp(buffer,"HTTP/1.0 200",12) != 0) continue;
        observe(source.sin_addr.s_addr,NULL,SN_SSDP,-1,NULL,"SSDP/UPnP");
        responses++;
    }
    close(fd);
    drain_arp();
}

static void scan(const char *cidr)
{
    /* Failed attempts also respect the configured interval. */
    s_next_scan = esp_timer_get_time()+(int64_t)sn_model->interval*1000000;
    if (sn_store_corrupt())
    { message("Archivio danneggiato: importa un backup prima di scansionare",0); return; }
    message("Preparazione scansione IPv4...",0);
    if (!connect_network()) return;
    uint32_t first, count;
    if (!scope(cidr,&first,&count)) { message("Intervallo non locale: usa /22 ... /30",0); return; }
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    sn_model->cycle++;
    if (sn_model->cycle == 0) sn_model->cycle++;
    s_status.cycle = sn_model->cycle;
    s_status.new_devices = 0; s_status.observed = 0;
    xSemaphoreGive(sn_mutex);
    if (!prepare_profile())
    { atomic_store(&s_capture,false); message("Limite reti raggiunto",0); return; }
    int fd = socket(AF_INET,SOCK_RAW,IPPROTO_ICMP);
    s_ping_id = (uint16_t)esp_random(); s_ping_nonce = esp_random();
    for (uint32_t index = 0; index < count && !atomic_load(&s_cancel); index += 12)
    {
        uint32_t batch = count-index < 12 ? count-index : 12;
        if (!wifi_is_connected()) { message("Connessione persa: risultati parziali",-1); atomic_store(&s_cancel,true); break; }
        arp_request(first+index,batch);
        for (uint32_t j = 0; j < batch; j++)
        {
            if (fd >= 0) echo_send(fd,htonl(first+index+j),(uint16_t)(index+j));
            vTaskDelay(pdMS_TO_TICKS(8));
        }
        int64_t until = esp_timer_get_time()+180000;
        while (esp_timer_get_time() < until)
        {
            drain_arp();
            if (fd >= 0) echo_read(fd,0,-1,true);
            vTaskDelay(pdMS_TO_TICKS(15));
        }
        message("Rilevo risposte ARP e ICMP...",(index+batch)*80/count);
    }
    if (fd >= 0) { echo_read(fd,0,-1,true); close(fd); }
    if (!atomic_load(&s_cancel))
    {
        message("Cerco servizi mDNS...",82); discover_mdns();
        message("Cerco annunci SSDP...",94); discover_ssdp();
    }
    drain_arp();
    atomic_store(&s_capture,false);
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    for (int i = 0; i < sn_model->device_count; i++)
        if (sn_model->devices[i].seen_cycle == sn_model->cycle && sn_confirmed(&sn_model->devices[i])) s_status.observed++;
    xSemaphoreGive(sn_mutex);
    bool cancelled = atomic_load(&s_cancel);
    if (save()) message(cancelled ? "Interrotta: risultati parziali salvati" : "Scansione completata e salvata",cancelled ? -1 : 100);
    s_next_scan = esp_timer_get_time()+(int64_t)sn_model->interval*1000000;
}

static bool port_open(uint32_t ip, uint16_t port)
{
    int fd = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (fd < 0) return false;
    fcntl(fd,F_SETFL,O_NONBLOCK);
    struct sockaddr_in target = {.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=ip};
    int result = connect(fd,(struct sockaddr *)&target,sizeof(target));
    bool ok = result == 0;
    if (!ok && errno == EINPROGRESS)
    {
        fd_set writes; FD_ZERO(&writes); FD_SET(fd,&writes);
        struct timeval timeout = {.tv_usec=250000};
        if (select(fd+1,NULL,&writes,NULL,&timeout) > 0)
        {
            int error = 0; socklen_t length = sizeof(error);
            ok = getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&length) == 0 && error == 0;
        }
    }
    close(fd);
    return ok;
}

static void diagnose(const char *text)
{
    if (!connect_network()) return;
    struct in_addr ip;
    if (inet_pton(AF_INET,text,&ip) != 1 || !local_ip(ip.s_addr))
    { message("Diagnostica limitata alla LAN collegata",0); return; }
    int fd = socket(AF_INET,SOCK_RAW,IPPROTO_ICMP), sent = 0, received = 0, total = 0;
    int minimum = 60000, maximum = 0;
    s_ping_id = (uint16_t)esp_random(); s_ping_nonce = esp_random();
    for (int i = 0; i < 5 && fd >= 0 && !atomic_load(&s_cancel); i++)
    {
        if (!echo_send(fd,ip.s_addr,i)) continue;
        sent++;
        int64_t until = esp_timer_get_time()+500000;
        int rtt = -1;
        while (esp_timer_get_time() < until && rtt < 0)
        { rtt = echo_read(fd,ip.s_addr,i,false); vTaskDelay(pdMS_TO_TICKS(10)); }
        if (rtt >= 0)
        {
            received++; total += rtt;
            if (rtt < minimum) minimum = rtt;
            if (rtt > maximum) maximum = rtt;
            observe(ip.s_addr,NULL,SN_ICMP,rtt,NULL,NULL);
        }
        message("Misuro latenza e perdita ICMP...",(i+1)*10);
    }
    if (fd >= 0) close(fd);
    static const uint16_t ports[] = {22,80,443,445,1883,3389,8080,8123};
    char opened[96] = "";
    for (unsigned i = 0; i < sizeof(ports)/sizeof(ports[0]) && !atomic_load(&s_cancel); i++)
    {
        if (port_open(ip.s_addr,ports[i]))
        {
            char service[20]; snprintf(service,sizeof(service),"TCP %u",ports[i]);
            observe(ip.s_addr,NULL,SN_TCP,-1,NULL,service);
            size_t used = strlen(opened); snprintf(opened+used,sizeof(opened)-used,"%s%u",used ? ", " : "",ports[i]);
        }
    }
    struct addrinfo hints = {.ai_family=AF_INET,.ai_socktype=SOCK_STREAM}, *answer = NULL;
    int64_t started = esp_timer_get_time();
    int dns = getaddrinfo("example.com",NULL,&hints,&answer);
    int dns_ms = (esp_timer_get_time()-started)/1000;
    if (answer) freeaddrinfo(answer);
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    snprintf(s_status.diagnostic,sizeof(s_status.diagnostic),
        "%s\nICMP %d/%d risposte; perdita %d%% (-1 = non misurata)\nMedia %d ms; min %d; max %d\nTCP aperte: %s\nDNS example.com: %s (%d ms)\nICMP filtrato non prova un guasto. Porte: raggiungibili, non vulnerabilita.",
        text,received,sent,sent ? (sent-received)*100/sent : -1,received ? total/received : -1,received ? minimum : -1,received ? maximum : -1,
        opened[0] ? opened : "nessuna rilevata",dns == 0 ? "OK" : "errore",dns_ms);
    s_status.revision++;
    xSemaphoreGive(sn_mutex);
    if (save()) message(atomic_load(&s_cancel) ? "Diagnostica interrotta" : "Diagnostica conclusa",100);
}

static void merge(sn_model_t *incoming)
{
    sn_model_t *candidate = heap_caps_malloc(sizeof(*candidate),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (candidate == NULL) { message("RAM insufficiente per importare",0); return; }
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    *candidate = *sn_model;
    xSemaphoreGive(sn_mutex);
    bool ok = true;
    int added = 0;
    for (int i = 0; i < incoming->network_count && ok; i++)
    {
        bool exists = false;
        for (int j = 0; j < candidate->network_count; j++)
            if (strcmp(incoming->networks[i].id,candidate->networks[j].id) == 0) exists = true;
        if (!exists)
        {
            if (candidate->network_count >= SN_NETWORKS) ok = false;
            else candidate->networks[candidate->network_count++] = incoming->networks[i];
        }
    }
    for (int i = 0; i < incoming->device_count && ok; i++)
    {
        sn_device_t *existing = NULL;
        for (int j = 0; j < candidate->device_count; j++)
        {
            sn_device_t *a = &incoming->devices[i], *b = &candidate->devices[j];
            if (strcmp(a->id,b->id) == 0 ||
                (a->mac[0] && !(a->sources & SN_PROXY) && !(b->sources & SN_PROXY) &&
                 strcmp(a->network,b->network) == 0 && strcmp(a->mac,b->mac) == 0)) existing = b;
        }
        if (existing == NULL)
        {
            if (candidate->device_count >= SN_DEVICES) ok = false;
            else { candidate->devices[candidate->device_count++] = incoming->devices[i]; added++; }
        }
        else if (strcmp(existing->network,incoming->devices[i].network) != 0) ok = false;
        else
        {
            /* Fill missing metadata when sharing archives between devices.
             * Existing explicit choices and trust decisions take precedence. */
            if (!existing->label[0]) snprintf(existing->label,sizeof(existing->label),"%s",incoming->devices[i].label);
            if (!existing->category[0]) snprintf(existing->category,sizeof(existing->category),"%s",incoming->devices[i].category);
            if (!existing->note[0]) snprintf(existing->note,sizeof(existing->note),"%s",incoming->devices[i].note);
        }
    }
    candidate->generation++;
    if (ok) ok = sn_store_restore(candidate);
    if (ok)
    {
        xSemaphoreTake(sn_mutex,portMAX_DELAY);
        *sn_model = *candidate;
        s_status.persistent = true; s_status.dirty = false;
        xSemaphoreGive(sn_mutex);
        char text[80]; snprintf(text,sizeof(text),"Importati %d dispositivi; dati esistenti conservati",added);
        message(text,100);
    }
    else message("Importazione annullata: limiti, conflitti o SD",0);
    free(candidate);
}

static void worker(void *unused)
{
    (void)unused;
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    /* Readers use try-lock: they skip a frame rather than seeing a partial
     * startup snapshot. The dashboard starts after this load completes. */
    sn_store_load(sn_model);
    s_status.persistent = storage_dir() != NULL && !sn_store_corrupt();
    xSemaphoreGive(sn_mutex);
    bool connected = connect_network();
    if (sn_store_corrupt()) message("Archivio SD danneggiato: esporta i file e ripristina un backup. Scrittura bloccata.",0);
    else if (connected) scan(""); /* First observation runs in this worker, never in LVGL. */
    atomic_store(&s_busy,false);
    for (;;)
    {
        command_t command;
        if (xQueueReceive(s_commands,&command,pdMS_TO_TICKS(1000)) != pdTRUE)
        {
            if (sn_model->interval && esp_timer_get_time() >= s_next_scan)
            {
                bool expected = false;
                if (atomic_compare_exchange_strong(&s_busy,&expected,true))
                { atomic_store(&s_cancel,false); scan(""); atomic_store(&s_busy,false); }
            }
            continue;
        }
        if (command.kind == SCAN) scan(command.target);
        else if (command.kind == DIAGNOSE) diagnose(command.target);
        else if (command.kind == EDIT)
        {
            bool found = false;
            xSemaphoreTake(sn_mutex,portMAX_DELAY);
            for (int i = 0; i < sn_model->device_count; i++)
            {
                sn_device_t *d = &sn_model->devices[i];
                if (strcmp(d->id,command.device->id) != 0) continue;
                snprintf(d->label,sizeof(d->label),"%s",command.device->label);
                snprintf(d->category,sizeof(d->category),"%s",command.device->category);
                snprintf(d->note,sizeof(d->note),"%s",command.device->note);
                d->trusted = command.device->trusted;
                s_status.dirty = true; found = true;
            }
            xSemaphoreGive(sn_mutex);
            if (found && save()) message("Nome e categoria salvati",100);
            else if (!found) message("Dispositivo non trovato",0);
        }
        else if (command.kind == SETTINGS)
        {
            xSemaphoreTake(sn_mutex,portMAX_DELAY);
            sn_model->interval = command.interval; s_status.dirty = true;
            xSemaphoreGive(sn_mutex);
            s_next_scan = esp_timer_get_time()+(int64_t)command.interval*1000000;
            if (save()) message(command.interval ? "Monitoraggio periodico attivo" : "Scansione manuale",100);
        }
        else if (command.kind == IMPORT) merge(command.incoming);
#if CONFIG_WATCH_DEV_CONSOLE
        else if (command.kind == SELFTEST)
        {
            bool ok = sn_store_selftest();
            printf("@NETSELFTEST %s\n",ok ? "PASS" : "FAIL");
            fflush(stdout);
            message(ok ? "Test archivio 1024 record: OK" : "Test archivio fallito",100);
        }
#endif
        else if (command.kind == EXIT)
        {
            if (command.discard || !s_status.dirty || save())
            {
                atomic_store(&s_capture,false);
                sn_web_stop();
                if (s_mdns) mdns_free();
                if (s_wifi_held) wifi_release();
                extapp_return_to_launcher();
            }
            else message("SD non disponibile: salva o conferma uscita senza salvare",0);
        }
        free(command.device);
        free(command.incoming);
        atomic_store(&s_busy,false);
        xSemaphoreTake(sn_mutex,portMAX_DELAY);
        s_status.revision++;
        xSemaphoreGive(sn_mutex);
        sys_heap_log("Sentinel job complete");
    }
}

static esp_err_t submit(command_t *command)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy,&expected,true)) return ESP_ERR_INVALID_STATE;
    atomic_store(&s_cancel,false);
    if (xQueueSend(s_commands,command,0) != pdTRUE)
    { atomic_store(&s_busy,false); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

esp_err_t sn_scan(const char *cidr)
{
    command_t command = {.kind=SCAN};
    if (cidr && strlen(cidr) >= sizeof(command.target)) return ESP_ERR_INVALID_ARG;
    snprintf(command.target,sizeof(command.target),"%s",cidr ? cidr : "");
    return submit(&command);
}
esp_err_t sn_diagnose(const char *ip)
{
    if (ip == NULL || strlen(ip) >= 16) return ESP_ERR_INVALID_ARG;
    command_t command = {.kind=DIAGNOSE};
    snprintf(command.target,sizeof(command.target),"%s",ip);
    return submit(&command);
}
esp_err_t sn_edit(const sn_device_t *device)
{
    if (sn_store_corrupt()) return ESP_ERR_INVALID_STATE;
    if (!device || !sn_text_valid(device->label,64) || !sn_text_valid(device->category,32) || !sn_text_valid(device->note,256)) return ESP_ERR_INVALID_ARG;
    command_t command = {.kind=EDIT,.device=malloc(sizeof(*device))};
    if (!command.device) return ESP_ERR_NO_MEM;
    *command.device = *device;
    esp_err_t result = submit(&command);
    if (result != ESP_OK) free(command.device);
    return result;
}
esp_err_t sn_settings(uint32_t interval)
{
    if (sn_store_corrupt()) return ESP_ERR_INVALID_STATE;
    if (interval > 3600 || (interval && interval < 30)) return ESP_ERR_INVALID_ARG;
    command_t command = {.kind=SETTINGS,.interval=interval}; return submit(&command);
}
esp_err_t sn_import(sn_model_t *incoming)
{
    if (incoming == NULL) return ESP_ERR_INVALID_ARG;
    command_t command = {.kind=IMPORT,.incoming=incoming}; return submit(&command);
}
esp_err_t sn_exit(bool discard)
{
    command_t command = {.kind=EXIT,.discard=discard}; return submit(&command);
}
void sn_cancel(void) { atomic_store(&s_cancel,true); }

#if CONFIG_WATCH_DEV_CONSOLE
esp_err_t sn_selftest(void)
{
    command_t command = {.kind=SELFTEST}; return submit(&command);
}
#endif

static void *json_allocate(size_t size)
{ return heap_caps_malloc(size,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

esp_err_t sn_init(void)
{
    cJSON_Hooks hooks = {.malloc_fn=json_allocate,.free_fn=free};
    cJSON_InitHooks(&hooks);
    sn_mutex = xSemaphoreCreateMutex();
    s_commands = xQueueCreate(3,sizeof(command_t)); s_replies = xQueueCreate(128,sizeof(reply_t));
    sn_model = heap_caps_calloc(1,sizeof(*sn_model),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sn_mutex || !s_commands || !s_replies || !sn_model) return ESP_ERR_NO_MEM;
    snprintf(s_status.code,sizeof(s_status.code),"%08lu",(unsigned long)(esp_random()%100000000));
    atomic_store(&s_busy,true);
    return sys_task_create(worker,"sentinel",8192,NULL,3,SYS_CORE_ANY) ? ESP_OK : ESP_ERR_NO_MEM;
}
