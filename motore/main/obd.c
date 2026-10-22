/*
 * OBD-II over an ELM327: one task owns the adapter.
 *
 *   connect   ATZ, echo/spaces/headers off, adaptive timing, automatic
 *             protocol; "0100" wakes the car's bus (up to ~10 s the first time).
 *   discover  supported PIDs (0100, 0120, ... while the chain goes on), the
 *             protocol name, the VIN (0902), trouble codes and readiness.
 *   live      fast values every cycle (rpm, speed, load, throttle), the rest
 *             one per cycle; battery voltage (AT RV) every few seconds; trip
 *             integration from speed and fuel flow.
 *
 * Fuel flow: PID 5E when the car has it, else MAF, else speed-density from
 * MAP, intake temperature, rpm and displacement. The air is real whatever
 * the engine burns, so with the fuel set to GPL the litres are LPG litres
 * (the car's ECU still believes it runs on petrol).
 *
 * Safety: nothing here writes to the car except mode 04 (clear codes), and
 * only when unlocked with the code, the car stopped and the engine off.
 */
#include "motore.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "motore";

#define UNLOCK_US        (10LL * 60 * 1000000)
#define FRESH_US         (5LL * 1000000)
#define BATTERY_EVERY_US (3LL * 1000000)
#define CODES_EVERY_US   (60LL * 1000000)

static const mt_value_info_t s_info[MT_VALUES] = {
    [MT_RPM] = {0x0C, "rpm", "Giri motore", "rpm", 0, 7000, 0, true},
    [MT_SPEED] = {0x0D, "speed", "Velocità", "km/h", 0, 220, 0, true},
    [MT_COOLANT] = {0x05, "coolant", "Liquido refrigerante", "°C", -40, 130, 0, false},
    [MT_INTAKE_T] = {0x0F, "intake", "Aria aspirata", "°C", -40, 80, 0, false},
    [MT_LOAD] = {0x04, "load", "Carico motore", "%", 0, 100, 0, true},
    [MT_THROTTLE] = {0x11, "throttle", "Farfalla", "%", 0, 100, 0, true},
    [MT_MAP] = {0x0B, "map", "Pressione collettore", "kPa", 0, 255, 0, true},
    [MT_MAF] = {0x10, "maf", "Flusso d'aria", "g/s", 0, 200, 1, true},
    [MT_STFT1] = {0x06, "stft", "Correzione breve", "%", -25, 25, 1, false},
    [MT_LTFT1] = {0x07, "ltft", "Correzione lunga", "%", -25, 25, 1, false},
    [MT_TIMING] = {0x0E, "timing", "Anticipo", "°", -20, 50, 1, false},
    [MT_O2_V] = {0x14, "o2", "Sonda lambda B1S1", "V", 0, 1.275f, 3, false},
    [MT_FUEL_LEVEL] = {0x2F, "fuel_level", "Livello carburante", "%", 0, 100, 0, false},
    [MT_BARO] = {0x33, "baro", "Pressione atmosferica", "kPa", 70, 110, 0, false},
    [MT_AMBIENT] = {0x46, "ambient", "Temperatura esterna", "°C", -30, 50, 0, false},
    [MT_RUNTIME] = {0x1F, "runtime", "Motore acceso da", "s", 0, 36000, 0, false},
    [MT_DIST_MIL] = {0x21, "dist_mil", "Km con spia accesa", "km", 0, 65535, 0, false},
    [MT_DIST_CLEAR] = {0x31, "dist_clear", "Km da cancellazione codici", "km", 0, 65535, 0, false},
    [MT_WARMUPS] = {0x30, "warmups", "Riscaldamenti da cancellazione", "", 0, 255, 0, false},
    [MT_MODULE_V] = {0x42, "module_v", "Tensione centralina", "V", 8, 16, 2, false},
    [MT_REL_THROTTLE] = {0x45, "rel_throttle", "Farfalla relativa", "%", 0, 100, 0, false},
    [MT_ODOMETER] = {0xA6, "odometer", "Contachilometri", "km", 0, 999999, 1, false},
    [MT_OIL_T] = {0x5C, "oil", "Olio motore", "°C", -40, 150, 0, false},
    [MT_FUEL_RATE] = {0x5E, "fuel_rate", "Consumo istantaneo", "l/h", 0, 30, 2, false},
    [MT_BATTERY] = {0x00, "battery", "Batteria", "V", 10, 15, 2, false},
};

static const char *const s_spark[] = {"Accensione (misfire)", "Sistema carburante", "Componenti", "Catalizzatore",
                                      "Catalizzatore riscaldato", "Sistema evaporativo", "Aria secondaria",
                                      "Climatizzatore", "Sonda lambda", "Riscaldatore sonda lambda", "EGR"};
static const char *const s_diesel[] = {"Accensione (misfire)", "Sistema carburante", "Componenti", "Catalizzatore NMHC",
                                       "Riduzione NOx/SCR", "Riservato", "Pressione turbo", "Riservato",
                                       "Sensore gas di scarico", "Filtro antiparticolato", "EGR / VVT"};

static SemaphoreHandle_t s_lock;
static mt_state_t s_state;
static mt_settings_t s_settings;
static int64_t s_unlocked_until;
static bool s_can;

/* Requests from other tasks. */
enum { REQ_RECONNECT = 1, REQ_CODES = 2, REQ_SCAN = 4, REQ_TRIP = 8, REQ_CLEAR = 16, REQ_CONSOLE = 32 };
static atomic_int s_requests;
static SemaphoreHandle_t s_wake, s_done;
static char s_console_cmd[48];
static char *s_console_out;
static size_t s_console_size;
static esp_err_t s_result;
static char s_why[96];

const mt_value_info_t *mt_value_info(mt_value_t v)
{
    return v < MT_VALUES ? &s_info[v] : NULL;
}

void mt_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void mt_unlock(void)
{
    xSemaphoreGive(s_lock);
}

const mt_state_t *mt_state(void)
{
    return &s_state;
}

bool mt_value_fresh(const mt_state_t *s, mt_value_t v)
{
    return s->updated[v] != 0 && esp_timer_get_time() - s->updated[v] < FRESH_US;
}

static void set_phase(mt_phase_t phase, const char *message)
{
    mt_lock();
    s_state.phase = phase;
    snprintf(s_state.message, sizeof(s_state.message), "%s", message);
    s_state.revision++;
    mt_unlock();
    ESP_LOGI(TAG, "%s", message);
}

/* ------------------------------------------------------------ settings */

static void settings_defaults(mt_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->link = MT_LINK_BLE;
    snprintf(s->host, sizeof(s->host), "192.168.0.10");
    s->port = 35000;
    s->fuel_type = MT_FUEL_PETROL;
    s->displacement_cc = 1200;
    s->volumetric = 0.80f;
    s->shift_rpm = 5500;
    /* Clio IV 1.2 16V (5 speed): km/h at 1000 rpm, refined later on the car. */
    const float clio[6] = {7.1f, 13.0f, 19.0f, 25.0f, 31.5f, 0};
    memcpy(s->gear_kmh, clio, sizeof(clio));
}

int mt_gear(const mt_settings_t *s, float speed, float rpm)
{
    if (speed < 3 || rpm < 700)
    {
        return 0;
    }
    float ratio = speed / (rpm / 1000.0f);
    int best = 0;
    float error = 0.12f;   /* within 12% of a gear, else clutch pressed */
    for (int g = 0; g < 6; g++)
    {
        if (s->gear_kmh[g] <= 0)
        {
            continue;
        }
        float e = fabsf(ratio - s->gear_kmh[g]) / s->gear_kmh[g];
        if (e < error)
        {
            error = e;
            best = g + 1;
        }
    }
    return best;
}

static void settings_load(void)
{
    settings_defaults(&s_settings);
    nvs_handle_t nvs;
    if (nvs_open("motore", NVS_READONLY, &nvs) == ESP_OK)
    {
        size_t size = sizeof(s_settings);
        mt_settings_t stored;
        if (nvs_get_blob(nvs, "settings", &stored, &size) == ESP_OK && size == sizeof(stored))
        {
            s_settings = stored;
        }
        nvs_close(nvs);
    }
}

void mt_settings(mt_settings_t *out)
{
    mt_lock();
    *out = s_settings;
    mt_unlock();
}

esp_err_t mt_set_settings(const mt_settings_t *in)
{
    mt_settings_t s = *in;
    s.fuel_type = s.fuel_type < MT_FUELS ? s.fuel_type : MT_FUEL_PETROL;
    s.displacement_cc = s.displacement_cc < 600 || s.displacement_cc > 8000 ? 1200 : s.displacement_cc;
    s.volumetric = s.volumetric < 0.5f || s.volumetric > 1.1f ? 0.80f : s.volumetric;
    s.shift_rpm = s.shift_rpm < 2000 || s.shift_rpm > 9000 ? 5500 : s.shift_rpm;
    s.host[sizeof(s.host) - 1] = 0;
    s.pin[sizeof(s.pin) - 1] = 0;
    s.ble_name[sizeof(s.ble_name) - 1] = 0;
    mt_lock();
    bool relink = s.link != s_settings.link || memcmp(s.ble_addr, s_settings.ble_addr, sizeof(s.ble_addr)) != 0 ||
                  strcmp(s.host, s_settings.host) != 0 || s.port != s_settings.port;
    s_settings = s;
    s_state.revision++;
    mt_unlock();
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("motore", NVS_READWRITE, &nvs);
    if (err == ESP_OK)
    {
        err = nvs_set_blob(nvs, "settings", &s, sizeof(s));
        if (err == ESP_OK)
        {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (relink)
    {
        mt_reconnect();
    }
    return err;
}

/* Air/fuel ratio and density of what the engine burns. */
static void fuel_constants(uint8_t fuel, float *afr, float *density)
{
    switch (fuel)
    {
    case MT_FUEL_LPG: *afr = 15.5f; *density = 0.54f; break;
    case MT_FUEL_DIESEL: *afr = 14.5f; *density = 0.832f; break;
    default: *afr = 14.7f; *density = 0.745f; break;
    }
}

/* ------------------------------------------------------------ requests */

static void request(int bit)
{
    atomic_fetch_or(&s_requests, bit);
    if (s_wake != NULL)
    {
        xSemaphoreGive(s_wake);
    }
}

void mt_reconnect(void) { request(REQ_RECONNECT); }
void mt_read_codes(void) { request(REQ_CODES); }
void mt_trip_reset(void) { request(REQ_TRIP); }

void mt_scan(void)
{
    request(REQ_SCAN);
}

int mt_scan_results(mt_found_t *out, int max)
{
    return mt_link_scan_results(out, max);
}

void mt_choose_ble(const mt_found_t *f)
{
    mt_settings_t s;
    mt_settings(&s);
    s.link = MT_LINK_BLE;
    memcpy(s.ble_addr, f->addr, sizeof(s.ble_addr));
    snprintf(s.ble_name, sizeof(s.ble_name), "%s", f->name[0] ? f->name : "Adattatore");
    mt_set_settings(&s);
}

bool mt_unlock_actions(const char *pin)
{
    mt_lock();
    bool ok = s_settings.pin[0] != 0 && pin != NULL && strcmp(pin, s_settings.pin) == 0;
    if (ok)
    {
        s_unlocked_until = esp_timer_get_time() + UNLOCK_US;
        s_state.unlocked = true;
        s_state.revision++;
    }
    mt_unlock();
    return ok;
}

void mt_lock_actions(void)
{
    mt_lock();
    s_unlocked_until = 0;
    s_state.unlocked = false;
    s_state.revision++;
    mt_unlock();
}

/* Runs a request in the OBD task and waits for its result. */
static esp_err_t run_in_task(int bit, int timeout_ms)
{
    xSemaphoreTake(s_done, 0);
    request(bit);
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return s_result;
}

esp_err_t mt_clear_codes(char *why, size_t size)
{
    static SemaphoreHandle_t once;
    if (once == NULL)
    {
        once = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(once, portMAX_DELAY);
    esp_err_t err = run_in_task(REQ_CLEAR, 15000);
    snprintf(why, size, "%s", err == ESP_ERR_TIMEOUT ? "Nessuna risposta" : s_why);
    xSemaphoreGive(once);
    return err;
}

/* Expert console: reading only. */
static bool console_allowed(const char *c)
{
    static const char *const at[] = {"ATI", "ATRV", "ATDP", "ATDPN", "AT@1", "ATSH", "ATCRA", "ATAR"};
    if (strncmp(c, "AT", 2) == 0)
    {
        for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++)
        {
            if (strncmp(c, at[i], strlen(at[i])) == 0)
            {
                return true;
            }
        }
        return false;
    }
    /* 01-03, 06, 07, 09, 0A: OBD reads. 19 and 22: UDS read DTC / data by id. */
    static const char *const modes[] = {"01", "02", "03", "06", "07", "09", "0A", "19", "22"};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
    {
        if (strncmp(c, modes[i], 2) == 0)
        {
            return true;
        }
    }
    return false;
}

esp_err_t mt_console(const char *command, char *out, size_t size)
{
    char clean[48];
    size_t n = 0;
    for (; *command != 0 && n < sizeof(clean) - 1; command++)
    {
        if (!isspace((unsigned char)*command))
        {
            clean[n++] = (char)toupper((unsigned char)*command);
        }
    }
    clean[n] = 0;
    if (!console_allowed(clean))
    {
        snprintf(out, size, "Comando non consentito: la console è in sola lettura.");
        return ESP_ERR_NOT_ALLOWED;
    }
    static SemaphoreHandle_t once;
    if (once == NULL)
    {
        once = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(once, portMAX_DELAY);
    snprintf(s_console_cmd, sizeof(s_console_cmd), "%s", clean);
    s_console_out = out;
    s_console_size = size;
    esp_err_t err = run_in_task(REQ_CONSOLE, 12000);
    if (err == ESP_ERR_TIMEOUT)
    {
        snprintf(out, size, "Nessuna risposta");
    }
    xSemaphoreGive(once);
    return err;
}

/* ------------------------------------------------------------ parsing */

/* All hex bytes of an answer, in order: frame indexes ("0:") and the CAN
 * length line ("014") are dropped. */
static int answer_bytes(const char *text, uint8_t *out, int max)
{
    int n = 0;
    const char *line = text;
    while (*line != 0 && n < max)
    {
        const char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        const char *p = line;
        const char *colon = memchr(line, ':', len);
        if (colon != NULL)
        {
            p = colon + 1;
        }
        else if (len == 3)
        {
            p = line + len;   /* "014": byte count of a multi-frame answer */
        }
        int nibble = -1;
        for (; p < line + len && n < max; p++)
        {
            int v = isdigit((unsigned char)*p) ? *p - '0' : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 : -1;
            if (v < 0)
            {
                if (!isspace((unsigned char)*p))
                {
                    nibble = -1;
                }
                continue;
            }
            if (nibble < 0)
            {
                nibble = v;
            }
            else
            {
                out[n++] = (uint8_t)(nibble << 4 | v);
                nibble = -1;
            }
        }
        if (end == NULL)
        {
            break;
        }
        line = end + 1;
    }
    return n;
}

static bool answer_failed(const char *text)
{
    return strstr(text, "NO DATA") != NULL || strstr(text, "UNABLE") != NULL || strstr(text, "ERROR") != NULL ||
           strstr(text, "STOPPED") != NULL || strstr(text, "?") != NULL || text[0] == 0;
}

/* Mode 01/02 answer data for a PID: pointer into bytes after "41 PID". */
static int pid_data(const char *text, uint8_t mode, uint8_t pid, uint8_t *out, int max)
{
    uint8_t b[64];
    int n = answer_bytes(text, b, sizeof(b));
    for (int i = 0; i + 1 < n; i++)
    {
        if (b[i] == (uint8_t)(mode + 0x40) && b[i + 1] == pid)
        {
            int skip = mode == 0x02 ? 3 : 2;   /* mode 02 repeats the frame number */
            int count = n - i - skip;
            count = count < max ? count : max;
            if (count > 0)
            {
                memcpy(out, b + i + skip, (size_t)count);
            }
            return count;
        }
    }
    return 0;
}

static bool decode(mt_value_t v, const uint8_t *d, int n, float *out)
{
    float A = n > 0 ? d[0] : 0, B = n > 1 ? d[1] : 0;
    if (n < 1)
    {
        return false;
    }
    switch (s_info[v].pid)
    {
    case 0x04: case 0x11: case 0x2F: case 0x45: *out = A * 100.0f / 255.0f; break;
    case 0x05: case 0x0F: case 0x46: case 0x5C: *out = A - 40; break;
    case 0x06: case 0x07: *out = (A - 128) * 100.0f / 128.0f; break;
    case 0x0B: case 0x0D: case 0x30: case 0x33: *out = A; break;
    case 0x0C: if (n < 2) return false; *out = (256 * A + B) / 4.0f; break;
    case 0x0E: *out = A / 2.0f - 64; break;
    case 0x10: if (n < 2) return false; *out = (256 * A + B) / 100.0f; break;
    case 0x14: *out = A / 200.0f; break;
    case 0x1F: case 0x21: case 0x31: if (n < 2) return false; *out = 256 * A + B; break;
    case 0x42: if (n < 2) return false; *out = (256 * A + B) / 1000.0f; break;
    case 0x5E: if (n < 2) return false; *out = (256 * A + B) / 20.0f; break;
    case 0xA6:
        if (n < 4) return false;
        *out = (float)(((uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]) / 10.0);
        break;
    default: return false;
    }
    return true;
}

static bool supported(uint8_t pid)
{
    return pid != 0 && (s_state.supported[(pid - 1) / 8] & (0x80 >> ((pid - 1) % 8)));
}

/* ------------------------------------------------------------ steps */

static esp_err_t cmd(const char *c, char *out, size_t size, int ms)
{
    esp_err_t err = mt_link_command(c, out, size, ms);
    ESP_LOGD(TAG, "%s -> %s", c, out);
    return err;
}

static esp_err_t adapter_setup(void)
{
    char a[128];
    cmd("ATZ", a, sizeof(a), 3000);   /* reset: answers with its name */
    vTaskDelay(pdMS_TO_TICKS(300));
    static const char *const setup[] = {"ATE0", "ATL0", "ATS0", "ATH0", "ATAT1", "ATSP0"};
    for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++)
    {
        esp_err_t err = cmd(setup[i], a, sizeof(a), 2000);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    cmd("ATI", a, sizeof(a), 2000);
    mt_lock();
    snprintf(s_state.adapter, sizeof(s_state.adapter), "%.39s", a);
    mt_unlock();
    return ESP_OK;
}

/* "0100" wakes the bus; then the chain of supported PID bitmaps. */
static bool car_setup(void)
{
    char a[256];
    if (cmd("0100", a, sizeof(a), 12000) != ESP_OK || answer_failed(a))
    {
        return false;
    }
    uint8_t bits[32] = {0};
    for (uint8_t base = 0x00; base <= 0xE0; base += 0x20)
    {
        char c[8];
        snprintf(c, sizeof(c), "01%02X", base);
        uint8_t d[4];
        if (base > 0 && cmd(c, a, sizeof(a), 3000) != ESP_OK)
        {
            break;
        }
        if (pid_data(a, 0x01, base, d, 4) < 4)
        {
            break;
        }
        memcpy(bits + base / 8, d, 4);
        if (!(d[3] & 0x01))
        {
            break;   /* the next range is not supported */
        }
    }
    cmd("ATDPN", a, sizeof(a), 2000);
    const char *num = a[0] == 'A' ? a + 1 : a;
    s_can = num[0] >= '6' && num[0] <= '9';
    char protocol[64];
    cmd("ATDP", protocol, sizeof(protocol), 2000);
    char vin[160];
    cmd("0902", vin, sizeof(vin), 5000);
    uint8_t vb[40];
    int vn = answer_bytes(vin, vb, sizeof(vb));
    char v[18] = "";
    for (int i = 0; i + 2 < vn; i++)
    {
        if (vb[i] == 0x49 && vb[i + 1] == 0x02)
        {
            int k = 0;
            for (int j = i + 3; j < vn && k < 17; j++)
            {
                if (vb[j] >= 0x30 && vb[j] < 0x7F)
                {
                    v[k++] = (char)vb[j];
                }
            }
            v[k] = 0;
            break;
        }
    }
    mt_lock();
    memcpy(s_state.supported, bits, sizeof(bits));
    snprintf(s_state.protocol, sizeof(s_state.protocol), "%.39s", protocol[0] == 'A' && protocol[1] == ',' ? protocol + 2 : protocol);
    snprintf(s_state.vin, sizeof(s_state.vin), "%s", v);
    s_state.revision++;
    mt_unlock();
    return true;
}

static void dtc_text_code(uint8_t a, uint8_t b, char out[6])
{
    static const char letters[] = "PCBU";
    snprintf(out, 6, "%c%d%X%02X", letters[a >> 6], (a >> 4) & 3, a & 15, b);
}

static int read_dtc_mode(const char *mode, uint8_t kind, mt_dtc_t *list, int count)
{
    char a[512];
    if (cmd(mode, a, sizeof(a), 5000) != ESP_OK || answer_failed(a))
    {
        return count;
    }
    uint8_t b[128];
    int n = answer_bytes(a, b, sizeof(b));
    uint8_t reply = (uint8_t)(strtol(mode, NULL, 16) + 0x40);
    for (int i = 0; i < n; i++)
    {
        if (b[i] != reply)
        {
            continue;
        }
        int j = i + 1, codes;
        if (s_can)
        {
            codes = j < n ? b[j++] : 0;   /* CAN: number of codes, then pairs */
        }
        else
        {
            codes = 3;                    /* older protocols: 3 per frame */
        }
        for (int c = 0; c < codes && j + 1 < n && count < MT_DTC_MAX; c++, j += 2)
        {
            if (b[j] == 0 && b[j + 1] == 0)
            {
                continue;
            }
            char code[6];
            dtc_text_code(b[j], b[j + 1], code);
            bool seen = false;
            for (int k = 0; k < count; k++)
            {
                seen |= strcmp(list[k].code, code) == 0;
            }
            if (!seen)
            {
                snprintf(list[count].code, sizeof(list[count].code), "%s", code);
                list[count].kind = kind;
                count++;
            }
        }
        if (s_can)
        {
            break;
        }
        i = j - 1;
    }
    return count;
}

static void read_codes(void)
{
    mt_dtc_t list[MT_DTC_MAX];
    int count = read_dtc_mode("03", MT_DTC_STORED, list, 0);
    count = read_dtc_mode("07", MT_DTC_PENDING, list, count);
    count = read_dtc_mode("0A", MT_DTC_PERMANENT, list, count);

    char a[128];
    uint8_t d[4];
    bool have_status = cmd("0101", a, sizeof(a), 3000) == ESP_OK && pid_data(a, 0x01, 0x01, d, 4) == 4;

    /* Freeze frame of the code that stored it. */
    float freeze[MT_VALUES];
    for (int v = 0; v < MT_VALUES; v++)
    {
        freeze[v] = NAN;
    }
    char freeze_code[6] = "";
    uint8_t f[4];
    if (cmd("020200", a, sizeof(a), 3000) == ESP_OK && pid_data(a, 0x02, 0x02, f, 2) == 2 && (f[0] | f[1]) != 0)
    {
        dtc_text_code(f[0], f[1], freeze_code);
        static const mt_value_t frame[] = {MT_RPM, MT_SPEED, MT_COOLANT, MT_LOAD, MT_STFT1, MT_LTFT1, MT_MAP, MT_INTAKE_T, MT_THROTTLE};
        for (size_t i = 0; i < sizeof(frame) / sizeof(frame[0]); i++)
        {
            char c[10];
            snprintf(c, sizeof(c), "02%02X00", s_info[frame[i]].pid);
            uint8_t fd[4];
            int n;
            if (supported(s_info[frame[i]].pid) && cmd(c, a, sizeof(a), 2000) == ESP_OK &&
                (n = pid_data(a, 0x02, s_info[frame[i]].pid, fd, 4)) > 0)
            {
                decode(frame[i], fd, n, &freeze[frame[i]]);
            }
        }
    }

    mt_lock();
    memcpy(s_state.dtc, list, sizeof(list));
    s_state.dtcs = (uint8_t)count;
    s_state.dtc_read = esp_timer_get_time();
    memcpy(s_state.freeze, freeze, sizeof(freeze));
    snprintf(s_state.freeze_code, sizeof(s_state.freeze_code), "%s", freeze_code);
    if (have_status)
    {
        s_state.mil = (d[0] & 0x80) != 0;
        s_state.dtc_count = d[0] & 0x7F;
        bool diesel = (d[1] & 0x08) != 0;
        s_state.compression = diesel;
        const char *const *names = diesel ? s_diesel : s_spark;
        /* B: bits 0-2 supported, 4-6 incomplete; C supported, D incomplete. */
        for (int i = 0; i < 3; i++)
        {
            s_state.monitor[i] = (mt_monitor_t){names[i], (d[1] >> i) & 1, !((d[1] >> (i + 4)) & 1)};
        }
        for (int i = 0; i < 8; i++)
        {
            s_state.monitor[3 + i] = (mt_monitor_t){names[3 + i], (d[2] >> i) & 1, !((d[3] >> i) & 1)};
        }
    }
    s_state.revision++;
    mt_unlock();
}

static void clear_codes(void)
{
    mt_lock();
    bool unlocked = s_state.unlocked && esp_timer_get_time() < s_unlocked_until;
    bool speed_known = mt_value_fresh(&s_state, MT_SPEED);
    float speed = s_state.value[MT_SPEED];
    bool rpm_known = mt_value_fresh(&s_state, MT_RPM);
    float rpm = s_state.value[MT_RPM];
    mt_unlock();
    char a[64];
    if (!unlocked)
    {
        s_result = ESP_ERR_NOT_ALLOWED;
        snprintf(s_why, sizeof(s_why), "Sblocca prima le azioni critiche con il codice.");
    }
    else if (!speed_known || speed > 0.5f)
    {
        s_result = ESP_ERR_INVALID_STATE;
        snprintf(s_why, sizeof(s_why), "L'auto deve essere ferma.");
    }
    else if (rpm_known && rpm > 50)
    {
        s_result = ESP_ERR_INVALID_STATE;
        snprintf(s_why, sizeof(s_why), "Spegni il motore e lascia il quadro acceso.");
    }
    else if (cmd("04", a, sizeof(a), 8000) != ESP_OK || strstr(a, "44") == NULL)
    {
        s_result = ESP_FAIL;
        snprintf(s_why, sizeof(s_why), "La centralina ha rifiutato la cancellazione (%.40s).", a);
    }
    else
    {
        s_result = ESP_OK;
        snprintf(s_why, sizeof(s_why), "Codici cancellati e spia spenta.");
        ESP_LOGW(TAG, "Trouble codes cleared");
        read_codes();
    }
    xSemaphoreGive(s_done);
}

static void console(void)
{
    s_result = cmd(s_console_cmd, s_console_out, s_console_size, 6000);
    xSemaphoreGive(s_done);
}

/* ------------------------------------------------------------ live */

static float fuel_flow_lph(void)
{
    float afr, density;
    fuel_constants(s_settings.fuel_type, &afr, &density);
    if (mt_value_fresh(&s_state, MT_FUEL_RATE) && s_settings.fuel_type != MT_FUEL_LPG)
    {
        return s_state.value[MT_FUEL_RATE];
    }
    float air_gs;
    if (mt_value_fresh(&s_state, MT_MAF))
    {
        air_gs = s_state.value[MT_MAF];
    }
    else if (mt_value_fresh(&s_state, MT_MAP) && mt_value_fresh(&s_state, MT_RPM) && mt_value_fresh(&s_state, MT_INTAKE_T))
    {
        /* Speed-density: air mass from manifold pressure (4-stroke). */
        float kelvin = s_state.value[MT_INTAKE_T] + 273.15f;
        float litres_per_s = s_state.value[MT_RPM] / 120.0f * (s_settings.displacement_cc / 1000.0f) * s_settings.volumetric;
        air_gs = litres_per_s * s_state.value[MT_MAP] * 28.97f / (8.314f * kelvin);
    }
    else
    {
        return NAN;
    }
    return air_gs / afr / density * 3.6f;   /* g/s air -> l/h fuel */
}

static void poll(mt_value_t v)
{
    char c[8], a[96];
    snprintf(c, sizeof(c), "01%02X", s_info[v].pid);
    uint8_t d[8];
    int n;
    float value;
    if (cmd(c, a, sizeof(a), 1500) == ESP_OK && (n = pid_data(a, 0x01, s_info[v].pid, d, sizeof(d))) > 0 && decode(v, d, n, &value))
    {
        mt_lock();
        s_state.value[v] = value;
        s_state.updated[v] = esp_timer_get_time();
        mt_unlock();
    }
}

static void battery(void)
{
    char a[32];
    if (cmd("ATRV", a, sizeof(a), 1500) == ESP_OK)
    {
        float volts = strtof(a, NULL);
        if (volts > 3 && volts < 30)
        {
            mt_lock();
            s_state.value[MT_BATTERY] = volts;
            s_state.updated[MT_BATTERY] = esp_timer_get_time();
            mt_unlock();
        }
    }
}

static void trip_step(float dt)
{
    mt_lock();
    mt_trip_t *t = &s_state.trip;
    bool running = mt_value_fresh(&s_state, MT_RPM) && s_state.value[MT_RPM] > 300;
    float speed = mt_value_fresh(&s_state, MT_SPEED) ? s_state.value[MT_SPEED] : 0;
    if (running)
    {
        t->seconds += (uint32_t)(dt + 0.5f);
        t->idle_seconds += speed < 1 ? (uint32_t)(dt + 0.5f) : 0;
        t->km += speed * dt / 3600.0f;
        float lph = fuel_flow_lph();
        if (!isnan(lph))
        {
            t->litres += lph * dt / 3600.0f;
            s_state.value[MT_FUEL_RATE] = lph;
            s_state.updated[MT_FUEL_RATE] = esp_timer_get_time();
        }
        t->max_speed = speed > t->max_speed ? speed : t->max_speed;
        t->max_rpm = s_state.value[MT_RPM] > t->max_rpm ? s_state.value[MT_RPM] : t->max_rpm;
    }
    mt_unlock();
}

/* ------------------------------------------------------------ task */

static bool handle_requests(void)
{
    int r = atomic_exchange(&s_requests, 0);
    if (r & REQ_TRIP)
    {
        mt_lock();
        memset(&s_state.trip, 0, sizeof(s_state.trip));
        s_state.revision++;
        mt_unlock();
    }
    if (r & REQ_CLEAR)
    {
        if (mt_link_up())
        {
            clear_codes();
        }
        else
        {
            s_result = ESP_ERR_INVALID_STATE;
            snprintf(s_why, sizeof(s_why), "Adattatore non collegato.");
            xSemaphoreGive(s_done);
        }
    }
    if (r & REQ_CONSOLE)
    {
        if (mt_link_up())
        {
            console();
        }
        else
        {
            s_result = ESP_ERR_INVALID_STATE;
            snprintf(s_console_out, s_console_size, "Adattatore non collegato.");
            xSemaphoreGive(s_done);
        }
    }
    if ((r & REQ_CODES) && mt_link_up())
    {
        read_codes();
    }
    if (r & REQ_SCAN)
    {
        mt_link_close();
        set_phase(MT_PHASE_SEARCHING, "Ricerca adattatori Bluetooth...");
        mt_link_scan();
        for (int i = 0; i < 90 && mt_link_scanning(); i++)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        set_phase(MT_PHASE_IDLE, "Scegli l'adattatore dall'elenco");
        return true;   /* do not reconnect to the old one by itself */
    }
    return (r & REQ_RECONNECT) != 0;
}

static void obd_task(void *arg)
{
    (void)arg;
    int backoff = 2;
    for (;;)
    {
        mt_settings_t s;
        mt_settings(&s);
        char why[96] = "";
        set_phase(MT_PHASE_CONNECTING, s.link == MT_LINK_WIFI ? "Collegamento all'adattatore Wi-Fi..." : "Collegamento all'adattatore Bluetooth...");
        esp_err_t opened = mt_link_open(&s, why, sizeof(why));
        if (opened == ESP_ERR_NOT_FOUND)
        {
            /* Nothing configured: wait for a choice, no retries. */
            set_phase(MT_PHASE_IDLE, why);
            xSemaphoreTake(s_wake, portMAX_DELAY);
            handle_requests();
            continue;
        }
        if (opened != ESP_OK)
        {
            set_phase(MT_PHASE_LOST, why[0] ? why : "Adattatore non raggiungibile");
            xSemaphoreTake(s_wake, pdMS_TO_TICKS(backoff * 1000));
            backoff = backoff < 30 ? backoff * 2 : 30;
            handle_requests();
            continue;
        }
        set_phase(MT_PHASE_SETUP, "Adattatore collegato, dialogo con l'auto...");
        if (adapter_setup() != ESP_OK)
        {
            set_phase(MT_PHASE_LOST, "L'adattatore non risponde ai comandi ELM327");
            mt_link_close();
            continue;
        }
        while (mt_link_up() && !car_setup())
        {
            set_phase(MT_PHASE_NO_CAR, "L'auto non risponde: accendi il quadro (chiave su ON)");
            if (xSemaphoreTake(s_wake, pdMS_TO_TICKS(4000)) == pdTRUE && handle_requests())
            {
                break;
            }
        }
        if (!mt_link_up())
        {
            continue;
        }
        backoff = 2;
        read_codes();
        set_phase(MT_PHASE_LIVE, "In lettura");

        mt_value_t fast[MT_VALUES], slow[MT_VALUES];
        int fasts = 0, slows = 0;
        for (int v = 0; v < MT_VALUES; v++)
        {
            if (v == MT_BATTERY || !supported(s_info[v].pid))
            {
                continue;
            }
            if (s_info[v].fast)
            {
                fast[fasts++] = (mt_value_t)v;
            }
            else
            {
                slow[slows++] = (mt_value_t)v;
            }
        }
        int next_slow = 0, polls = 0, misses = 0;
        int64_t last = esp_timer_get_time(), last_battery = 0, last_codes = last, second = last;
        bool reconnect = false;
        while (mt_link_up() && !reconnect)
        {
            int64_t before = s_state.updated[MT_RPM];
            /* Rpm before every other value: ~10 readings per second on CAN,
             * enough to follow a gear change on the shift lights. */
            bool has_rpm = supported(0x0C);
            for (int i = 0; i < fasts; i++)
            {
                if (has_rpm && fast[i] != MT_RPM)
                {
                    poll(MT_RPM);
                    polls++;
                }
                poll(fast[i]);
                polls++;
            }
            if (slows > 0)
            {
                if (has_rpm)
                {
                    poll(MT_RPM);
                    polls++;
                }
                poll(slow[next_slow]);
                next_slow = (next_slow + 1) % slows;
                polls++;
            }
            int64_t now = esp_timer_get_time();
            if (now - last_battery > BATTERY_EVERY_US)
            {
                battery();
                last_battery = now;
            }
            /* No rpm answers for a while: ignition off. */
            misses = s_state.updated[MT_RPM] == before && supported(0x0C) ? misses + 1 : 0;
            if (misses > 8)
            {
                set_phase(MT_PHASE_NO_CAR, "L'auto non risponde: quadro spento?");
                while (mt_link_up() && !car_setup())
                {
                    if (xSemaphoreTake(s_wake, pdMS_TO_TICKS(4000)) == pdTRUE && handle_requests())
                    {
                        reconnect = true;
                        break;
                    }
                }
                misses = 0;
                set_phase(MT_PHASE_LIVE, "In lettura");
            }
            trip_step((float)(now - last) / 1e6f);
            last = now;
            if (now - last_codes > CODES_EVERY_US)
            {
                read_codes();
                last_codes = now;
            }
            if (now - second >= 1000000)
            {
                mt_lock();
                s_state.polls_per_s = (uint32_t)polls;
                if (s_state.unlocked && now > s_unlocked_until)
                {
                    s_state.unlocked = false;   /* the lock comes back by itself */
                }
                s_state.revision++;
                mt_unlock();
                polls = 0;
                second = now;
            }
            if (atomic_load(&s_requests) != 0)
            {
                reconnect = handle_requests();
            }
        }
        mt_link_close();
        if (!reconnect)
        {
            set_phase(MT_PHASE_LOST, "Collegamento perso, riprovo...");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}

esp_err_t mt_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    if (s_lock == NULL || s_wake == NULL || s_done == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    settings_load();
    for (int v = 0; v < MT_VALUES; v++)
    {
        s_state.freeze[v] = NAN;
    }
    return xTaskCreatePinnedToCore(obd_task, "motore_obd", 6144, NULL, 5, NULL, tskNO_AFFINITY) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
