/* Who is behind an IP? Observed answers (router DNS name, NetBIOS name, mDNS
 * name, UPnP description, open TCP ports) are stored with the device; the
 * "probable type" is a deduction computed on demand and always shown as such. */
#include "internal.h"
#include "packet.h"
#include "oui.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parse_mac(const char *text, uint8_t mac[6])
{
    unsigned v[6];
    if (strlen(text) != 17 || sscanf(text,"%2x:%2x:%2x:%2x:%2x:%2x",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
    return true;
}

bool sn_private_mac(const sn_device_t *device)
{
    uint8_t mac[6];
    return parse_mac(device->mac,mac) && (mac[0] & 2) != 0; /* locally administered bit */
}

void sn_vendor(const sn_device_t *device, char *out, size_t size)
{
    uint8_t mac[6];
    if (!parse_mac(device->mac,mac)) snprintf(out,size,"MAC non rilevato");
    else if (mac[0] & 2) snprintf(out,size,"MAC privato (casuale)");
    else
    {
        const char *vendor = sn_oui_vendor(mac);
        snprintf(out,size,"%s",vendor ? vendor : "Produttore non registrato");
    }
}

const char *sn_display_name(const sn_device_t *d)
{
    if (d->label[0]) return d->label;
    if (d->hostname[0]) return d->hostname;
    if (d->netbios[0]) return d->netbios;
    if (d->dns[0]) return d->dns;
    if (d->model[0]) return d->model;
    return d->ip;
}

static bool contains(const char *text, const char *needle)
{
    size_t length = strlen(needle);
    for (; *text; text++)
    {
        size_t i = 0;
        while (i < length && text[i] && tolower((unsigned char)text[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == length) return true;
    }
    return false;
}

static bool named(const sn_device_t *d, const char *needle)
{
    return contains(d->label,needle) || contains(d->hostname,needle) || contains(d->dns,needle) ||
           contains(d->netbios,needle) || contains(d->model,needle);
}

/* Ordered rules: the strongest, most specific evidence first. */
bool sn_guess(const sn_device_t *d, bool gateway, char *type, size_t type_size, char *reasons, size_t size)
{
    char vendor[48];
    sn_vendor(d,vendor,sizeof(vendor));
    const char *s = d->services;
    bool apple = contains(vendor,"Apple");
#define RESULT(kind, ...) do { snprintf(type,type_size,"%s",kind); snprintf(reasons,size,__VA_ARGS__); return true; } while (0)
    if (gateway || contains(s,"Gateway")) RESULT("Router / gateway","e il gateway della rete (%s)",vendor);
    if (contains(s,"TCP 62078")) RESULT("iPhone o iPad","porta 62078 aperta: servizio di sincronizzazione dei dispositivi iOS");
    if (named(d,"iphone")) RESULT("iPhone","il nome annunciato contiene \"iPhone\"");
    if (named(d,"ipad")) RESULT("iPad","il nome annunciato contiene \"iPad\"");
    if (contains(s,"_googlecast") || contains(s,"TCP 8009")) RESULT("Chromecast, Google TV o altoparlante Google","servizio Google Cast annunciato");
    if (contains(s,"_airplay") || contains(s,"_raop"))
        RESULT(apple ? "Apple TV, HomePod o Mac" : "TV o altoparlante AirPlay","annuncia AirPlay%s",apple ? "; MAC Apple" : "");
    if (contains(s,"_sonos") || (contains(vendor,"Sonos"))) RESULT("Altoparlante Sonos","%s",contains(s,"_sonos") ? "servizio Sonos annunciato" : "MAC Sonos");
    if (contains(s,"_ipp") || contains(s,"_printer") || contains(s,"_pdl-datastream") || contains(s,"TCP 9100") || contains(s,"TCP 631"))
        RESULT("Stampante","servizio di stampa (IPP o porta 9100) disponibile");
    if (contains(s,"_home-assistant") || contains(s,"TCP 8123")) RESULT("Home Assistant","servizio Home Assistant (porta 8123)");
    if (contains(s,"TCP 7000") && !apple) RESULT("Smart TV o ricevitore AirPlay","porta 7000 (AirPlay) aperta");
    if (contains(s,"TCP 554") || contains(s,"_rtsp")) RESULT("Videocamera IP o registratore","flusso video RTSP (porta 554) disponibile");
    if (contains(s,"_hap")) RESULT("Accessorio domotico HomeKit","annuncia HomeKit (_hap)");
    if (contains(s,"_amzn") || named(d,"echo") || named(d,"fire tv")) RESULT("Dispositivo Amazon (Echo / Fire TV)","servizio o nome Amazon");
    if (contains(vendor,"Amazon")) RESULT("Dispositivo Amazon (Echo, Fire TV, Kindle)","MAC registrato da Amazon");
    if (named(d,"android") || named(d,"galaxy") || named(d,"pixel") || named(d,"redmi") || named(d,"oneplus"))
        RESULT("Smartphone o tablet Android","il nome annunciato e tipico di Android");
    if (named(d,"macbook") || named(d,"imac") || named(d,"mac-mini") || named(d,"macmini")) RESULT("Mac","il nome annunciato e tipico di un Mac");
    if (named(d,"desktop-") || named(d,"laptop-")) RESULT("PC Windows","nome Windows predefinito (%s)",d->netbios[0] ? d->netbios : d->hostname);
    if (d->netbios[0] || contains(s,"TCP 3389")) RESULT("PC Windows o server Samba","%s",d->netbios[0] ? "risponde con un nome NetBIOS" : "Desktop remoto (porta 3389)");
    if (apple && (contains(s,"_smb") || contains(s,"_afpovertcp") || contains(s,"TCP 548") || contains(s,"_ssh")))
        RESULT("Mac","MAC Apple con condivisione file o SSH");
    if (named(d,"tv") || named(d,"bravia") || named(d,"webos") || named(d,"tizen")) RESULT("Smart TV","il nome o il modello indica un televisore");
    if (contains(vendor,"Sony Interactive") || named(d,"ps5") || named(d,"ps4")) RESULT("Console PlayStation","MAC o nome Sony PlayStation");
    if (contains(vendor,"Nintendo")) RESULT("Console Nintendo","MAC registrato da Nintendo");
    if (named(d,"xbox")) RESULT("Console Xbox","il nome annunciato contiene \"Xbox\"");
    if (contains(vendor,"Raspberry")) RESULT("Raspberry Pi","MAC registrato da Raspberry Pi");
    if (contains(vendor,"Espressif")) RESULT("Dispositivo smart con chip Espressif","MAC Espressif: prese, luci, sensori, termostati");
    if (contains(vendor,"Signify") || contains(vendor,"Philips Lighting")) RESULT("Bridge o luce Philips Hue","MAC Signify (Philips Hue)");
    if (contains(vendor,"Allterco") || named(d,"shelly")) RESULT("Dispositivo Shelly","MAC o nome Shelly");
    if (contains(vendor,"Tuya")) RESULT("Dispositivo smart Tuya","MAC Tuya");
    if (contains(s,"_mqtt") || contains(s,"TCP 1883")) RESULT("Server MQTT / domotica","broker MQTT (porta 1883)");
    if (contains(s,"_smb") || contains(s,"TCP 445")) RESULT("Computer o NAS","condivisione file SMB (porta 445)");
    if (d->model[0]) RESULT("Dispositivo UPnP","si descrive come \"%s\"",d->model);
    if (sn_private_mac(d)) RESULT("Smartphone, tablet o PC recente","MAC privato casuale: tipico di iOS, Android e Windows 10/11");
    if (apple) RESULT("Dispositivo Apple","MAC registrato da Apple");
    if (contains(vendor,"Intel") || contains(vendor,"Realtek") || contains(vendor,"AzureWave") || contains(vendor,"Liteon") ||
        contains(vendor,"Hon Hai") || contains(vendor,"Dell") || contains(vendor,"Lenovo") || contains(vendor,"Hewlett"))
        RESULT("Computer o portatile","scheda di rete %s, tipica dei PC",vendor);
    if (contains(vendor,"Bilian") || contains(vendor,"Tuya") || contains(vendor,"Hui Zhou Gaoshengda") || contains(vendor,"Fn-Link"))
        RESULT("Modulo Wi-Fi generico (IoT, TV box, telecamere)","scheda di rete %s, comune in dispositivi economici",vendor);
    if (strcmp(vendor,"MAC non rilevato") != 0 && strcmp(vendor,"Produttore non registrato") != 0)
        RESULT("Non determinato","solo il produttore della scheda di rete: %s",vendor);
#undef RESULT
    snprintf(type,type_size,"Non determinato");
    snprintf(reasons,size,"nessun indizio: prova Identifica");
    return false;
}

/* ---- Batch name lookups: one UDP socket, all queries out, answers matched by id. */

typedef struct
{
    uint32_t ip;
    char dns[65], netbios[17], mdns[65];
} lookup_t;

static void drain(int fd, lookup_t *targets, int count)
{
    uint8_t buffer[600];
    struct sockaddr_in from;
    socklen_t size = sizeof(from);
    int length;
    while ((length = recvfrom(fd,buffer,sizeof(buffer),MSG_DONTWAIT,(struct sockaddr *)&from,&size)) > 0)
    {
        uint16_t port = ntohs(from.sin_port), id = 0;
        char name[65];
        if (port == 53 || port == 5353)
        {
            if (!sn_dns_ptr_answer(buffer,length,&id,name,sizeof(name))) { size = sizeof(from); continue; }
            int i = id & 0x3fff;
            if (i < count && (id & 0xc000) == 0x4000 && port == 53 && !contains(name,"in-addr.arpa"))
                snprintf(targets[i].dns,sizeof(targets[i].dns),"%s",name);
            else if (i < count && (id & 0xc000) == 0x8000 && port == 5353 && from.sin_addr.s_addr == targets[i].ip &&
                     name[0] != '(') /* "(none).local": Linux without a hostname */
                snprintf(targets[i].mdns,sizeof(targets[i].mdns),"%s",name);
        }
        else if (port == 137)
        {
            for (int i = 0; i < count; i++)
                if (targets[i].ip == from.sin_addr.s_addr &&
                    sn_nbstat_name(buffer,length,(uint16_t)(0xc000 | i),name,sizeof(targets[i].netbios)))
                    snprintf(targets[i].netbios,sizeof(targets[i].netbios),"%.16s",name);
        }
        size = sizeof(from);
    }
}

static void query_names(lookup_t *targets, int count, int wait_ms)
{
    int fd = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (fd < 0) return;
    uint32_t dns = sn_dns_server();
    if (dns == 0) dns = sn_ip_info()->gw.addr;
    uint8_t packet[64];
    for (int i = 0; i < count && i < 0x4000 && !sn_cancelled(); i++)
    {
        struct sockaddr_in to = {.sin_family=AF_INET};
        size_t length = sn_dns_ptr_query(packet,sizeof(packet),(uint16_t)(0x4000 | i),targets[i].ip);
        to.sin_addr.s_addr = dns; to.sin_port = htons(53);
        sendto(fd,packet,length,0,(struct sockaddr *)&to,sizeof(to));
        length = sn_nbstat_query(packet,sizeof(packet),(uint16_t)(0xc000 | i));
        to.sin_addr.s_addr = targets[i].ip; to.sin_port = htons(137);
        sendto(fd,packet,length,0,(struct sockaddr *)&to,sizeof(to));
        /* Legacy unicast mDNS: Apple, Android and Avahi answer a reverse query sent to port 5353. */
        length = sn_dns_ptr_query(packet,sizeof(packet),(uint16_t)(0x8000 | i),targets[i].ip);
        to.sin_port = htons(5353);
        sendto(fd,packet,length,0,(struct sockaddr *)&to,sizeof(to));
        if ((i & 7) == 7) { drain(fd,targets,count); vTaskDelay(pdMS_TO_TICKS(20)); }
    }
    int64_t until = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    while (esp_timer_get_time() < until && !sn_cancelled())
    {
        drain(fd,targets,count);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    drain(fd,targets,count);
    close(fd);
}

static void apply_names(const lookup_t *targets, int count)
{
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    for (int i = 0; i < count; i++)
    {
        sn_device_t *d = sn_device_by_ip_locked(targets[i].ip);
        if (d == NULL) continue;
        if (targets[i].dns[0]) snprintf(d->dns,sizeof(d->dns),"%s",targets[i].dns);
        if (targets[i].netbios[0]) snprintf(d->netbios,sizeof(d->netbios),"%s",targets[i].netbios);
        if (targets[i].mdns[0] && !d->hostname[0]) snprintf(d->hostname,sizeof(d->hostname),"%s",targets[i].mdns);
    }
    sn_touch_locked();
    xSemaphoreGive(sn_mutex);
}

void sn_lookup_names(void)
{
    lookup_t *targets = heap_caps_calloc(256,sizeof(lookup_t),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (targets == NULL) return;
    int count = 0;
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    for (int i = 0; i < sn_model->device_count && count < 256; i++)
    {
        const sn_device_t *d = &sn_model->devices[i];
        struct in_addr ip;
        if (d->seen_cycle != sn_model->cycle || !sn_confirmed(d) || inet_pton(AF_INET,d->ip,&ip) != 1) continue;
        if (sn_device_by_ip_locked(ip.s_addr) != d) continue; /* only the network we are on */
        targets[count++].ip = ip.s_addr;
    }
    xSemaphoreGive(sn_mutex);
    if (count) query_names(targets,count,1500);
    apply_names(targets,count);
    free(targets);
}

/* ---- Small bounded TCP helpers. */

static int connect_timeout(uint32_t ip, uint16_t port, int timeout_ms)
{
    int fd = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (fd < 0) return -1;
    fcntl(fd,F_SETFL,O_NONBLOCK);
    struct sockaddr_in target = {.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=ip};
    if (connect(fd,(struct sockaddr *)&target,sizeof(target)) != 0)
    {
        bool ok = false;
        if (errno == EINPROGRESS)
        {
            fd_set writes; FD_ZERO(&writes); FD_SET(fd,&writes);
            struct timeval timeout = {.tv_sec=timeout_ms/1000,.tv_usec=(timeout_ms%1000)*1000};
            int error = 0; socklen_t length = sizeof(error);
            ok = select(fd+1,NULL,&writes,NULL,&timeout) > 0 &&
                 getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&length) == 0 && error == 0;
        }
        if (!ok) { close(fd); return -1; }
    }
    return fd;
}

/* GET path from a LAN device: bounded size and time, plain HTTP only. */
static int http_get(uint32_t ip, uint16_t port, const char *path, char *buffer, size_t capacity, int timeout_ms)
{
    int fd = connect_timeout(ip,port,800);
    if (fd < 0) return -1;
    char request[256], host[16];
    inet_ntop(AF_INET,&ip,host,sizeof(host));
    int length = snprintf(request,sizeof(request),"GET %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: Sentinel\r\nConnection: close\r\n\r\n",
                          path,host,port);
    int used = -1;
    if (length > 0 && length < (int)sizeof(request) && send(fd,request,length,0) == length)
    {
        used = 0;
        int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        while (used < (int)capacity - 1 && esp_timer_get_time() < until)
        {
            fd_set reads; FD_ZERO(&reads); FD_SET(fd,&reads);
            struct timeval wait = {.tv_usec=100000};
            if (select(fd+1,&reads,NULL,NULL,&wait) <= 0) continue;
            int got = recv(fd,buffer+used,capacity-1-used,0);
            if (got <= 0) break;
            used += got;
        }
        buffer[used] = 0;
    }
    close(fd);
    return used;
}

static void join_text(char *out, size_t size, const char *a, const char *separator, const char *b)
{
    if (!b[0]) snprintf(out,size,"%s",a);
    else if (!a[0]) snprintf(out,size,"%s",b);
    else snprintf(out,size,"%s%s%s",a,separator,b);
}

void sn_upnp_describe(const sn_upnp_t *list, int count)
{
    char *xml = heap_caps_malloc(8192,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (xml == NULL) return;
    for (int i = 0; i < count && !sn_cancelled(); i++)
    {
        if (http_get(list[i].ip,list[i].port,list[i].path,xml,8192,1500) <= 0) continue;
        char friendly[65] = "", maker[49] = "", model[49] = "", product[100], text[97];
        sn_xml_value(xml,"friendlyName",friendly,sizeof(friendly));
        sn_xml_value(xml,"manufacturer",maker,sizeof(maker));
        sn_xml_value(xml,"modelName",model,sizeof(model));
        if (maker[0] && model[0] && contains(model,maker)) maker[0] = 0; /* "Sonos" + "Sonos One" */
        join_text(product,sizeof(product),maker," ",model);
        if (contains(friendly,product)) product[0] = 0;
        join_text(text,sizeof(text),friendly," - ",product);
        if (!text[0] || !sn_text_valid(text,96)) continue;
        xSemaphoreTake(sn_mutex,portMAX_DELAY);
        sn_device_t *d = sn_device_by_ip_locked(list[i].ip);
        if (d != NULL) { snprintf(d->model,sizeof(d->model),"%s",text); sn_touch_locked(); }
        xSemaphoreGive(sn_mutex);
    }
    free(xml);
}

/* Ask one host directly for its UPnP description location. */
static bool ssdp_unicast(uint32_t ip, sn_upnp_t *out)
{
    int fd = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (fd < 0) return false;
    struct sockaddr_in target = {.sin_family=AF_INET,.sin_port=htons(1900),.sin_addr.s_addr=ip};
    char host[16]; inet_ntop(AF_INET,&ip,host,sizeof(host));
    static const char *types[] = {"upnp:rootdevice","ssdp:all"};
    for (unsigned i = 0; i < 2; i++)
    {
        char query[200];
        int length = snprintf(query,sizeof(query),"M-SEARCH * HTTP/1.1\r\nHOST: %s:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n",host,types[i]);
        sendto(fd,query,length,0,(struct sockaddr *)&target,sizeof(target));
    }
    bool found = false;
    int64_t until = esp_timer_get_time() + 1500000;
    char *buffer = malloc(1200);
    while (buffer != NULL && !found && esp_timer_get_time() < until && !sn_cancelled())
    {
        fd_set reads; FD_ZERO(&reads); FD_SET(fd,&reads);
        struct timeval wait = {.tv_usec=100000};
        if (select(fd+1,&reads,NULL,NULL,&wait) <= 0) continue;
        struct sockaddr_in from; socklen_t size = sizeof(from);
        int length = recvfrom(fd,buffer,1199,0,(struct sockaddr *)&from,&size);
        if (length <= 0 || from.sin_addr.s_addr != ip) continue;
        buffer[length] = 0;
        out->ip = ip;
        found = sn_location(buffer,ip,&out->port,out->path,sizeof(out->path));
    }
    free(buffer);
    close(fd);
    return found;
}

static const uint16_t s_ports[] = {21,22,23,53,80,81,139,443,445,548,554,631,1400,1883,3389,5000,5357,
                                   7000,8000,8008,8009,8080,8123,8443,8888,9100,49152,62078};

/* Parallel connect: six sockets at a time, 500 ms per batch. */
static void scan_ports(uint32_t ip, bool open[])
{
    const int total = sizeof(s_ports)/sizeof(s_ports[0]);
    for (int start = 0; start < total && !sn_cancelled(); start += 6)
    {
        int fds[6], batch = total-start < 6 ? total-start : 6, highest = -1;
        fd_set pending; FD_ZERO(&pending);
        for (int j = 0; j < batch; j++)
        {
            fds[j] = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
            if (fds[j] < 0) continue;
            fcntl(fds[j],F_SETFL,O_NONBLOCK);
            struct sockaddr_in target = {.sin_family=AF_INET,.sin_port=htons(s_ports[start+j]),.sin_addr.s_addr=ip};
            if (connect(fds[j],(struct sockaddr *)&target,sizeof(target)) == 0) open[start+j] = true;
            else if (errno == EINPROGRESS) { FD_SET(fds[j],&pending); if (fds[j] > highest) highest = fds[j]; continue; }
            close(fds[j]); fds[j] = -1;
        }
        int64_t until = esp_timer_get_time() + 500000;
        while (highest >= 0 && esp_timer_get_time() < until)
        {
            fd_set writes = pending;
            int64_t left = until - esp_timer_get_time();
            struct timeval wait = {.tv_sec=0,.tv_usec=left > 0 ? (long)left : 0};
            if (select(highest+1,NULL,&writes,NULL,&wait) <= 0) break;
            bool any = false;
            for (int j = 0; j < batch; j++)
            {
                if (fds[j] < 0) continue;
                if (FD_ISSET(fds[j],&writes))
                {
                    int error = 0; socklen_t length = sizeof(error);
                    open[start+j] = getsockopt(fds[j],SOL_SOCKET,SO_ERROR,&error,&length) == 0 && error == 0;
                    FD_CLR(fds[j],&pending); close(fds[j]); fds[j] = -1;
                }
                else any = true;
            }
            if (!any) break;
        }
        for (int j = 0; j < batch; j++) if (fds[j] >= 0) close(fds[j]);
    }
}

void sn_run_identify(const char *text)
{
    if (!sn_connect()) return;
    struct in_addr ip;
    if (inet_pton(AF_INET,text,&ip) != 1 || !sn_local(ip.s_addr))
    { sn_message("Identificazione limitata alla rete collegata",0); return; }
    sn_message("Misuro la risposta...",5);
    int replies = 0, best = -1;
    for (int i = 0; i < 3 && !sn_cancelled(); i++)
    {
        int rtt = sn_ping(ip.s_addr,700);
        if (rtt >= 0) { replies++; if (best < 0 || rtt < best) best = rtt; }
    }
    if (best >= 0) sn_observe(ip.s_addr,SN_ICMP,best,NULL);
    sn_message("Chiedo il nome: DNS del router, NetBIOS, mDNS...",20);
    lookup_t target = {.ip=ip.s_addr};
    query_names(&target,1,1200);
    apply_names(&target,1);
    sn_message("Cerco una descrizione UPnP...",40);
    sn_upnp_t upnp;
    if (!sn_cancelled() && ssdp_unicast(ip.s_addr,&upnp)) sn_upnp_describe(&upnp,1);
    sn_message("Controllo 28 servizi TCP comuni...",60);
    bool open[sizeof(s_ports)/sizeof(s_ports[0])] = {false};
    scan_ports(ip.s_addr,open);
    char ports[160] = "";
    uint16_t web = 0;
    for (unsigned i = 0; i < sizeof(s_ports)/sizeof(s_ports[0]); i++)
    {
        if (!open[i]) continue;
        char service[16]; snprintf(service,sizeof(service),"TCP %u",s_ports[i]);
        sn_observe(ip.s_addr,SN_TCP,-1,service);
        size_t used = strlen(ports);
        snprintf(ports+used,sizeof(ports)-used,"%s%u",used ? ", " : "",s_ports[i]);
        if (!web && (s_ports[i] == 80 || s_ports[i] == 8080 || s_ports[i] == 8000 || s_ports[i] == 81)) web = s_ports[i];
    }
    char web_text[97] = "";
    if (web && !sn_cancelled())
    {
        sn_message("Leggo la pagina web del dispositivo...",85);
        char *page = heap_caps_malloc(6144,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (page != NULL && http_get(ip.s_addr,web,"/",page,6144,1500) > 0)
        {
            char title[65] = "", server[40] = "";
            sn_xml_value(page,"title",title,sizeof(title));
            sn_http_header(page,"Server",server,sizeof(server));
            if (title[0] && server[0]) snprintf(web_text,sizeof(web_text),"Web: %.56s (%.30s)",title,server);
            else if (title[0] || server[0]) snprintf(web_text,sizeof(web_text),"Web: %.64s",title[0] ? title : server);
        }
        free(page);
    }
    sn_device_t d = {0};
    bool known = false;
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    sn_device_t *stored = sn_device_by_ip_locked(ip.s_addr);
    if (stored != NULL)
    {
        if (web_text[0] && !stored->model[0]) snprintf(stored->model,sizeof(stored->model),"%s",web_text);
        d = *stored; known = true;
        sn_touch_locked();
    }
    xSemaphoreGive(sn_mutex);
    if (!known) snprintf(d.ip,sizeof(d.ip),"%s",text);
    char vendor[48], type[64], reasons[160], names[200] = "", report[1280];
    sn_vendor(&d,vendor,sizeof(vendor));
    sn_guess(&d,ip.s_addr == sn_ip_info()->gw.addr,type,sizeof(type),reasons,sizeof(reasons));
    if (d.hostname[0]) snprintf(names,sizeof(names),"mDNS %s",d.hostname);
    if (d.dns[0]) { size_t u = strlen(names); snprintf(names+u,sizeof(names)-u,"%sDNS %s",u ? " · " : "",d.dns); }
    if (d.netbios[0]) { size_t u = strlen(names); snprintf(names+u,sizeof(names)-u,"%sNetBIOS %s",u ? " · " : "",d.netbios); }
    snprintf(report,sizeof(report),
        "%s · %s\nProduttore: %s\nNomi: %s\nDescrizione: %s\nPorte TCP aperte: %s\nPing: %s%d ms (%d/3)\n\n"
        "Tipo probabile: %s\nIndizi: %s\nE una deduzione, non una certezza: conferma con il nome o spegnendo il dispositivo.",
        d.ip,d.mac[0] ? d.mac : "MAC non rilevato",vendor,names[0] ? names : "nessuno annunciato",
        d.model[0] ? d.model : "nessuna",ports[0] ? ports : "nessuna tra le 28 controllate",
        replies ? "" : "nessuna risposta, ",replies ? best : 0,replies,type,reasons);
    sn_report(report);
    if (sn_save()) sn_message(sn_cancelled() ? "Identificazione interrotta" : "Identificazione conclusa",100);
}
