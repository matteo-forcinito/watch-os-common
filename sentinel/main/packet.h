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
bool sn_proxy_arp(uint32_t sender_ip, uint32_t gateway_ip, const uint8_t *sender_mac, const uint8_t *gateway_mac);
/* Identification helpers: pure and bounded; ip arguments are in network byte order. */
size_t sn_dns_ptr_query(uint8_t *out, size_t capacity, uint16_t id, uint32_t ip);
bool sn_dns_ptr_answer(const uint8_t *packet, size_t length, uint16_t *id, char *name, size_t capacity);
size_t sn_nbstat_query(uint8_t *out, size_t capacity, uint16_t id);
bool sn_nbstat_name(const uint8_t *packet, size_t length, uint16_t id, char *name, size_t capacity);
bool sn_http_header(const char *response, const char *header, char *out, size_t capacity);
bool sn_location(const char *response, uint32_t source_ip, uint16_t *port, char *path, size_t capacity);
bool sn_xml_value(const char *xml, const char *tag, char *out, size_t capacity);
#endif
