#ifndef PIENO_FUEL_H
#define PIENO_FUEL_H

/*
 * Pieno's pure maths and parsing (no ESP-IDF, no libc: tested in an emulator).
 *
 * Data: the Italian ministry (MIMIT) publishes every morning two CSV files
 * with "|" separators: every station (id, brand, name, address, coordinates)
 * and every price (station id, product, price, self/served, when it was
 * communicated). Station names may themselves contain "|", so a station line
 * is read from both ends.
 */

#include <stdbool.h>
#include <stdint.h>

#define PN_FUELS 4          /* the base products every station compares on */
enum { PN_BENZINA, PN_GASOLIO, PN_GPL, PN_METANO };
enum { PN_SERVED, PN_SELF };

typedef struct
{
    uint32_t id;
    const char *brand;      /* pointers into the (modified) line */
    const char *name;
    const char *address;
    const char *town;
    const char *province;
    double lat;
    double lon;
} pn_station_line_t;

typedef struct
{
    uint32_t id;
    const char *product;
    float price;
    bool self;
    int64_t when;           /* local time as seconds since 1970 (no time zone applied) */
} pn_price_line_t;

/* Split a station / price line in place. false: header, malformed or out of range. */
bool pn_parse_station(char *line, pn_station_line_t *out);
bool pn_parse_price(char *line, pn_price_line_t *out);

/* Base product index for a product name, -1 for the premium ones ("Blue Super"...). */
int pn_fuel_of(const char *product);

/* "12/10/2026 06:31:01" -> seconds (local), -1 if malformed. */
int64_t pn_parse_when(const char *text);
/* Days since 1970 for a civil date. */
int64_t pn_days_from_civil(int year, int month, int day);

/* Distance in km on the earth's surface (equirectangular: exact enough below 100 km). */
double pn_distance_km(double lat1, double lon1, double lat2, double lon2);
double pn_cos(double radians);

/*
 * What a fill really costs per litre when the station is `km` away from home:
 * the detour (there and back, road ~1.3 x straight line) burns fuel bought at
 * the same price. consumption: km per litre. litres: the fill.
 */
float pn_real_price(float price, float km, float litres, float km_per_litre);

/* min / median / max of n values (values are reordered). false if n == 0. */
bool pn_stats(float *values, int n, float *min, float *median, float *max);

/* Number parser for "1.899" / "-12.5" / "42.29466" (no exponent). */
double pn_atof(const char *text, bool *ok);

#endif
