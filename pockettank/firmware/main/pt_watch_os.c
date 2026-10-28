/* pt_watch_os.c - see pt_watch_os.h */
#include "pt_watch_os.h"
#include <string.h>
#include "extapp_sdk.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "spi_flash_mmap.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_time.h"
#include "update.h"
#include "render.h"
#include "tank.h"
#include "display_port.h"
#include "touch_port.h"

static const char *TAG = "watch_os";

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint8_t sha256[32];
} pt_model_trailer_t;

#define PAGE 0x10000u
#define RUN_A_PAGES ((PT_MODEL_RUN_A_END - PT_MODEL_RUN_A) / PAGE)
#define RUN_B_PAGES ((PT_MODEL_RUN_B_END - PT_MODEL_RUN_B) / PAGE)
#define LAST_RUN    (RUN_B_PAGES ? PT_MODEL_RUN_B : PT_MODEL_RUN_A)
/* bytes of model the runs hold before the trailer sector */
#define CAPACITY    ((size_t)RUN_A_PAGES * PAGE + RUN_B_PAGES * PAGE - 0x1000u)

/* The runs are raw flash: refuse them if this board's table claims any of it. */
static bool runs_free(void)
{
    bool free_ = true;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        /* This app's own slot: its tail past the image is run A by design
         * (the image size is checked at publish and install). */
        if (p == esp_ota_get_running_partition()) continue;
        uint32_t end = p->address + p->size;
        bool over_a = p->address < PT_MODEL_RUN_A_END && end > PT_MODEL_RUN_A;
        bool over_b = RUN_B_PAGES && p->address < PT_MODEL_RUN_B_END && end > PT_MODEL_RUN_B;
        if (over_a || over_b) {
            ESP_LOGE(TAG, "model: partition %s (0x%lx) lies in the model runs - no LLM on this layout", p->label, (unsigned long)p->address);
            free_ = false;
        }
    }
    esp_partition_iterator_release(it);
    return free_;
}

void pt_watch_os_boot(void)
{
    esp_err_t err = extapp_sdk_init();
    ESP_LOGI(TAG, "hosted by Watch OS: next reset returns to the launcher (%s)", esp_err_to_name(err));
}

bool pt_watch_os_model_map(const void **map, size_t *len)
{
    pt_model_trailer_t t;
    if (!runs_free()) return false;
    if (esp_flash_read(NULL, &t, PT_MODEL_TRAILER, sizeof t) != ESP_OK || t.magic != PT_MODEL_MAGIC) {
        ESP_LOGW(TAG, "model: no trailer at 0x%x - run tools/install_usb.py", (unsigned)PT_MODEL_TRAILER);
        return false;
    }
    size_t pages = (t.len + PAGE - 1) / PAGE;
    if (t.len == 0 || t.len > CAPACITY) {
        ESP_LOGE(TAG, "model: bad length %lu", (unsigned long)t.len);
        return false;
    }
    int page_list[RUN_A_PAGES + RUN_B_PAGES];
    for (size_t i = 0; i < pages; i++) {
        page_list[i] = i < RUN_A_PAGES ? (int)(PT_MODEL_RUN_A / PAGE + i)
                                       : (int)(PT_MODEL_RUN_B / PAGE + (i - RUN_A_PAGES));
    }
    const void *ptr = NULL;
    spi_flash_mmap_handle_t handle;
    if (spi_flash_mmap_pages(page_list, pages, SPI_FLASH_MMAP_DATA, &ptr, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "model: mmap of %u pages failed", (unsigned)pages);
        return false;
    }
    /* Another external app or an OS update written into ota_1 can overrun
     * run A: the hash, not the trailer, says the body is intact. */
    int64_t t0 = esp_timer_get_time();
    uint8_t sum[32];
    size_t sum_len = 0;
    psa_crypto_init();
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_hash_setup(&op, PSA_ALG_SHA_256);
    for (size_t off = 0; off < t.len; off += PAGE) {
        size_t n = t.len - off < PAGE ? t.len - off : PAGE;
        psa_hash_update(&op, (const uint8_t *)ptr + off, n);
        if ((off / PAGE) % 16 == 15) vTaskDelay(1);
    }
    if (psa_hash_finish(&op, sum, sizeof sum, &sum_len) != PSA_SUCCESS || memcmp(sum, t.sha256, sizeof sum) != 0) {
        ESP_LOGE(TAG, "model: SHA-256 mismatch (overwritten?) - reinstall with tools/install_usb.py");
        spi_flash_munmap(handle);
        return false;
    }
    ESP_LOGI(TAG, "model: %lu bytes in %u pages, verified in %lld ms", (unsigned long)t.len, (unsigned)pages,
             (long long)((esp_timer_get_time() - t0) / 1000));
    *map = ptr;
    *len = t.len;
    return true;
}

typedef struct {
    uint16_t *fb;
    size_t off;
    size_t erased_a, erased_b;   /* bytes of each run already erased */
    int shown;
    bool skipped;
    psa_hash_operation_t hash;
} install_t;

#define C_BG   0x041018
#define C_TEXT 0xcfeef2
#define C_DIM  0x5f8a92
#define C_BAR  0x46c8d8

static void install_page(install_t *in, int pct, const char *line)
{
    if (!in->fb) return;
    memset(in->fb, 0, (size_t)TANK_W * TANK_H * sizeof *in->fb);
    int w = TANK_W * 2 / 3, x = (TANK_W - w) / 2, y = TANK_H / 2;
    update_text(in->fb, TANK_W, x, y - 60, 3, C_TEXT, "Pocket Tank");
    update_text(in->fb, TANK_W, x, y - 24, 2, C_DIM, line);
    render_rect_edge(in->fb, TANK_W, x, y + 4, w, 14, C_DIM);
    if (pct > 0) render_rect(in->fb, TANK_W, x + 2, y + 6, (w - 4) * pct / 100, 10, C_BAR);
    update_text(in->fb, TANK_W, x, y + 34, 2, C_DIM, "tocca per saltare");
    display_port_flush(in->fb);
}

/* Erase lazily, then write: [run, run + n) of one run, never past its end. */
static bool run_write(uint32_t run, uint32_t run_end, size_t *erased, size_t at, const uint8_t *p, size_t n)
{
    if (run + at + n > run_end) return false;
    while (*erased < at + n) {
        if (esp_flash_erase_region(NULL, run + *erased, PAGE) != ESP_OK) return false;
        *erased += PAGE;
    }
    return esp_flash_write(NULL, p, run + at, n) == ESP_OK;
}

static bool install_sink(const uint8_t *p, size_t n, size_t total, void *arg)
{
    install_t *in = arg;
    float x, y;
    if (touch_port_read_raw(&x, &y)) { in->skipped = true; return false; }
    if (total != PT_MODEL_LEN || in->off + n > PT_MODEL_LEN) return false;
    psa_hash_update(&in->hash, p, n);
    const size_t a_size = PT_MODEL_RUN_A_END - PT_MODEL_RUN_A;
    while (n) {
        bool in_a = in->off < a_size;
        size_t room = in_a ? a_size - in->off : n;
        size_t k = n < room ? n : room;
        bool ok = in_a ? run_write(PT_MODEL_RUN_A, PT_MODEL_RUN_A_END, &in->erased_a, in->off, p, k)
                       : run_write(PT_MODEL_RUN_B, PT_MODEL_RUN_B_END, &in->erased_b, in->off - a_size, p, k);
        if (!ok) { ESP_LOGE(TAG, "model: flash write failed at %u", (unsigned)in->off); return false; }
        in->off += k; p += k; n -= k;
    }
    int pct = (int)(in->off * 100 / PT_MODEL_LEN);
    if (pct != in->shown) {
        in->shown = pct;
        char line[32];
        snprintf(line, sizeof line, "Scarico il cervello %d%%", pct);
        install_page(in, pct, line);
    }
    return true;
}

bool pt_watch_os_model_install(uint16_t *fb)
{
    if (!runs_free() || PT_MODEL_LEN > CAPACITY) return false;
    install_t in = { .fb = fb, .shown = -1, .hash = PSA_HASH_OPERATION_INIT };
    char ssid[NET_SSID_MAX + 1], pass[NET_PASS_MAX + 1], line[48];
    if (!net_port_creds_get(ssid, pass)) {
        ESP_LOGW(TAG, "model install: no Wi-Fi saved in Watch OS");
        return false;
    }
    snprintf(line, sizeof line, "Wi-Fi %.24s...", ssid);
    ESP_LOGI(TAG, "model install: connecting to %s", ssid);
    install_page(&in, 0, line);
    /* No trailer while the body is half written. */
    if (esp_flash_erase_region(NULL, PT_MODEL_TRAILER, 0x1000) != ESP_OK) return false;
    psa_crypto_init();
    psa_hash_setup(&in.hash, PSA_ALG_SHA_256);
    bool ok = net_port_download(PT_MODEL_URL, install_sink, &in) && in.off == PT_MODEL_LEN;
    uint8_t sum[32];
    size_t sum_len = 0;
    if (psa_hash_finish(&in.hash, sum, sizeof sum, &sum_len) != PSA_SUCCESS) ok = false;
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", sum[i]);
    if (ok && strcmp(hex, PT_MODEL_SHA256) != 0) {
        ESP_LOGE(TAG, "model: downloaded body has sha256 %.16s..., expected %.16s...", hex, PT_MODEL_SHA256);
        ok = false;
    }
    if (ok) {
        pt_model_trailer_t t = { .magic = PT_MODEL_MAGIC, .len = PT_MODEL_LEN };
        memcpy(t.sha256, sum, sizeof sum);
        ok = esp_flash_write(NULL, &t, PT_MODEL_TRAILER, sizeof t) == ESP_OK;
    }
    ESP_LOGI(TAG, "model install: %s", ok ? "done" : in.skipped ? "skipped by a touch" : "failed");
    if (!ok && !in.skipped) {
        install_page(&in, 0, "Niente Wi-Fi: pesci a regole");
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    if (fb) { memset(fb, 0, (size_t)TANK_W * TANK_H * sizeof *fb); display_port_flush(fb); }
    return ok;
}

void pt_watch_os_before_deep_sleep(void)
{
    /* A deep-sleep wake is a reset: boot back into the tank, not the launcher. */
    const esp_partition_t *self = esp_ota_get_running_partition();
    if (self != NULL) esp_ota_set_boot_partition(self);
}

void pt_watch_os_exit(void)
{
    ESP_LOGI(TAG, "BOOT: back to the launcher");
    vTaskDelay(pdMS_TO_TICKS(50));
    extapp_return_to_launcher();
}
