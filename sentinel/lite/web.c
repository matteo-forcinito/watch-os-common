/*
 * Sentinel Lite dashboard API (appkit, http://<watch>/sentinel/).
 *
 *   GET  /api/state      network, status, devices, last check, nearby networks
 *   POST /api/scan       a new inventory
 *   POST /api/check      router / internet / DNS check
 *   POST /api/air        nearby networks
 *   POST /api/ports      {ip}: open services of a device
 *   POST /api/edit       {mac|ip, label, trusted}
 *   GET  /api/export.csv the inventory
 */
#include "lite.h"

#include "appkit.h"

#include "esp_heap_caps.h"
#include "lwip/inet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char sentinel_page_start[] asm("_binary_sentinel_dashboard_html_start");
extern const char sentinel_page_end[] asm("_binary_sentinel_dashboard_html_end");

#define GUARD()                                  \
    do                                           \
    {                                            \
        if (!appkit_authorized(request))         \
        {                                        \
            return appkit_unauthorized(request); \
        }                                        \
    } while (0)

static cJSON *device_json(const sl_device_t *d, const char *gateway)
{
    char name[64], ip[16], mac[20] = "";
    sl_display_name(d, name, sizeof(name));
    sl_ip_text(d->ip, ip, sizeof(ip));
    if (d->has_mac)
    {
        sl_mac_text(d->mac, mac, sizeof(mac));
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "label", d->label);
    cJSON_AddStringToObject(o, "ip", ip);
    cJSON_AddStringToObject(o, "mac", mac);
    cJSON_AddStringToObject(o, "vendor", d->vendor);
    cJSON_AddStringToObject(o, "kind", sl_kind(d, strcmp(ip, gateway) == 0));
    cJSON_AddStringToObject(o, "mdns", d->mdns);
    cJSON_AddStringToObject(o, "netbios", d->netbios);
    cJSON_AddStringToObject(o, "upnp", d->upnp);
    cJSON_AddStringToObject(o, "dns", d->dns);
    cJSON_AddBoolToObject(o, "online", d->online);
    cJSON_AddBoolToObject(o, "fresh", d->fresh);
    cJSON_AddBoolToObject(o, "trusted", d->trusted);
    cJSON_AddNumberToObject(o, "rtt", d->rtt);
    cJSON_AddNumberToObject(o, "first", (double)d->first_seen);
    cJSON_AddNumberToObject(o, "last", (double)d->last_seen);
    if (d->ports_checked)
    {
        int count;
        const uint16_t *ports = sl_port_list(&count);
        cJSON *open = cJSON_AddArrayToObject(o, "ports");
        for (int i = 0; i < count; i++)
        {
            if (d->open_ports & (1u << i))
            {
                cJSON *p = cJSON_CreateObject();
                cJSON_AddNumberToObject(p, "port", ports[i]);
                cJSON_AddStringToObject(p, "name", sl_port_name(i));
                const char *warning = sl_port_warning(i);
                if (warning)
                {
                    cJSON_AddStringToObject(p, "warning", warning);
                }
                cJSON_AddItemToArray(open, p);
            }
        }
    }
    return o;
}

static esp_err_t state(httpd_req_t *request)
{
    GUARD();
    sl_status_t st;
    sl_status(&st);
    sl_check_t c;
    sl_check_result(&c);
    cJSON *out = cJSON_CreateObject();
    cJSON *s = cJSON_AddObjectToObject(out, "status");
    cJSON_AddNumberToObject(s, "phase", st.phase);
    cJSON_AddNumberToObject(s, "percent", st.percent);
    cJSON_AddStringToObject(s, "message", st.message);
    cJSON_AddStringToObject(s, "ssid", st.ssid);
    cJSON_AddStringToObject(s, "ip", st.ip);
    cJSON_AddStringToObject(s, "gateway", st.gateway);
    cJSON_AddNumberToObject(s, "online", st.online);
    cJSON_AddNumberToObject(s, "fresh", st.fresh);
    cJSON_AddNumberToObject(s, "devices", st.devices);
    cJSON_AddNumberToObject(s, "scanned", (double)st.scanned);
    cJSON_AddNumberToObject(s, "revision", st.revision);
    if (c.done)
    {
        cJSON *k = cJSON_AddObjectToObject(out, "check");
        cJSON_AddNumberToObject(k, "rssi", c.rssi);
        cJSON_AddNumberToObject(k, "channel", c.channel);
        cJSON_AddStringToObject(k, "security", c.security);
        cJSON_AddNumberToObject(k, "gateway_avg", c.gateway_avg);
        cJSON_AddNumberToObject(k, "gateway_max", c.gateway_max);
        cJSON_AddNumberToObject(k, "gateway_loss", c.gateway_loss);
        cJSON_AddNumberToObject(k, "internet_ms", c.internet_ms);
        cJSON_AddNumberToObject(k, "dns_ms", c.dns_ms);
        cJSON_AddStringToObject(k, "verdict", c.verdict);
    }
    sl_lock();
    int count;
    const sl_device_t *list = sl_devices(&count);
    cJSON *devices = cJSON_AddArrayToObject(out, "devices");
    for (int i = 0; i < count; i++)
    {
        cJSON_AddItemToArray(devices, device_json(&list[i], st.gateway));
    }
    const sl_ap_t *aps = sl_aps(&count);
    cJSON *air = cJSON_AddArrayToObject(out, "aps");
    for (int i = 0; i < count; i++)
    {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(a, "channel", aps[i].channel);
        cJSON_AddNumberToObject(a, "rssi", aps[i].rssi);
        cJSON_AddStringToObject(a, "security", aps[i].security);
        cJSON_AddBoolToObject(a, "current", aps[i].current);
        cJSON_AddItemToArray(air, a);
    }
    sl_unlock();
    return appkit_json(request, out);
}

static esp_err_t simple(httpd_req_t *request, void (*fn)(void))
{
    GUARD();
    fn();
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t scan(httpd_req_t *request) { return simple(request, sl_scan); }
static esp_err_t check(httpd_req_t *request) { return simple(request, sl_check); }
static esp_err_t air(httpd_req_t *request) { return simple(request, sl_airwaves); }

static cJSON *body(httpd_req_t *request)
{
    char *text = appkit_body(request, 1024);
    cJSON *in = text != NULL ? cJSON_Parse(text) : NULL;
    free(text);
    return in;
}

static esp_err_t ports(httpd_req_t *request)
{
    GUARD();
    cJSON *in = body(request);
    struct in_addr ip = {0};
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(in, "ip"));
    bool ok = text != NULL && inet_pton(AF_INET, text, &ip) == 1;
    cJSON_Delete(in);
    if (!ok)
    {
        return appkit_error(request, "400 Bad Request", "IP non valido");
    }
    sl_ports(ip.s_addr);
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t edit(httpd_req_t *request)
{
    GUARD();
    cJSON *in = body(request);
    const char *mac_text = cJSON_GetStringValue(cJSON_GetObjectItem(in, "mac"));
    const char *ip_text = cJSON_GetStringValue(cJSON_GetObjectItem(in, "ip"));
    const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(in, "label"));
    bool trusted = cJSON_IsTrue(cJSON_GetObjectItem(in, "trusted"));
    uint8_t mac[6];
    bool has_mac = mac_text != NULL &&
                   sscanf(mac_text, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6;
    struct in_addr ip = {0};
    if (ip_text != NULL)
    {
        inet_pton(AF_INET, ip_text, &ip);
    }
    char clean[32] = "";
    if (label != NULL)
    {
        snprintf(clean, sizeof(clean), "%s", label);
    }
    bool ok = sl_edit(has_mac ? mac : NULL, ip.s_addr, clean, trusted);
    cJSON_Delete(in);
    return ok ? appkit_json(request, cJSON_CreateObject()) : appkit_error(request, "404 Not Found", "Dispositivo non trovato");
}

static esp_err_t export_csv(httpd_req_t *request)
{
    GUARD();
    sl_status_t st;
    sl_status(&st);
    httpd_resp_set_type(request, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(request, "Content-Disposition", "attachment; filename=\"dispositivi.csv\"");
    httpd_resp_send_chunk(request, "nome;tipo;ip;mac;produttore;fidato;in linea;visto per la prima volta\n", HTTPD_RESP_USE_STRLEN);
    sl_lock();
    int count;
    const sl_device_t *list = sl_devices(&count);
    char line[256];
    for (int i = 0; i < count; i++)
    {
        const sl_device_t *d = &list[i];
        char name[64], ip[16], mac[20] = "";
        sl_display_name(d, name, sizeof(name));
        sl_ip_text(d->ip, ip, sizeof(ip));
        if (d->has_mac)
        {
            sl_mac_text(d->mac, mac, sizeof(mac));
        }
        for (char *p = name; *p; p++)
        {
            *p = *p == ';' ? ',' : *p;
        }
        snprintf(line, sizeof(line), "%s;%s;%s;%s;%s;%s;%s;%lld\n", name, sl_kind(d, strcmp(ip, st.gateway) == 0), ip, mac,
                 d->vendor, d->trusted ? "si" : "no", d->online ? "si" : "no", (long long)d->first_seen);
        httpd_resp_send_chunk(request, line, HTTPD_RESP_USE_STRLEN);
    }
    sl_unlock();
    return httpd_resp_send_chunk(request, NULL, 0);
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/state", .method = HTTP_GET, .handler = state},
    {.uri = "/api/scan", .method = HTTP_POST, .handler = scan},
    {.uri = "/api/check", .method = HTTP_POST, .handler = check},
    {.uri = "/api/air", .method = HTTP_POST, .handler = air},
    {.uri = "/api/ports", .method = HTTP_POST, .handler = ports},
    {.uri = "/api/edit", .method = HTTP_POST, .handler = edit},
    {.uri = "/api/export.csv", .method = HTTP_GET, .handler = export_csv},
};

static const appkit_web_config_t s_config = {
    .name = "Sentinel",
    .page_start = sentinel_page_start,
    .page_end = sentinel_page_end,
    .routes = s_routes,
    .route_count = sizeof(s_routes) / sizeof(s_routes[0]),
};

esp_err_t sentinel_web_start(void)
{
    return appkit_web_start(&s_config);
}
