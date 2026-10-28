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

static const char *TAG = "watch_os";

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint8_t sha256[32];
} pt_model_trailer_t;

#define PAGE 0x10000u
#define RUN_A_PAGES ((PT_MODEL_RUN_A_END - PT_MODEL_RUN_A) / PAGE)
#define RUN_B_PAGES ((PT_MODEL_RUN_B_END - PT_MODEL_RUN_B) / PAGE)

void pt_watch_os_boot(void)
{
    esp_err_t err = extapp_sdk_init();
    ESP_LOGI(TAG, "hosted by Watch OS: next reset returns to the launcher (%s)", esp_err_to_name(err));
}

bool pt_watch_os_model_map(const void **map, size_t *len)
{
    pt_model_trailer_t t;
    if (esp_flash_read(NULL, &t, PT_MODEL_TRAILER, sizeof t) != ESP_OK || t.magic != PT_MODEL_MAGIC) {
        ESP_LOGW(TAG, "model: no trailer at 0x%x - run tools/install_usb.py", (unsigned)PT_MODEL_TRAILER);
        return false;
    }
    size_t pages = (t.len + PAGE - 1) / PAGE;
    if (t.len == 0 || pages > RUN_A_PAGES + RUN_B_PAGES ||
        (pages > RUN_A_PAGES && t.len - RUN_A_PAGES * PAGE > PT_MODEL_TRAILER - PT_MODEL_RUN_B)) {
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
