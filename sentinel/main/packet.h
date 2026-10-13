#ifndef SENTINEL_PACKET_H
#define SENTINEL_PACKET_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Parse only an Ethernet/IPv4 ARP reply addressed to this station. */
bool sn_arp_reply(const void *frame, size_t length, uint32_t own_ip,
                  uint32_t *sender_ip, uint8_t sender_mac[6]);
bool sn_text_valid(const char *text, size_t maximum);
bool sn_json_safe(const char *json, size_t length);
#endif
