#ifndef CAPTIVE_DNS_H
#define CAPTIVE_DNS_H

/*
 * Tiny DNS server for the watch's own access point (web_server.c only):
 * every name resolves to `ip` (network byte order), so phones open the
 * setup page by themselves and stay connected.
 */

#include <stdint.h>

#include "esp_err.h"

esp_err_t captive_dns_start(uint32_t ip);
void captive_dns_stop(void);

#endif
