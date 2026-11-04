/*
 * Astri - the night sky above you, on the wrist. Offline: stars, constellations,
 * the Milky Way, the Moon with its phase, the Sun and the five naked-eye planets.
 *
 *   astro.c  time and coordinates: sidereal time, the equatorial -> horizontal
 *            rotation, Sun and Moon (Astronomical Almanac low precision),
 *            planets (JPL approximate Keplerian elements, 1800-2050)
 *   sky.c    the catalogue (assets from d3-celestial, tools/build_data.py) and
 *            the renderer: stereographic projection, zenith at the centre and the
 *            horizon on the round edge, north up and east on the left (looking up)
 *   ui.c     the screen: turning a finger around the centre moves time
 *            (one turn = 24 h), a tap names what is under it, the centre is "now"
 *
 * Positions are J2000 (no precession: about 0.4 degrees off in 2026, below a
 * pixel near the zenith of a 466 px sky). Accuracy: Sun and planets better than
 * 0.1 degrees, the Moon about 0.3 degrees (geocentric: parallax up to 1 degree).
 */
#ifndef ASTRI_H
#define ASTRI_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Equatorial -> horizontal rotation for one place and moment. */
typedef struct
{
    float m[3][3];   /* rows: east, north, up; columns: x, y, z (equatorial J2000) */
} astri_frame_t;

typedef enum
{
    ASTRI_SUN, ASTRI_MOON, ASTRI_MERCURY, ASTRI_VENUS, ASTRI_MARS, ASTRI_JUPITER, ASTRI_SATURN, ASTRI_BODIES
} astri_body_t;

typedef struct
{
    float ra, dec;      /* degrees, J2000 */
    float mag;          /* rough visual magnitude, for size and labels */
} astri_position_t;

/* ---- astro.c */
double astri_julian_day(time_t t);
void astri_frame(astri_frame_t *frame, time_t t, float lat_deg, float lon_deg);
/* Unit vector of (ra, dec) in degrees, then rotated into east/north/up. */
void astri_to_horizontal(const astri_frame_t *frame, float ra, float dec, float enu[3]);
void astri_body(astri_body_t body, time_t t, astri_position_t *out);
const char *astri_body_name(astri_body_t body);
/* Moon: illuminated fraction 0..1 and whether it is waxing. */
void astri_moon_phase(time_t t, float *illuminated, bool *waxing);

/* ---- sky.c */
typedef struct
{
    int width, height;          /* canvas */
    int cx, cy, radius;         /* the sky disc */
    uint16_t *pixels;           /* RGB565, width * height */
} astri_view_t;

bool astri_sky_load(void);
/* Draw the whole sky for time t. fast: skip the Milky Way (while the finger moves). */
void astri_sky_render(const astri_view_t *view, void *lv_canvas, time_t t, float lat, float lon, bool fast);
/* Name of the object nearest to (x, y) within max_px of the last render, or NULL. */
const char *astri_sky_pick(int x, int y, int max_px, char *buffer, int size);
/* Sun altitude in degrees at the last render (day, twilight, night). */
float astri_sky_sun_altitude(void);

/* ---- ui.c */
void astri_ui_create(void *screen);
bool astri_ui_back(void);

#endif
