/* Pieno's pure maths and parsing (see fuel.h). Freestanding: no libc calls. */
#include "fuel.h"

#include <stddef.h>

#define MAX_FIELDS 16

static bool same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;
}

/* Split on '|' in place; returns the field count. */
static int split(char *line, char **fields)
{
    int count = 0;
    fields[count++] = line;
    for (char *c = line; *c != '\0'; c++)
    {
        if (*c == '\r' || *c == '\n')
        {
            *c = '\0';
            break;
        }
        if (*c == '|')
        {
            *c = '\0';
            if (count == MAX_FIELDS)
            {
                return -1;
            }
            fields[count++] = c + 1;
        }
    }
    return count;
}

static bool parse_id(const char *text, uint32_t *out)
{
    uint32_t value = 0;
    if (*text == '\0')
    {
        return false;
    }
    for (; *text != '\0'; text++)
    {
        if (*text < '0' || *text > '9' || value > 400000000u)
        {
            return false;
        }
        value = value * 10 + (uint32_t)(*text - '0');
    }
    *out = value;
    return value != 0;
}

double pn_atof(const char *text, bool *ok)
{
    double value = 0, scale = 1;
    bool negative = false, digits = false, fraction = false;
    while (*text == ' ')
    {
        text++;
    }
    if (*text == '-' || *text == '+')
    {
        negative = *text == '-';
        text++;
    }
    for (; *text != '\0'; text++)
    {
        if (*text >= '0' && *text <= '9')
        {
            digits = true;
            if (fraction)
            {
                scale /= 10;
                value += (*text - '0') * scale;
            }
            else
            {
                value = value * 10 + (*text - '0');
            }
        }
        else if ((*text == '.' || *text == ',') && !fraction)
        {
            fraction = true;
        }
        else if (*text == ' ')
        {
            break;
        }
        else
        {
            *ok = false;
            return 0;
        }
    }
    *ok = digits;
    return negative ? -value : value;
}

bool pn_parse_station(char *line, pn_station_line_t *out)
{
    char *f[MAX_FIELDS];
    int n = split(line, f);
    /* id|operator|brand|type|name...|address|town|province|lat|lon */
    if (n < 10 || !parse_id(f[0], &out->id))
    {
        return false;
    }
    bool ok_lat, ok_lon;
    out->lat = pn_atof(f[n - 2], &ok_lat);
    out->lon = pn_atof(f[n - 1], &ok_lon);
    if (!ok_lat || !ok_lon || out->lat < 35 || out->lat > 48 || out->lon < 6 || out->lon > 19)
    {
        return false;   /* Italy only: 0/0 and swapped coordinates exist in the data */
    }
    out->brand = f[2];
    out->name = f[4];
    /* A name with '|' inside spans several fields: glue them back. */
    for (int i = 5; i < n - 5; i++)
    {
        f[i][-1] = '|';
    }
    out->address = f[n - 5];
    out->town = f[n - 4];
    out->province = f[n - 3];
    return true;
}

int64_t pn_days_from_civil(int year, int month, int day)
{
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t yoe = year - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int digits(const char *text, int count, bool *ok)
{
    int value = 0;
    for (int i = 0; i < count; i++)
    {
        if (text[i] < '0' || text[i] > '9')
        {
            *ok = false;
            return 0;
        }
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

int64_t pn_parse_when(const char *t)
{
    /* dd/mm/yyyy hh:mm:ss */
    bool ok = true;
    for (int i = 0; i < 19; i++)
    {
        if (t[i] == '\0')
        {
            return -1;
        }
    }
    if (t[2] != '/' || t[5] != '/' || t[10] != ' ' || t[13] != ':' || t[16] != ':')
    {
        return -1;
    }
    int day = digits(t, 2, &ok), month = digits(t + 3, 2, &ok), year = digits(t + 6, 4, &ok);
    int hour = digits(t + 11, 2, &ok), minute = digits(t + 14, 2, &ok), second = digits(t + 17, 2, &ok);
    if (!ok || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59)
    {
        return -1;
    }
    return pn_days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

bool pn_parse_price(char *line, pn_price_line_t *out)
{
    char *f[MAX_FIELDS];
    int n = split(line, f);
    /* id|product|price|isSelf|dd/mm/yyyy hh:mm:ss */
    if (n != 5 || !parse_id(f[0], &out->id))
    {
        return false;
    }
    bool ok;
    double price = pn_atof(f[2], &ok);
    if (!ok || price < 0.2 || price > 5)
    {
        return false;   /* zero and absurd prices are typing errors of the operators */
    }
    if ((f[3][0] != '0' && f[3][0] != '1') || f[3][1] != '\0')
    {
        return false;
    }
    out->product = f[1];
    out->price = (float)price;
    out->self = f[3][0] == '1';
    out->when = pn_parse_when(f[4]);
    return out->when >= 0;
}

int pn_fuel_of(const char *product)
{
    if (same(product, "Benzina"))
    {
        return PN_BENZINA;
    }
    if (same(product, "Gasolio"))
    {
        return PN_GASOLIO;
    }
    if (same(product, "GPL"))
    {
        return PN_GPL;
    }
    if (same(product, "Metano"))
    {
        return PN_METANO;
    }
    return -1;
}

double pn_cos(double x)
{
    /* Range reduction to [-pi, pi], then Taylor to x^12 (error < 1e-9 at pi/2). */
    const double pi = 3.14159265358979323846;
    while (x > pi)
    {
        x -= 2 * pi;
    }
    while (x < -pi)
    {
        x += 2 * pi;
    }
    bool flip = false;
    if (x > pi / 2)
    {
        x = pi - x;
        flip = true;
    }
    else if (x < -pi / 2)
    {
        x = -pi - x;
        flip = true;
    }
    double x2 = x * x, term = 1, sum = 1;
    for (int k = 1; k <= 6; k++)
    {
        term *= -x2 / ((2 * k - 1) * (2 * k));
        sum += term;
    }
    return flip ? -sum : sum;
}

static double square_root(double v)
{
    if (v <= 0)
    {
        return 0;
    }
    double r = v > 1 ? v : 1;
    for (int i = 0; i < 40; i++)
    {
        r = 0.5 * (r + v / r);
    }
    return r;
}

double pn_distance_km(double lat1, double lon1, double lat2, double lon2)
{
    const double rad = 3.14159265358979323846 / 180;
    double x = (lon2 - lon1) * rad * pn_cos((lat1 + lat2) / 2 * rad);
    double y = (lat2 - lat1) * rad;
    return 6371.0 * square_root(x * x + y * y);
}

float pn_real_price(float price, float km, float litres, float km_per_litre)
{
    if (litres <= 0 || km_per_litre <= 0)
    {
        return price;
    }
    float detour_litres = km * 2 * 1.3f / km_per_litre;
    return price * (litres + detour_litres) / litres;
}

bool pn_stats(float *v, int n, float *min, float *median, float *max)
{
    if (n <= 0)
    {
        return false;
    }
    for (int i = 1; i < n; i++)   /* insertion sort: n is a few hundred at most */
    {
        float key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key)
        {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
    *min = v[0];
    *max = v[n - 1];
    *median = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    return true;
}
