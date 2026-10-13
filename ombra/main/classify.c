/*
 * Advertising classifier (see classify.h). Freestanding C: no libc calls, so
 * the same file runs in the RISC-V emulator tests and on the watch.
 */
#include "classify.h"

/* AD types (Bluetooth Core Supplement, part A). */
#define AD_FLAGS 0x01
#define AD_UUID16_SOME 0x02
#define AD_UUID16_ALL 0x03
#define AD_NAME_SHORT 0x08
#define AD_NAME_FULL 0x09
#define AD_TX_POWER 0x0A
#define AD_SERVICE_DATA16 0x16
#define AD_APPEARANCE 0x19
#define AD_MANUFACTURER 0xFF

#define COMPANY_APPLE 0x004C
#define COMPANY_MICROSOFT 0x0006
#define COMPANY_SAMSUNG 0x0075

static void zero(void *data, size_t size)
{
    uint8_t *p = data;
    for (size_t i = 0; i < size; i++)
        p[i] = 0;
}

static void set_text(char *out, size_t size, const char *text)
{
    size_t i = 0;
    for (; text[i] && i + 1 < size; i++)
        out[i] = text[i];
    out[i] = 0;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

/* Case-insensitive substring test, needle in lower case. */
static bool contains(const char *text, const char *needle)
{
    for (; *text; text++)
    {
        size_t i = 0;
        while (needle[i] && lower(text[i]) == needle[i])
            i++;
        if (!needle[i])
            return true;
    }
    return false;
}

static bool contains_any(const char *text, const char *const *needles)
{
    for (; *needles; needles++)
        if (contains(text, *needles))
            return true;
    return false;
}

/* ------------------------------------------------------------ companies */

typedef struct
{
    uint16_t id;
    const char *name;
} company_t;

/* A selection of Bluetooth SIG company identifiers seen around people. */
static const company_t s_companies[] = {
    {0x0001, "Nokia"},         {0x0002, "Intel"},          {0x0006, "Microsoft"},
    {0x000A, "Qualcomm"},      {0x000D, "Texas Instruments"}, {0x000F, "Broadcom"},
    {0x001D, "Qualcomm"},      {0x0030, "STMicroelectronics"}, {0x0046, "MediaTek"},
    {0x004C, "Apple"},         {0x0057, "Harman (JBL)"},   {0x0059, "Nordic Semiconductor"},
    {0x0065, "HP"},            {0x006B, "Polar"},          {0x0075, "Samsung"},
    {0x0078, "Nike"},          {0x0087, "Garmin"},         {0x009E, "Bose"},
    {0x00C4, "LG Electronics"}, {0x00E0, "Google"},        {0x012D, "Sony"},
    {0x0131, "Cypress"},       {0x0157, "Huami (Amazfit)"}, {0x0171, "Amazon"},
    {0x01DA, "Logitech"},      {0x022B, "Tesla"},          {0x027D, "Huawei"},
    {0x02E5, "Espressif"},     {0x02FF, "Silicon Labs"},   {0x038F, "Xiaomi"},
    {0x0499, "Ruuvi"},         {0x05A7, "Sonos"},
};

const char *classify_company(uint16_t company)
{
    for (size_t i = 0; i < sizeof(s_companies) / sizeof(s_companies[0]); i++)
        if (s_companies[i].id == company)
            return s_companies[i].name;
    return 0;
}

const char *classify_kind_name(ombra_kind_t kind)
{
    static const char *const names[OMBRA_KIND_COUNT] = {
        "Sconosciuto", "Tracker", "Telefono", "Computer", "Orologio", "Audio",
        "TV e media", "Comando", "Beacon", "Casa", "Salute", "Veicolo",
    };
    return kind < OMBRA_KIND_COUNT ? names[kind] : names[0];
}

const char *classify_tracker_name(ombra_tracker_t tracker)
{
    switch (tracker)
    {
    case OMBRA_TRACKER_AIRTAG: return "AirTag";
    case OMBRA_TRACKER_FINDMY: return "Accessorio Dov'è";
    case OMBRA_TRACKER_APPLE: return "Apple (Dov'è)";
    case OMBRA_TRACKER_AIRPODS: return "AirPods (Dov'è)";
    case OMBRA_TRACKER_SMARTTAG: return "Galaxy SmartTag";
    case OMBRA_TRACKER_TILE: return "Tile";
    case OMBRA_TRACKER_CHIPOLO: return "Chipolo";
    case OMBRA_TRACKER_GOOGLE: return "Tag Trova il mio dispositivo";
    default: return "";
    }
}

/* ------------------------------------------------------------ distance */

/* 100 * 10^(d/22) for d = -30..70 dB (path loss exponent 2.2), in cm. */
static const uint32_t s_distance[101] = {
    4, 5, 5, 6, 7, 7, 8, 9, 10, 11, 12, 14, 15, 17, 19, 21, 23, 26, 28, 32, 35, 39, 43, 48, 53, 59,
    66, 73, 81, 90, 100, 111, 123, 137, 152, 169, 187, 208, 231, 257, 285, 316, 351, 390, 433, 481,
    534, 593, 658, 731, 811, 901, 1000, 1110, 1233, 1369, 1520, 1688, 1874, 2081, 2310, 2565, 2848,
    3162, 3511, 3899, 4329, 4806, 5337, 5926, 6579, 7305, 8111, 9006, 10000, 11103, 12328, 13689,
    15199, 16876, 18738, 20806, 23101, 25650, 28480, 31623, 35112, 38986, 43288, 48064, 53367,
    59255, 65793, 73053, 81113, 90063, 100000, 111034, 123285, 136887, 151991,
};

uint32_t classify_distance_cm(int rssi, int tx_power_1m)
{
    int loss = tx_power_1m - rssi;
    if (loss < -30)
        loss = -30;
    if (loss > 70)
        loss = 70;
    return s_distance[loss + 30];
}

/* ------------------------------------------------------------ rules */

static void set_kind(ombra_adv_t *out, ombra_kind_t kind, const char *detail)
{
    /* A tracker verdict always wins; otherwise the first precise rule. */
    if (out->kind == OMBRA_KIND_TRACKER && kind != OMBRA_KIND_TRACKER)
        return;
    out->kind = (uint8_t)kind;
    if (detail && (!out->detail[0] || kind == OMBRA_KIND_TRACKER))
        set_text(out->detail, sizeof(out->detail), detail);
}

static void set_tracker(ombra_adv_t *out, ombra_tracker_t tracker, bool separated, const char *detail)
{
    out->kind = OMBRA_KIND_TRACKER;
    out->tracker = (uint8_t)tracker;
    out->separated = out->separated || separated;
    set_text(out->detail, sizeof(out->detail), detail);
}

static void add_service(ombra_adv_t *out, uint16_t uuid)
{
    for (unsigned i = 0; i < out->service_count; i++)
        if (out->services[i] == uuid)
            return;
    if (out->service_count < sizeof(out->services) / sizeof(out->services[0]))
        out->services[out->service_count++] = uuid;
}

static void apple(ombra_adv_t *out, const uint8_t *d, size_t n)
{
    /* Continuity messages: [type][length][payload]... */
    size_t i = 0;
    while (i + 2 <= n)
    {
        uint8_t type = d[i], length = d[i + 1];
        const uint8_t *p = d + i + 2;
        if (i + 2 + length > n)
            break;
        if (!out->apple_type)
            out->apple_type = type;
        if (type == 0x12 && length >= 2)
        {
            /* Find My: 25 bytes = full public key = separated from the owner. */
            bool separated = length == 25;
            uint8_t status = p[0];
            out->battery = (int8_t)(status >> 6);
            switch ((status >> 4) & 3)
            {
            case 1: set_tracker(out, OMBRA_TRACKER_AIRTAG, separated, separated ? "AirTag lontano dal proprietario" : "AirTag vicino al proprietario"); break;
            case 2: set_tracker(out, OMBRA_TRACKER_FINDMY, separated, separated ? "Accessorio Dov'è separato" : "Accessorio Dov'è"); break;
            case 3: set_tracker(out, OMBRA_TRACKER_AIRPODS, separated, "AirPods in rete Dov'è"); break;
            default: set_tracker(out, OMBRA_TRACKER_APPLE, separated, "Apple in rete Dov'è"); break;
            }
        }
        else if (type == 0x02 && length == 21)
        {
            set_kind(out, OMBRA_KIND_BEACON, "iBeacon");
            out->tx_power = (int8_t)p[20];
            out->has_tx_power = true;
        }
        else if (type == 0x07)
            set_kind(out, OMBRA_KIND_AUDIO, "AirPods o Beats");
        else if (type == 0x09 || type == 0x0A)
            set_kind(out, OMBRA_KIND_TV, "Ricevitore AirPlay");
        else if (type == 0x0D || type == 0x0E)
            set_kind(out, OMBRA_KIND_PHONE, "Hotspot Apple");
        else if (type == 0x10 || type == 0x0C || type == 0x0F || type == 0x05)
            set_kind(out, OMBRA_KIND_PHONE, "iPhone, iPad o Mac");
        i += 2u + length;
    }
    if (out->kind == OMBRA_KIND_UNKNOWN)
        set_kind(out, OMBRA_KIND_PHONE, "Dispositivo Apple");
}

static void manufacturer(ombra_adv_t *out, const uint8_t *d, size_t n)
{
    if (n < 2)
        return;
    uint16_t company = (uint16_t)(d[0] | d[1] << 8);
    if (out->company == 0xFFFF)
        out->company = company;
    d += 2;
    n -= 2;
    if (company == COMPANY_APPLE)
        apple(out, d, n);
    else if (company == COMPANY_MICROSOFT && n >= 1)
        set_kind(out, d[0] == 0x01 ? OMBRA_KIND_COMPUTER : OMBRA_KIND_INPUT,
                 d[0] == 0x01 ? "PC Windows" : "Accessorio Swift Pair");
}

static void service_data(ombra_adv_t *out, const uint8_t *d, size_t n)
{
    if (n < 2)
        return;
    uint16_t uuid = (uint16_t)(d[0] | d[1] << 8);
    add_service(out, uuid);
    d += 2;
    n -= 2;
    if (uuid == 0xFD5A)
        set_tracker(out, OMBRA_TRACKER_SMARTTAG, true, "Samsung SmartTag");
    else if (uuid == 0xFEAA && n >= 1)
    {
        if (d[0] == 0x40 || d[0] == 0x41)
            set_tracker(out, OMBRA_TRACKER_GOOGLE, d[0] == 0x41,
                        d[0] == 0x41 ? "Tag Google lontano dal proprietario" : "Tag Google");
        else
        {
            set_kind(out, OMBRA_KIND_BEACON, "Eddystone");
            if ((d[0] == 0x00 || d[0] == 0x10) && n >= 2)
            {
                /* UID/URL frames carry the power at 0 m: +41 dB is the usual 1 m loss. */
                out->tx_power = (int8_t)((int8_t)d[1] - 41);
                out->has_tx_power = true;
            }
        }
    }
}

static void service(ombra_adv_t *out, uint16_t uuid)
{
    add_service(out, uuid);
    switch (uuid)
    {
    case 0xFEED:
    case 0xFEEC: set_tracker(out, OMBRA_TRACKER_TILE, true, "Tile"); break;
    case 0xFE33:
    case 0xFE65: set_tracker(out, OMBRA_TRACKER_CHIPOLO, true, "Chipolo"); break;
    case 0xFD5A: set_tracker(out, OMBRA_TRACKER_SMARTTAG, true, "Samsung SmartTag"); break;
    case 0x1812: set_kind(out, OMBRA_KIND_INPUT, "Tastiera, mouse o comando"); break;
    case 0x180D: set_kind(out, OMBRA_KIND_WATCH, "Cardiofrequenzimetro"); break;
    case 0x181D:
    case 0x1810:
    case 0x1809:
    case 0x1808:
    case 0x1822: set_kind(out, OMBRA_KIND_HEALTH, "Dispositivo per la salute"); break;
    case 0x1816:
    case 0x1818: set_kind(out, OMBRA_KIND_VEHICLE, "Sensore bici"); break;
    case 0xFE2C: set_kind(out, OMBRA_KIND_AUDIO, "Google Fast Pair"); break;
    case 0xFE95: set_kind(out, OMBRA_KIND_HOME, "Xiaomi Mi Home"); break;
    case 0xFE0F: set_kind(out, OMBRA_KIND_HOME, "Philips Hue"); break;
    case 0xFEBE: set_kind(out, OMBRA_KIND_AUDIO, "Bose"); break;
    case 0xFE03: set_kind(out, OMBRA_KIND_HOME, "Amazon Alexa"); break;
    case 0xFD6F: set_kind(out, OMBRA_KIND_PHONE, "Telefono (notifiche esposizione)"); break;
    default: break;
    }
}

static void appearance(ombra_adv_t *out)
{
    switch (out->appearance >> 6)
    {
    case 1: set_kind(out, OMBRA_KIND_PHONE, 0); break;
    case 2: set_kind(out, OMBRA_KIND_COMPUTER, 0); break;
    case 3: set_kind(out, OMBRA_KIND_WATCH, 0); break;
    case 6:
    case 15:
    case 0x2A: set_kind(out, OMBRA_KIND_INPUT, 0); break;
    case 10:
    case 0x21:
    case 0x25:
    case 0x29: set_kind(out, OMBRA_KIND_AUDIO, 0); break;
    case 0x27:
    case 0x28: set_kind(out, OMBRA_KIND_TV, 0); break;
    case 12:
    case 13:
    case 14:
    case 16:
    case 0x31:
    case 0x32:
    case 0x34: set_kind(out, OMBRA_KIND_HEALTH, 0); break;
    case 18:
    case 0x23:
    case 0x33: set_kind(out, OMBRA_KIND_VEHICLE, 0); break;
    case 0x24: set_kind(out, OMBRA_KIND_HOME, 0); break;
    default: break;
    }
}

/* Names are the last resort: what people call their own devices. */
static void name_rules(ombra_adv_t *out)
{
    static const char *const audio[] = {"buds", "airpods", "headphone", "earbud", "jbl", "bose", "beats",
                                        "soundcore", "speaker", "wh-", "wf-", "sony", "marshall", "jabra",
                                        "sennheiser", "soundbar", 0};
    static const char *const watch[] = {"watch", "band", "amazfit", "garmin", "forerunner", "fenix", "fitbit",
                                        "polar", "whoop", "mi smart", 0};
    static const char *const tv[] = {"tv", "bravia", "fire tv", "chromecast", "roku", "shield", 0};
    static const char *const input[] = {"keyboard", "mouse", "mx ", "k380", "controller", "xbox", "dualsense",
                                        "joy-con", "pro controller", "remote", "pencil", 0};
    static const char *const phone[] = {"iphone", "galaxy", "pixel", "redmi", "oneplus", "phone", "xiaomi", "oppo",
                                        "huawei", "motorola", 0};
    static const char *const computer[] = {"macbook", "laptop", "desktop", "imac", "thinkpad", "surface", 0};
    static const char *const vehicle[] = {"tesla", "car", "ninebot", "scooter", "bike", "vanmoof", 0};
    static const char *const home[] = {"bulb", "light", "hue", "plug", "sensor", "thermo", "switch", "lock",
                                       "nuki", "lamp", "shelly", "tado", "netatmo", "govee", 0};
    static const char *const health[] = {"scale", "glucose", "oximeter", "bp ", "omron", 0};
    const char *n = out->name;
    if (!n[0])
        return;
    if (contains_any(n, audio)) set_kind(out, OMBRA_KIND_AUDIO, 0);
    else if (contains_any(n, watch)) set_kind(out, OMBRA_KIND_WATCH, 0);
    else if (contains_any(n, tv)) set_kind(out, OMBRA_KIND_TV, 0);
    else if (contains_any(n, input)) set_kind(out, OMBRA_KIND_INPUT, 0);
    else if (contains_any(n, phone)) set_kind(out, OMBRA_KIND_PHONE, 0);
    else if (contains_any(n, computer)) set_kind(out, OMBRA_KIND_COMPUTER, 0);
    else if (contains_any(n, vehicle)) set_kind(out, OMBRA_KIND_VEHICLE, 0);
    else if (contains_any(n, home)) set_kind(out, OMBRA_KIND_HOME, 0);
    else if (contains_any(n, health)) set_kind(out, OMBRA_KIND_HEALTH, 0);
}

void classify_adv(const uint8_t *data, size_t length, ombra_adv_t *out)
{
    zero(out, sizeof(*out));
    out->company = 0xFFFF;
    out->battery = -1;
    size_t i = 0;
    while (data && i < length)
    {
        uint8_t field = data[i];
        if (field == 0 || i + 1 + field > length)
            break;
        uint8_t type = data[i + 1];
        const uint8_t *d = data + i + 2;
        size_t n = field - 1u;
        switch (type)
        {
        case AD_FLAGS:
            out->connectable_hint = n >= 1 && (d[0] & 0x02);
            break;
        case AD_UUID16_SOME:
        case AD_UUID16_ALL:
            for (size_t k = 0; k + 1 < n; k += 2)
                service(out, (uint16_t)(d[k] | d[k + 1] << 8));
            break;
        case AD_NAME_SHORT:
        case AD_NAME_FULL:
            if (type == AD_NAME_FULL || !out->name[0])
            {
                size_t k = 0;
                for (; k < n && k + 1 < sizeof(out->name); k++)
                    out->name[k] = d[k] >= 0x20 && d[k] != 0x7F ? (char)d[k] : '?';
                out->name[k] = 0;
            }
            break;
        case AD_TX_POWER:
            if (n >= 1 && !out->has_tx_power)
            {
                /* Advertised power at the antenna; about 41 dB are lost in the first metre. */
                out->tx_power = (int8_t)((int8_t)d[0] - 41);
                out->has_tx_power = true;
            }
            break;
        case AD_SERVICE_DATA16:
            service_data(out, d, n);
            break;
        case AD_APPEARANCE:
            if (n >= 2)
                out->appearance = (uint16_t)(d[0] | d[1] << 8);
            break;
        case AD_MANUFACTURER:
            manufacturer(out, d, n);
            break;
        default:
            break;
        }
        i += 1u + field;
    }
    if (out->appearance)
        appearance(out);
    name_rules(out);
    if (out->company == COMPANY_SAMSUNG && out->kind == OMBRA_KIND_UNKNOWN)
        set_kind(out, OMBRA_KIND_PHONE, "Dispositivo Samsung");
}

void classify_merge(ombra_adv_t *into, const ombra_adv_t *more)
{
    if (more->name[0] && !into->name[0])
        set_text(into->name, sizeof(into->name), more->name);
    if (into->company == 0xFFFF)
        into->company = more->company;
    if (!into->appearance)
        into->appearance = more->appearance;
    if (!into->has_tx_power && more->has_tx_power)
    {
        into->tx_power = more->tx_power;
        into->has_tx_power = true;
    }
    for (unsigned i = 0; i < more->service_count; i++)
        add_service(into, more->services[i]);
    if (more->kind == OMBRA_KIND_TRACKER)
    {
        into->kind = OMBRA_KIND_TRACKER;
        into->tracker = more->tracker;
        into->separated = into->separated || more->separated;
        if (more->battery >= 0)
            into->battery = more->battery;
        set_text(into->detail, sizeof(into->detail), more->detail);
    }
    else if (into->kind == OMBRA_KIND_UNKNOWN && more->kind != OMBRA_KIND_UNKNOWN)
    {
        into->kind = more->kind;
        set_text(into->detail, sizeof(into->detail), more->detail);
    }
    if (into->kind == OMBRA_KIND_UNKNOWN)
        name_rules(into);
}
