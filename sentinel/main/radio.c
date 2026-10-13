/* Radio tools, all run on the Sentinel worker:
 * - Find: signal strength of the frames a device transmits, heard by the watch
 *   (promiscuous receive on our channel, transmitter address only, no payload kept).
 *   It tells "warmer / colder" while walking around, never a distance in metres.
 * - Quality: gateway ping, TCP connect to 1.1.1.1:443, DNS answer time, RSSI.
 * - Speed: HTTPS transfer of an explicit size with speed.cloudflare.com.
 * - Wi-Fi: nearby 2.4 GHz access points and switching network. */
#include "internal.h"
#include "core/power.h"
#include "services/wifi.h"
#include "packet.h"
#include "esp_crt_bundle.h"
#include "mbedtls/x509.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPEED_HOST "speed.cloudflare.com"

static sn_quality_t s_quality;
static sn_find_t s_find;
static sn_ap_t s_aps[SN_APS];
static int s_ap_count;
static uint8_t s_target[6];
static atomic_int s_last_rssi, s_frames;
static atomic_llong s_last_frame;

void sn_radio_init(void)
{
    s_quality.gateway_avg = s_quality.internet_avg = s_quality.dns_ms = -1;
    s_quality.download_kbps = s_quality.upload_kbps = -1;
}

bool sn_quality(sn_quality_t *out)
{
    if (sn_mutex == NULL || xSemaphoreTake(sn_mutex,0) != pdTRUE) return false;
    *out = s_quality;
    xSemaphoreGive(sn_mutex);
    return true;
}

bool sn_find_state(sn_find_t *out)
{
    if (sn_mutex == NULL || xSemaphoreTake(sn_mutex,0) != pdTRUE) return false;
    *out = s_find;
    xSemaphoreGive(sn_mutex);
    return true;
}

int sn_wifi_list(sn_ap_t *out, int max)
{
    if (sn_mutex == NULL || xSemaphoreTake(sn_mutex,0) != pdTRUE) return -1;
    int count = s_ap_count < max ? s_ap_count : max;
    memcpy(out,s_aps,sizeof(sn_ap_t)*count);
    xSemaphoreGive(sn_mutex);
    return count;
}

/* ---- Find ------------------------------------------------------------------ */

/* Wi-Fi task context: compare the transmitter address only, keep nothing else. */
static void sniff(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *packet = buffer;
    if ((type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) || packet->rx_ctrl.sig_len < 24) return;
    if (memcmp(packet->payload + 10,s_target,6) != 0) return;
    atomic_store(&s_last_rssi,packet->rx_ctrl.rssi);
    atomic_fetch_add(&s_frames,1);
    atomic_store(&s_last_frame,esp_timer_get_time());
}

void sn_run_find(const char *device_id)
{
    sn_device_t device;
    bool found = false;
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    for (int i = 0; i < sn_model->device_count; i++)
        if (strcmp(sn_model->devices[i].id,device_id) == 0) { device = sn_model->devices[i]; found = true; break; }
    xSemaphoreGive(sn_mutex);
    unsigned v[6];
    if (!found || sscanf(device.mac,"%2x:%2x:%2x:%2x:%2x:%2x",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6)
    { sn_message("Serve il MAC del dispositivo: esegui prima una scansione",0); return; }
    if (!sn_connect()) return;
    struct in_addr ip;
    if (inet_pton(AF_INET,device.ip,&ip) != 1 || !sn_local(ip.s_addr))
    { sn_message("Il dispositivo non e nella rete collegata",0); return; }
    for (int i = 0; i < 6; i++) s_target[i] = (uint8_t)v[i];
    atomic_store(&s_frames,0);
    atomic_store(&s_last_frame,0);
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    memset(&s_find,0,sizeof(s_find));
    s_find.running = true;
    s_find.best = -127;
    snprintf(s_find.ip,sizeof(s_find.ip),"%s",device.ip);
    snprintf(s_find.mac,sizeof(s_find.mac),"%s",device.mac);
    snprintf(s_find.name,sizeof(s_find.name),"%s",sn_display_name(&device));
    xSemaphoreGive(sn_mutex);
    wifi_set_fast(true); /* modem sleep would miss most frames */
    power_keep_screen_on(true);
    wifi_promiscuous_filter_t filter = {.filter_mask=WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_err_t error = esp_wifi_set_promiscuous_filter(&filter);
    if (error == ESP_OK) error = esp_wifi_set_promiscuous_rx_cb(sniff);
    if (error == ESP_OK) error = esp_wifi_set_promiscuous(true);
    if (error != ESP_OK) sn_message("Ricezione radio non disponibile",0);
    else sn_message("Ricerca attiva: muoviti e osserva il segnale",-1);
    int64_t start = esp_timer_get_time(), next_second = start;
    int seen = 0, history[8] = {0}, filled = 0;
    float smooth = 0;
    while (error == ESP_OK && !sn_cancelled() && esp_timer_get_time()-start < 300000000LL && wifi_is_connected())
    {
        sn_ping(ip.s_addr,200); /* the reply is a frame transmitted by the device */
        int frames = atomic_load(&s_frames);
        if (frames != seen)
        {
            int rssi = atomic_load(&s_last_rssi);
            smooth = seen == 0 ? rssi : smooth*0.7f + rssi*0.3f;
            seen = frames;
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_second && seen)
        {
            next_second = now + 1000000;
            memmove(history+1,history,sizeof(history)-sizeof(history[0]));
            history[0] = (int)smooth;
            if (filled < 8) filled++;
        }
        xSemaphoreTake(sn_mutex,portMAX_DELAY);
        s_find.frames = seen;
        s_find.seconds = (int)((now-start)/1000000);
        s_find.age_ms = seen ? (int)((now-atomic_load(&s_last_frame))/1000) : -1;
        if (seen)
        {
            s_find.rssi = atomic_load(&s_last_rssi);
            s_find.smooth = (int)(smooth - 0.5f);
            if (s_find.smooth > s_find.best) s_find.best = s_find.smooth;
            int change = filled > 3 ? history[0] - history[3] : 0;
            s_find.trend = change >= 3 ? 1 : change <= -3 ? -1 : 0;
        }
        xSemaphoreGive(sn_mutex);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    power_keep_screen_on(false);
    wifi_set_fast(false);
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    s_find.running = false;
    xSemaphoreGive(sn_mutex);
    if (error == ESP_OK)
        sn_message(seen ? "Ricerca terminata" : "Nessun segnale diretto: 5 GHz, cavo o altro access point",-1);
}

/* ---- Quality ----------------------------------------------------------------- */

static int connect_time(uint32_t ip, uint16_t port, int timeout_ms)
{
    int fd = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (fd < 0) return -1;
    fcntl(fd,F_SETFL,O_NONBLOCK);
    struct sockaddr_in target = {.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=ip};
    int64_t start = esp_timer_get_time();
    int result = -1;
    if (connect(fd,(struct sockaddr *)&target,sizeof(target)) == 0) result = 0;
    else if (errno == EINPROGRESS)
    {
        fd_set writes; FD_ZERO(&writes); FD_SET(fd,&writes);
        struct timeval wait = {.tv_sec=timeout_ms/1000,.tv_usec=(timeout_ms%1000)*1000};
        int error = 0; socklen_t length = sizeof(error);
        if (select(fd+1,NULL,&writes,NULL,&wait) > 0 && getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&length) == 0 && error == 0)
            result = (int)((esp_timer_get_time()-start+500)/1000);
    }
    close(fd);
    return result;
}

/* Time to any answer (even "name not found") from the DNS server. */
static int dns_time(uint32_t server, uint32_t name_ip)
{
    int fd = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (fd < 0) return -1;
    uint8_t packet[256];
    uint16_t id = (uint16_t)esp_random();
    size_t length = sn_dns_ptr_query(packet,sizeof(packet),id,name_ip);
    struct sockaddr_in target = {.sin_family=AF_INET,.sin_port=htons(53),.sin_addr.s_addr=server};
    int64_t start = esp_timer_get_time();
    int result = -1;
    if (sendto(fd,packet,length,0,(struct sockaddr *)&target,sizeof(target)) == (int)length)
    {
        while (result < 0 && esp_timer_get_time()-start < 1500000)
        {
            fd_set reads; FD_ZERO(&reads); FD_SET(fd,&reads);
            struct timeval wait = {.tv_usec=100000};
            if (select(fd+1,&reads,NULL,NULL,&wait) <= 0) continue;
            int got = recv(fd,packet,sizeof(packet),0);
            if (got >= 12 && packet[0] == (id >> 8) && packet[1] == (id & 255) && (packet[2] & 0x80))
                result = (int)((esp_timer_get_time()-start+500)/1000);
        }
    }
    close(fd);
    return result;
}

static void verdict(sn_quality_t *q)
{
    const char *signal = q->rssi_min == 0 ? "non misurato" : q->rssi >= -60 ? "ottimo" : q->rssi >= -67 ? "buono" :
                         q->rssi >= -75 ? "discreto" : "debole";
    int loss = q->gateway_sent ? q->gateway_lost*100/q->gateway_sent : 100;
    const char *local = q->gateway_avg < 0 ? "senza risposta" :
                        loss > 5 || q->gateway_avg > 50 || q->gateway_jitter > 30 ? "instabile" :
                        q->gateway_avg > 15 || q->gateway_jitter > 10 || loss > 0 ? "accettabile" : "stabile";
    const char *internet = q->internet_avg < 0 ? "non raggiungibile" : q->internet_avg > 150 ? "lenta" :
                           q->internet_avg > 60 ? "nella norma" : "reattiva";
    const char *hint = "";
    if (q->rssi < -70 && strcmp(local,"stabile") != 0) hint = " Avvicinati all'access point o prova un canale meno affollato.";
    else if (strcmp(local,"stabile") == 0 && (q->internet_avg > 150 || q->internet_avg < 0)) hint = " La rete locale e buona: il limite e oltre il router (linea o provider).";
    snprintf(q->verdict,sizeof(q->verdict),"Segnale %s (%d dBm). Rete locale %s: %d ms, jitter %d ms, persi %d%%. Internet %s.%s",
             signal,q->rssi,local,q->gateway_avg,q->gateway_jitter,loss,internet,hint);
}

void sn_run_quality(int seconds)
{
    if (!sn_connect()) return;
    if (seconds < 10) seconds = 10;
    if (seconds > 120) seconds = 120;
    uint32_t gateway = sn_ip_info()->gw.addr, dns = sn_dns_server(), internet;
    inet_pton(AF_INET,"1.1.1.1",&internet);
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    s_quality.running = true;
    s_quality.seconds = 0; s_quality.duration = seconds;
    s_quality.rssi = s_quality.rssi_min = s_quality.rssi_max = 0;
    s_quality.gateway_sent = s_quality.gateway_lost = s_quality.gateway_jitter = 0;
    s_quality.gateway_avg = s_quality.gateway_min = s_quality.gateway_max = -1;
    s_quality.internet_sent = s_quality.internet_lost = 0; s_quality.internet_avg = -1;
    s_quality.dns_ms = -1; s_quality.history = 0; s_quality.verdict[0] = 0;
    xSemaphoreGive(sn_mutex);
    wifi_set_fast(true);
    int sum = 0, count = 0, previous = -1, jitter = 0, jitter_count = 0, internet_sum = 0, internet_count = 0;
    int rssi_sum = 0, rssi_count = 0;
    for (int second = 0; second < seconds && !sn_cancelled() && wifi_is_connected(); second++)
    {
        int64_t tick = esp_timer_get_time();
        int rtt = sn_ping(gateway,800), rssi = wifi_rssi(), connect_ms = -2, dns_ms = -2;
        if (second % 3 == 0) connect_ms = connect_time(internet,443,1500);
        if (second % 10 == 0) dns_ms = dns_time(dns ? dns : gateway,gateway);
        xSemaphoreTake(sn_mutex,portMAX_DELAY);
        sn_quality_t *q = &s_quality;
        q->seconds = second + 1;
        q->gateway_sent++;
        if (rtt < 0) q->gateway_lost++;
        else
        {
            sum += rtt; count++;
            if (previous >= 0) { jitter += abs(rtt-previous); jitter_count++; }
            previous = rtt;
            if (q->gateway_min < 0 || rtt < q->gateway_min) q->gateway_min = rtt;
            if (rtt > q->gateway_max) q->gateway_max = rtt;
            q->gateway_avg = sum/count;
            q->gateway_jitter = jitter_count ? jitter/jitter_count : 0;
        }
        if (rssi != 0)
        {
            rssi_sum += rssi; rssi_count++;
            q->rssi = rssi_sum/rssi_count;
            if (q->rssi_min == 0 || rssi < q->rssi_min) q->rssi_min = rssi;
            if (q->rssi_max == 0 || rssi > q->rssi_max) q->rssi_max = rssi;
        }
        if (connect_ms != -2)
        {
            q->internet_sent++;
            if (connect_ms < 0) q->internet_lost++;
            else { internet_sum += connect_ms; internet_count++; q->internet_avg = internet_sum/internet_count; }
        }
        if (dns_ms != -2 && dns_ms >= 0) q->dns_ms = dns_ms;
        if (q->history == SN_HISTORY)
        {
            memmove(q->rtt_history,q->rtt_history+1,sizeof(int)*(SN_HISTORY-1));
            memmove(q->rssi_history,q->rssi_history+1,sizeof(int)*(SN_HISTORY-1));
            q->history--;
        }
        q->rtt_history[q->history] = rtt;
        q->rssi_history[q->history] = rssi;
        q->history++;
        verdict(q);
        xSemaphoreGive(sn_mutex);
        char text[64];
        snprintf(text,sizeof(text),"Misuro la qualita: %d/%d s",second+1,seconds);
        sn_message(text,(second+1)*100/seconds);
        int64_t left = 1000000 - (esp_timer_get_time()-tick);
        if (left > 0) vTaskDelay(pdMS_TO_TICKS(left/1000));
    }
    wifi_set_fast(false);
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    s_quality.running = false;
    xSemaphoreGive(sn_mutex);
    sn_message(sn_cancelled() ? "Misura interrotta" : "Misura della qualita conclusa",100);
}

/* ---- Speed ------------------------------------------------------------------- */

static void speed_state(int percent, const char *text)
{
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    if (percent >= 0) s_quality.speed_percent = percent;
    if (text) snprintf(s_quality.speed_text,sizeof(s_quality.speed_text),"%s",text);
    xSemaphoreGive(sn_mutex);
}

void sn_run_speed(unsigned megabytes, bool upload)
{
    if (!sn_connect()) return;
    unsigned limit = upload ? 10 : 25;
    if (megabytes < 1) megabytes = 1;
    if (megabytes > limit) megabytes = limit;
    uint32_t bytes = megabytes * 1000000u;
    char *buffer = heap_caps_calloc(1,8192,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) { sn_message("RAM insufficiente per il test",0); return; }
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    s_quality.speed_running = true;
    xSemaphoreGive(sn_mutex);
    speed_state(0,"Connessione a " SPEED_HOST "...");
    char url[96];
    if (upload) snprintf(url,sizeof(url),"https://" SPEED_HOST "/__up");
    else snprintf(url,sizeof(url),"https://" SPEED_HOST "/__down?bytes=%lu",(unsigned long)bytes);
    esp_http_client_config_t config = {.url=url,.crt_bundle_attach=esp_crt_bundle_attach,.timeout_ms=10000,
                                       .buffer_size=4096,.method=upload ? HTTP_METHOD_POST : HTTP_METHOD_GET};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    wifi_set_fast(true);
    power_cpu_boost(true); /* TLS decryption is the bottleneck on the watch */
    uint32_t moved = 0;
    int64_t start = 0, elapsed = 0;
    bool ok = false;
    int tls_error = 0, tls_flags = 0;
    if (client != NULL)
    {
        if (upload) esp_http_client_set_header(client,"Content-Type","application/octet-stream");
        if (esp_http_client_open(client,upload ? (int)bytes : 0) == ESP_OK)
        {
            start = esp_timer_get_time();
            if (upload)
            {
                while (moved < bytes && !sn_cancelled())
                {
                    int chunk = bytes-moved < 8192 ? (int)(bytes-moved) : 8192;
                    int written = esp_http_client_write(client,buffer,chunk);
                    if (written <= 0) break;
                    moved += written;
                    speed_state((int)((uint64_t)moved*100/bytes),NULL);
                }
                ok = moved == bytes && esp_http_client_fetch_headers(client) >= 0 &&
                     esp_http_client_get_status_code(client) == 200;
            }
            else if (esp_http_client_fetch_headers(client) >= 0 && esp_http_client_get_status_code(client) == 200)
            {
                start = esp_timer_get_time(); /* from the first byte: excludes the TLS handshake */
                while (moved < bytes && !sn_cancelled())
                {
                    int got = esp_http_client_read(client,buffer,8192);
                    if (got <= 0) break;
                    moved += got;
                    speed_state((int)((uint64_t)moved*100/bytes),NULL);
                }
                ok = moved == bytes;
            }
            elapsed = esp_timer_get_time() - start;
        }
        else esp_http_client_get_and_clear_last_tls_error(client,&tls_error,&tls_flags);
        esp_http_client_cleanup(client);
    }
    power_cpu_boost(false);
    wifi_set_fast(false);
    free(buffer);
    char text[128];
    int kbps = ok && elapsed > 0 ? (int)((uint64_t)moved*8000/elapsed) : -1;
    if (ok)
        snprintf(text,sizeof(text),"%s %d.%d Mbit/s: %u MB in %d.%d s con " SPEED_HOST ". Limite dell'orologio, non della linea.",
                 upload ? "Upload" : "Download",kbps/1000,kbps%1000/100,megabytes,(int)(elapsed/1000000),(int)(elapsed/100000%10));
    else if (tls_flags & (MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_EXPIRED))
        snprintf(text,sizeof(text),"%s non riuscito: certificato fuori data, controlla l'ora dell'orologio",upload ? "Upload" : "Download");
    else if (tls_flags)
        snprintf(text,sizeof(text),"%s non riuscito: certificato non verificato (0x%x)",upload ? "Upload" : "Download",tls_flags);
    else snprintf(text,sizeof(text),"%s non riuscito%s",upload ? "Upload" : "Download",sn_cancelled() ? " (interrotto)" : ": rete o server non raggiungibile");
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    if (upload) s_quality.upload_kbps = kbps; else s_quality.download_kbps = kbps;
    s_quality.speed_running = false;
    s_quality.speed_percent = ok ? 100 : 0;
    snprintf(s_quality.speed_text,sizeof(s_quality.speed_text),"%s",text);
    xSemaphoreGive(sn_mutex);
    sn_message(ok ? "Test di velocita concluso" : "Test di velocita non riuscito",ok ? 100 : 0);
}

/* ---- Wi-Fi ------------------------------------------------------------------- */

static const char *security(wifi_auth_mode_t mode)
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
        case WIFI_AUTH_OWE: return "OWE";
        default: return "Altra";
    }
}

static int compare_rssi(const void *a, const void *b)
{
    return ((const sn_ap_t *)b)->rssi - ((const sn_ap_t *)a)->rssi;
}

void sn_run_wifi_scan(void)
{
    sn_message("Cerco reti Wi-Fi a 2,4 GHz...",10);
    wifi_known_t *known = calloc(WIFI_KNOWN_MAX,sizeof(wifi_known_t));
    int known_count = known ? wifi_known_list(known,WIFI_KNOWN_MAX) : 0;
    sn_ap_t *found = calloc(SN_APS,sizeof(sn_ap_t));
    int count = 0;
    if (found != NULL && wifi_is_connected())
    {
        /* Connected: the radio is on, a direct scan also reports channel and BSSID. */
        wifi_scan_config_t scan = {.show_hidden=true};
        uint16_t records = 32;
        wifi_ap_record_t *list = calloc(records,sizeof(wifi_ap_record_t));
        wifi_ap_record_t current;
        bool connected = esp_wifi_sta_get_ap_info(&current) == ESP_OK;
        if (list != NULL && esp_wifi_scan_start(&scan,true) == ESP_OK && esp_wifi_scan_get_ap_records(&records,list) == ESP_OK)
        {
            for (int i = 0; i < records && count < SN_APS; i++)
            {
                sn_ap_t *ap = &found[count++];
                snprintf(ap->ssid,sizeof(ap->ssid),"%s",(const char *)list[i].ssid);
                memcpy(ap->bssid,list[i].bssid,6);
                ap->rssi = list[i].rssi;
                ap->channel = list[i].primary;
                ap->open = list[i].authmode == WIFI_AUTH_OPEN;
                snprintf(ap->security,sizeof(ap->security),"%s",security(list[i].authmode));
                ap->current = connected && memcmp(current.bssid,list[i].bssid,6) == 0;
            }
        }
        else if (list == NULL) esp_wifi_clear_ap_list();
        free(list);
    }
    else if (found != NULL)
    {
        /* Not connected: the shared service powers the radio for the scan (no channel). */
        wifi_ap_t *list = calloc(WIFI_SCAN_MAX,sizeof(wifi_ap_t));
        int n = list ? wifi_scan(list,WIFI_SCAN_MAX) : 0;
        for (int i = 0; i < n && count < SN_APS; i++)
        {
            sn_ap_t *ap = &found[count++];
            snprintf(ap->ssid,sizeof(ap->ssid),"%s",list[i].ssid);
            ap->rssi = list[i].rssi;
            ap->open = list[i].open;
            snprintf(ap->security,sizeof(ap->security),"%s",list[i].open ? "Aperta" : "Protetta");
        }
        free(list);
    }
    for (int i = 0; i < count; i++)
        for (int k = 0; k < known_count; k++)
            if (found[i].ssid[0] && strcmp(found[i].ssid,known[k].ssid) == 0) found[i].known = true;
    if (known) { memset(known,0,sizeof(wifi_known_t)*WIFI_KNOWN_MAX); free(known); }
    qsort(found,count,sizeof(sn_ap_t),compare_rssi);
    xSemaphoreTake(sn_mutex,portMAX_DELAY);
    if (found) memcpy(s_aps,found,sizeof(sn_ap_t)*count);
    s_ap_count = count;
    xSemaphoreGive(sn_mutex);
    free(found);
    char text[64];
    snprintf(text,sizeof(text),"%d access point trovati (solo 2,4 GHz)",count);
    sn_message(text,100);
}

bool sn_run_wifi_join(const char *ssid, const char *password, bool saved)
{
    char secret[65] = "";
    if (saved)
    {
        wifi_known_t *known = calloc(WIFI_KNOWN_MAX,sizeof(wifi_known_t));
        int count = known ? wifi_known_list(known,WIFI_KNOWN_MAX) : 0;
        for (int i = 0; i < count; i++)
            if (strcmp(known[i].ssid,ssid) == 0) snprintf(secret,sizeof(secret),"%s",known[i].password);
        if (known) { memset(known,0,sizeof(wifi_known_t)*WIFI_KNOWN_MAX); free(known); }
    }
    else if (password != NULL) snprintf(secret,sizeof(secret),"%s",password);
    char text[96];
    snprintf(text,sizeof(text),"Collegamento a %s...",ssid);
    sn_message(text,20);
    esp_err_t result = wifi_connect_new(ssid,secret,20000);
    memset(secret,0,sizeof(secret));
    if (result == ESP_OK)
    {
        sn_wifi_adopt();
        snprintf(text,sizeof(text),"Collegato a %s",ssid);
        sn_message(text,100);
        return true;
    }
    snprintf(text,sizeof(text),"Non collegato a %s: %s",ssid,wifi_reason_text(wifi_last_reason()));
    sn_message(text,0);
    sn_connect(); /* back to the best saved network */
    return false;
}
