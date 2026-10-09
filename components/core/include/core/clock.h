#ifndef CORE_CLOCK_H
#define CORE_CLOCK_H

/*
 * Wall clock helpers and the minute tick.
 *
 * A one-shot esp_timer fires exactly at every new minute and sets
 * STATE_MINUTE: watch faces bind to it and redraw once a minute (the
 * always-on face does nothing else). The timer survives light sleep.
 *
 * The time itself comes from the RTC at boot, then from NTP (Wi-Fi) or the
 * phone (BLE): see services/time_sync.
 */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Load the time zone and start the minute tick. */
void clock_init(void);

/* Set the system time (UTC) and the RTC, re-align the minute tick. */
void clock_set_utc(time_t utc);

/* Apply a new POSIX time zone string ("CET-1CEST,M3.5.0,M10.5.0/3"). */
void clock_set_timezone(const char *tz);

/* Local time now. */
void clock_local(struct tm *out);

/* True once the time came from a trusted source (RTC set, NTP or phone). */
bool clock_is_valid(void);

/* "14:05" or "2:05 PM" according to the 24h setting. */
void clock_format_hm(const struct tm *t, char *out, size_t size);

/* Italian day / month names. */
const char *clock_weekday_name(int wday, bool short_name);
const char *clock_month_name(int mon, bool short_name);

#endif
