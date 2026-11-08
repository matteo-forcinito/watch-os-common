#ifndef TEST_SERVICES_ECOS_H
#define TEST_SERVICES_ECOS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ECOS_ORBIT_MAX 144
#define ECOS_PEERS_MAX 6
#define ECOS_NAME_SIZE 24
typedef struct { char id[13]; bool reachable; } ecos_member_t;
typedef struct { bool radio; int count; ecos_member_t members[ECOS_PEERS_MAX]; } ecos_family_t;
void ecos_family(ecos_family_t *out);
void ecos_family_hold(bool on);
bool ecos_family_stream_active(void);
int ecos_family_orbit(const char *id, const uint8_t *request, size_t length,
                      uint8_t *reply, size_t max, int timeout_ms);
#endif
