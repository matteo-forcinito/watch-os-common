#pragma once
/* ECOS bus: shared ESP-NOW for the OS services (see ecos_bus.c). */
#include "esp_err.h"
#include "esp_now.h"

#include <stddef.h>
#include <stdint.h>

/* Called from the Wi-Fi task: copy the frame into a queue and return. */
typedef void (*ecos_bus_recv_t)(const esp_now_recv_info_t *info, const uint8_t *data, int length);
typedef void (*ecos_bus_sent_t)(const esp_now_send_info_t *info, esp_now_send_status_t status);

/* Opens the service whose frames start with `tag` (4 bytes, e.g. "EL02"). The first open
 * turns the radio and ESP-NOW on. tx_power: highest TX power wanted (0.25 dBm units), 0 = any. */
esp_err_t ecos_bus_open(const char tag[4], ecos_bus_recv_t recv, ecos_bus_sent_t sent, int8_t tx_power);
esp_err_t ecos_bus_open_mask(const char tag[4], const uint8_t pattern[4], const uint8_t mask[4],
                             ecos_bus_recv_t recv, ecos_bus_sent_t sent, int8_t tx_power);
/* Call open/close/power from service tasks, never from callbacks. Close drains
 * dispatched callbacks before returning; the last close turns the radio off. */
void ecos_bus_close(const char tag[4]);
/* Changes the TX power limit of an open service (0 = no limit). */
esp_err_t ecos_bus_power(const char tag[4], int8_t tx_power);
/* Adds a (clear-text: frames are sealed by the service) peer if missing. */
esp_err_t ecos_bus_peer(const uint8_t mac[6]);
/* Call send from a service task, not an ESP-NOW callback. */
esp_err_t ecos_bus_send(const uint8_t mac[6], const uint8_t *data, size_t length);
/* Explicit-owner variant for legacy frames without a leading four-byte tag.
 * Call from a service task, not an ESP-NOW callback. */
esp_err_t ecos_bus_send_tag(const char tag[4], const uint8_t mac[6], const uint8_t *data, size_t length);
