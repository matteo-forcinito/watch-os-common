#ifndef HOUSE_TEST_SERVICES_ECOS_H
#define HOUSE_TEST_SERVICES_ECOS_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define ECOS_PEERS_MAX 6
#define ECOS_NAME_SIZE 24
#define ECOS_ORBIT_MAX 144
typedef struct {
    char id[13], name[ECOS_NAME_SIZE];
    char product[32], version[16];
    bool known, reachable, usb, charging, powered, companion;
    uint32_t seen_ago;
    int battery;
} ecos_member_t;
typedef struct {
    bool radio, busy;
    int count;
    ecos_member_t members[ECOS_PEERS_MAX];
} ecos_family_t;
esp_err_t ecos_family_refresh(void);
void ecos_family(ecos_family_t *out);
void ecos_family_hold(bool on);
bool ecos_family_companion(char id[13]);
#endif
