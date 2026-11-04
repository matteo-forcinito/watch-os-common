/* Astri: time, coordinates and the bodies of the solar system. See astri.h. */
#include "astri.h"

#include <math.h>

#define DEG (float)(M_PI / 180.0)
#define OBLIQUITY 23.43928f   /* J2000 */

double astri_julian_day(time_t t)
{
    return (double)t / 86400.0 + 2440587.5;
}

static float wrap360(double a)
{
    a = fmod(a, 360.0);
    return (float)(a < 0 ? a + 360.0 : a);
}

void astri_frame(astri_frame_t *frame, time_t t, float lat_deg, float lon_deg)
{
    double d = astri_julian_day(t) - 2451545.0;
    float lst = wrap360(280.46061837 + 360.98564736629 * d + lon_deg) * DEG;   /* local sidereal time */
    float sl = sinf(lst), cl = cosf(lst);
    float sp = sinf(lat_deg * DEG), cp = cosf(lat_deg * DEG);

    /* east  = -x sinL + y cosL
     * north = -sinP (x cosL + y sinL) + z cosP
     * up    =  cosP (x cosL + y sinL) + z sinP */
    frame->m[0][0] = -sl;      frame->m[0][1] = cl;       frame->m[0][2] = 0.0f;
    frame->m[1][0] = -sp * cl; frame->m[1][1] = -sp * sl; frame->m[1][2] = cp;
    frame->m[2][0] = cp * cl;  frame->m[2][1] = cp * sl;  frame->m[2][2] = sp;
}

void astri_to_horizontal(const astri_frame_t *frame, float ra, float dec, float enu[3])
{
    float cd = cosf(dec * DEG);
    float v[3] = {cd * cosf(ra * DEG), cd * sinf(ra * DEG), sinf(dec * DEG)};

    for (int i = 0; i < 3; i++)
    {
        enu[i] = frame->m[i][0] * v[0] + frame->m[i][1] * v[1] + frame->m[i][2] * v[2];
    }
}

static void ecliptic_to_equatorial(float x, float y, float z, astri_position_t *out)
{
    float se = sinf(OBLIQUITY * DEG), ce = cosf(OBLIQUITY * DEG);
    float xe = x, ye = y * ce - z * se, ze = y * se + z * ce;
    out->ra = wrap360(atan2f(ye, xe) / DEG);
    out->dec = atan2f(ze, sqrtf(xe * xe + ye * ye)) / DEG;
}

/* ---------------------------------------------------------------- planets */

/* JPL "Approximate positions of the planets", table 1 (1800-2050):
 * a (au), e, I, L, long. perihelion, long. ascending node (deg), and their rates per century. */
typedef struct
{
    float a, e, i, l, w, n;
    float da, de, di, dl, dw, dn;
} elements_t;

static const elements_t ELEMENTS[] = {
    /* Mercury */ {0.38709927f, 0.20563593f, 7.00497902f, 252.25032350f, 77.45779628f, 48.33076593f,
                   0.00000037f, 0.00001906f, -0.00594749f, 149472.67411175f, 0.16047689f, -0.12534081f},
    /* Venus   */ {0.72333566f, 0.00677672f, 3.39467605f, 181.97909950f, 131.60246718f, 76.67984255f,
                   0.00000390f, -0.00004107f, -0.00078890f, 58517.81538729f, 0.00268329f, -0.27769418f},
    /* Earth-Moon barycentre */
                  {1.00000261f, 0.01671123f, -0.00001531f, 100.46457166f, 102.93768193f, 0.0f,
                   0.00000562f, -0.00004392f, -0.01294668f, 35999.37244981f, 0.32327364f, 0.0f},
    /* Mars    */ {1.52371034f, 0.09339410f, 1.84969142f, -4.55343205f, -23.94362959f, 49.55953891f,
                   0.00001847f, 0.00007882f, -0.00813131f, 19140.30268499f, 0.44441088f, -0.29257343f},
    /* Jupiter */ {5.20288700f, 0.04838624f, 1.30439695f, 34.39644051f, 14.72847983f, 100.47390909f,
                   -0.00011607f, -0.00013253f, -0.00183714f, 3034.74612775f, 0.21252668f, 0.20469106f},
    /* Saturn  */ {9.53667594f, 0.05386179f, 2.48599187f, 49.95424423f, 92.59887831f, 113.66242448f,
                   -0.00125060f, -0.00050991f, 0.00193609f, 1222.49362201f, -0.41897216f, -0.28867794f},
};
enum { EL_MERCURY, EL_VENUS, EL_EARTH, EL_MARS, EL_JUPITER, EL_SATURN };

/* Heliocentric ecliptic position (au) at T Julian centuries from J2000. */
static void heliocentric(const elements_t *el, double T, float out[3])
{
    double a = el->a + el->da * T, e = el->e + el->de * T;
    double inc = (el->i + el->di * T) * DEG, l = el->l + el->dl * T;
    double peri = el->w + el->dw * T, node = (el->n + el->dn * T);
    double M = fmod(l - peri, 360.0);
    if (M > 180.0) M -= 360.0;
    if (M < -180.0) M += 360.0;
    M *= DEG;
    double E = M + e * sin(M);
    for (int k = 0; k < 6; k++)
    {
        E -= (E - e * sin(E) - M) / (1.0 - e * cos(E));   /* Kepler, Newton steps */
    }
    double xp = a * (cos(E) - e), yp = a * sqrt(1.0 - e * e) * sin(E);
    double w = (peri - node) * DEG, o = node * DEG;
    double cw = cos(w), sw = sin(w), co = cos(o), so = sin(o), ci = cos(inc), si = sin(inc);
    out[0] = (float)((cw * co - sw * so * ci) * xp + (-sw * co - cw * so * ci) * yp);
    out[1] = (float)((cw * so + sw * co * ci) * xp + (-sw * so + cw * co * ci) * yp);
    out[2] = (float)((sw * si) * xp + (cw * si) * yp);
}

static void sun(double d, astri_position_t *out)
{
    float g = wrap360(357.528 + 0.9856003 * d) * DEG;
    float lambda = (wrap360(280.460 + 0.9856474 * d) + 1.915f * sinf(g) + 0.020f * sinf(2 * g)) * DEG;
    ecliptic_to_equatorial(cosf(lambda), sinf(lambda), 0.0f, out);
    out->mag = -26.7f;
}

static void moon(double d, astri_position_t *out)
{
    double T = d / 36525.0;
#define S(a, b) sin(fmod((a) + (b) * T, 360.0) * DEG)
    double lambda = 218.32 + 481267.881 * T + 6.29 * S(135.0, 477198.87) - 1.27 * S(259.3, -413335.36) +
                    0.66 * S(235.7, 890534.22) + 0.21 * S(269.9, 954397.74) - 0.19 * S(357.5, 35999.05) -
                    0.11 * S(186.5, 966404.03);
    double beta = 5.13 * S(93.3, 483202.02) + 0.28 * S(228.2, 960400.89) - 0.28 * S(318.3, 6003.15) -
                  0.17 * S(217.6, -407332.21);
#undef S
    float l = wrap360(lambda) * DEG, b = (float)beta * DEG;
    ecliptic_to_equatorial(cosf(b) * cosf(l), cosf(b) * sinf(l), sinf(b), out);
    out->mag = -12.0f;
}

void astri_body(astri_body_t body, time_t t, astri_position_t *out)
{
    double d = astri_julian_day(t) - 2451545.0;

    if (body == ASTRI_SUN)
    {
        sun(d, out);
        return;
    }
    if (body == ASTRI_MOON)
    {
        moon(d, out);
        return;
    }

    static const int index[] = {[ASTRI_MERCURY] = EL_MERCURY, [ASTRI_VENUS] = EL_VENUS, [ASTRI_MARS] = EL_MARS,
                                [ASTRI_JUPITER] = EL_JUPITER, [ASTRI_SATURN] = EL_SATURN};
    static const float mag[] = {[ASTRI_MERCURY] = 0.0f, [ASTRI_VENUS] = -4.2f, [ASTRI_MARS] = 0.5f,
                                [ASTRI_JUPITER] = -2.3f, [ASTRI_SATURN] = 0.6f};
    double T = d / 36525.0;
    float p[3], e[3];
    heliocentric(&ELEMENTS[index[body]], T, p);
    heliocentric(&ELEMENTS[EL_EARTH], T, e);
    ecliptic_to_equatorial(p[0] - e[0], p[1] - e[1], p[2] - e[2], out);
    out->mag = mag[body];
}

const char *astri_body_name(astri_body_t body)
{
    static const char *const names[] = {"Sole", "Luna", "Mercurio", "Venere", "Marte", "Giove", "Saturno"};
    return body < ASTRI_BODIES ? names[body] : "";
}

void astri_moon_phase(time_t t, float *illuminated, bool *waxing)
{
    astri_position_t s, m;
    astri_body(ASTRI_SUN, t, &s);
    astri_body(ASTRI_MOON, t, &m);
    float cos_elong = sinf(s.dec * DEG) * sinf(m.dec * DEG) +
                      cosf(s.dec * DEG) * cosf(m.dec * DEG) * cosf((m.ra - s.ra) * DEG);
    *illuminated = (1.0f - cos_elong) * 0.5f;
    float ahead = fmodf(m.ra - s.ra + 360.0f, 360.0f);   /* Moon east of the Sun: waxing */
    *waxing = ahead < 180.0f;
}
