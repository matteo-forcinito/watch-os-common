/* Pieno parsing and maths tests, on real MIMIT lines. Returns the failing line, 0 if all pass. */
#include "fuel.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static int near(double a, double b, double tolerance) { return a - b < tolerance && b - a < tolerance; }

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void copy(char *out, const char *in)
{
    while ((*out++ = *in++) != '\0') { }
}

int pieno_tests(void)
{
    static char line[512];
    pn_station_line_t s;
    pn_price_line_t p;
    bool ok;

    /* Numbers. */
    CHECK(near(pn_atof("1.899", &ok), 1.899, 1e-9) && ok);
    CHECK(near(pn_atof("-12.5", &ok), -12.5, 1e-9) && ok);
    CHECK(near(pn_atof("42.29466", &ok), 42.29466, 1e-9) && ok);
    pn_atof("1.2x", &ok);
    CHECK(!ok);
    pn_atof("", &ok);
    CHECK(!ok);

    /* A normal station line. */
    copy(line, "59183|ENIMOOV S.P.A.|Agip Eni|Stradale|19829 AGRIGENTO|SS.189 KM. 64+649 - C.DA SAN MICHELE  S.N.C  |AGRIGENTO|AG|37.333935|13.595533");
    CHECK(pn_parse_station(line, &s));
    CHECK(s.id == 59183 && eq(s.brand, "Agip Eni") && eq(s.name, "19829 AGRIGENTO") && eq(s.town, "AGRIGENTO"));
    CHECK(eq(s.province, "AG") && near(s.lat, 37.333935, 1e-9) && near(s.lon, 13.595533, 1e-9));

    /* A name with '|' inside: read from both ends, the name glued back. */
    copy(line, "40820|STOIL SIMPLE|Pompe Bianche|Stradale|STOIL SIMPLE | gestori.prezzibenzina.it|STR. PROV.LE 82 SPINETTA SALE  15122|ALESSANDRIA|AL|44.91704718250436|8.70067298412323");
    CHECK(pn_parse_station(line, &s));
    CHECK(s.id == 40820 && eq(s.name, "STOIL SIMPLE | gestori.prezzibenzina.it"));
    CHECK(eq(s.address, "STR. PROV.LE 82 SPINETTA SALE  15122") && eq(s.town, "ALESSANDRIA") && near(s.lon, 8.70067298, 1e-6));

    /* Header, missing coordinates, outside Italy, CRLF. */
    copy(line, "idImpianto|Gestore|Bandiera|Tipo Impianto|Nome Impianto|Indirizzo|Comune|Provincia|Latitudine|Longitudine");
    CHECK(!pn_parse_station(line, &s));
    copy(line, "1|a|b|c|d|e|f|g||");
    CHECK(!pn_parse_station(line, &s));
    copy(line, "2|a|b|c|d|e|f|g|0|0");
    CHECK(!pn_parse_station(line, &s));
    copy(line, "3|a|b|c|d|e|f|RM|41.9|12.5\r\n");
    CHECK(pn_parse_station(line, &s) && eq(s.province, "RM") && near(s.lon, 12.5, 1e-9));

    /* Prices. */
    copy(line, "3464|Benzina|2.349|0|09/10/2026 19:30:07");
    CHECK(pn_parse_price(line, &p));
    CHECK(p.id == 3464 && eq(p.product, "Benzina") && near(p.price, 2.349, 1e-6) && !p.self);
    CHECK(p.when == pn_days_from_civil(2026, 10, 9) * 86400 + 19 * 3600 + 30 * 60 + 7);
    copy(line, "3464|GPL|0.739|1|12/10/2026 06:31:01");
    CHECK(pn_parse_price(line, &p) && p.self && pn_fuel_of(p.product) == PN_GPL);
    copy(line, "3464|Benzina|0|1|12/10/2026 06:31:01");
    CHECK(!pn_parse_price(line, &p));        /* zero price: an operator's typing error */
    copy(line, "3464|Benzina|19.99|1|12/10/2026 06:31:01");
    CHECK(!pn_parse_price(line, &p));        /* absurd price */
    copy(line, "3464|Benzina|1.9|2|12/10/2026 06:31:01");
    CHECK(!pn_parse_price(line, &p));
    copy(line, "3464|Benzina|1.9|1|12-10-2026 06:31:01");
    CHECK(!pn_parse_price(line, &p));
    copy(line, "Estrazione del 2026-10-12");
    CHECK(!pn_parse_price(line, &p));

    /* Products. */
    CHECK(pn_fuel_of("Benzina") == PN_BENZINA && pn_fuel_of("Gasolio") == PN_GASOLIO);
    CHECK(pn_fuel_of("Metano") == PN_METANO && pn_fuel_of("Blue Super") == -1 && pn_fuel_of("Benzina speciale") == -1);

    /* Dates. */
    CHECK(pn_days_from_civil(1970, 1, 1) == 0 && pn_days_from_civil(2000, 3, 1) == 11017);
    CHECK(pn_parse_when("29/02/2024 00:00:00") == pn_days_from_civil(2024, 2, 29) * 86400);
    CHECK(pn_parse_when("31/13/2024 00:00:00") == -1 && pn_parse_when("1/1/2024") == -1);

    /* Geometry: cos and a known distance (Civita Castellana -> Corchiano ~7.2 km). */
    CHECK(near(pn_cos(0), 1, 1e-9) && near(pn_cos(3.14159265358979 / 3), 0.5, 1e-7));
    CHECK(near(pn_cos(3.14159265358979), -1, 1e-7) && near(pn_cos(-2 * 3.14159265358979), 1, 1e-7));
    double km = pn_distance_km(42.2946, 12.4103, 42.3440, 12.3540);
    CHECK(km > 6.9 && km < 7.4);
    CHECK(near(pn_distance_km(42, 12, 42, 12), 0, 1e-9));
    CHECK(near(pn_distance_km(42, 12, 43, 12), 111.19, 0.05));

    /* Real price: 7 km away, 40 L fill, 15 km/L -> 1.21 L more at the same price. */
    float real = pn_real_price(1.80f, 7, 40, 15);
    CHECK(near(real, 1.80 * (40 + 7 * 2 * 1.3 / 15) / 40, 1e-5));
    CHECK(near(pn_real_price(1.80f, 0, 40, 15), 1.80, 1e-6) && near(pn_real_price(1.8f, 5, 0, 15), 1.8, 1e-6));

    /* Stats. */
    float v[] = {2.1f, 1.9f, 2.0f, 2.3f};
    float lo, mid, hi;
    CHECK(pn_stats(v, 4, &lo, &mid, &hi) && near(lo, 1.9, 1e-6) && near(mid, 2.05, 1e-6) && near(hi, 2.3, 1e-6));
    float one[] = {1.5f};
    CHECK(pn_stats(one, 1, &lo, &mid, &hi) && near(mid, 1.5, 1e-6));
    CHECK(!pn_stats(v, 0, &lo, &mid, &hi));
    return 0;
}

#ifdef PIENO_EMULATOR_ENTRY
__attribute__((naked)) void _start(void)
{
    /* Full-size ebreak: the runner looks for the 4-byte instruction. */
    __asm__ volatile(".option push\n .option norvc\n call pieno_tests\n ebreak\n .option pop\n");
}
#else
#include <stdio.h>
int main(void)
{
    int line = pieno_tests();
    if (line) printf("FAIL at line %d\n", line);
    return line;
}
#endif
