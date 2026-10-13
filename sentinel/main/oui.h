/* MAC address -> manufacturer, from the IEEE MA-L registry (oui_table.c is generated). */
#ifndef SENTINEL_OUI_H
#define SENTINEL_OUI_H
#include <stdint.h>

/* Vendor of the first three MAC bytes, or NULL when not registered.
 * Locally administered (random/private) MACs never match a real vendor. */
const char *sn_oui_vendor(const uint8_t mac[3]);
#endif
