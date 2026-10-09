#ifndef SERVICES_FW_VERSION_H
#define SERVICES_FW_VERSION_H

/*
 * Firmware version numbers ("semantic versioning"):
 *
 *   1.3.0          a stable release
 *   1.3.0-dev.2    a development build on the way to 1.3.0 (dev channel)
 *
 *   1.2.9 < 1.3.0-dev.2 < 1.3.0-dev.10 < 1.3.0 < 1.3.1
 *
 * Pure C (no ESP-IDF): can be tested on the PC.
 */

#include <stdbool.h>

/* <0 if a is older than b, 0 if equal, >0 if a is newer. Not a version: plain text order. */
int fw_version_compare(const char *a, const char *b);

/* True for "1.3.0-dev.2" (a pre-release: dev channel). */
bool fw_version_is_prerelease(const char *version);

#endif
