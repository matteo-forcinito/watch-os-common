/* Standalone password vault. The main OS never links this firmware. */
#include "vault.h"
#include "core/app.h"
#include "core/clock.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/sys.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "services/storage.h"
#include "services/wifi.h"
#include "ui/ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include <string.h>

#if VAULT_QA
/* Synthetic fixture in a random, build-specific folder. Never read /vault. */
static bool qa_wait(void)
{
    for (unsigned i = 0; i < 1200; i++)
    {
        vault_status_t status;
        if (vault_status(&status) && !status.busy)
            return status.last_error == ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

static bool qa_submit(vault_job_t *job)
{
    if (job == NULL)
        return false;
    vault_operation_t operation = job->operation;
    esp_err_t submitted = vault_submit(job);
    if (submitted != ESP_OK)
    {
        vault_job_free(job);
        printf("@VAULTQA op=%d submit=%s\n", operation, esp_err_to_name(submitted));
        return false;
    }
    bool ok = qa_wait();
    vault_status_t status;
    vault_status(&status);
    printf("@VAULTQA op=%d result=%s count=%u\n", operation, esp_err_to_name(status.last_error), status.count);
    return ok;
}

static vault_job_t *qa_job(vault_operation_t operation)
{
    vault_job_t *job = vault_secure_alloc(sizeof(*job));
    if (job != NULL)
    {
        memset(job, 0, sizeof(*job));
        job->operation = operation;
    }
    return job;
}

static void qa_benchmark(void)
{
    static const uint8_t salt[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t key[32];
    power_cpu_boost(true);
    for (int hardware = 1; hardware >= 0; hardware--)
    {
        vault_kdf_hardware = hardware;
        int64_t start = esp_timer_get_time();
        vault_derive("benchmark only", salt, 20000, key, NULL);
        printf("@VAULTBENCH %s 20000 iterations %lld ms, CPU %lu MHz, core %d\n", hardware ? "hardware" : "software",
               (long long)((esp_timer_get_time() - start) / 1000), (unsigned long)(esp_clk_cpu_freq() / 1000000),
               (int)xPortGetCoreID());
    }
    vault_kdf_hardware = true;
    power_cpu_boost(false);
    vault_wipe(key, sizeof(key));
}

static void qa_bootstrap(void *unused)
{
    (void)unused;
    qa_benchmark();
    vault_status_t status;
    if (!vault_status(&status) || !status.storage)
        goto done;
    vault_job_t *job = qa_job(status.exists ? VAULT_UNLOCK : VAULT_CREATE);
    if (job == NULL)
        goto done;
    snprintf(job->password, sizeof(job->password), "Vault QA only passphrase");
    if (!qa_submit(job))
        goto done;
    for (unsigned i = 0; i < 2; i++)
    {
        if (!vault_status(&status))
        {
            printf("@VAULTQA status unavailable\n");
            goto done;
        }
        printf("@VAULTQA entry %u: unlocked=%d count=%u\n", i, status.unlocked, status.count);
        if (status.count > i)
            continue;
        job = qa_job(VAULT_EDIT);
        if (job == NULL)
            goto done;
        job->index = status.count;
        job->expected_generation = status.generation;
        snprintf(job->entry.id, sizeof(job->entry.id), "%032u", i + 1);
        snprintf(job->entry.name, sizeof(job->entry.name), "%s",
                 i ? "Solo dati sintetici" : "Account di prova");
        snprintf(job->entry.email, sizeof(job->entry.email), "%s",
                 i ? "beta@vaultqa.invalid" : "alice@vaultqa.invalid");
        snprintf(job->entry.username, sizeof(job->entry.username), "%s", i ? "beta" : "alice");
        snprintf(job->entry.website, sizeof(job->entry.website), "https://vaultqa.invalid");
        snprintf(job->entry.password, sizeof(job->entry.password), "%s",
                 i ? "VaultQA_Test#84" : "VaultQA_Only#42");
        snprintf(job->entry.otp, sizeof(job->entry.otp), "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ");
        job->entry.otp_digits = 6;
        job->entry.otp_period = 30;
        job->entry.otp_hash = 1;
        if (!qa_submit(job))
            goto done;
    }
    job = qa_job(VAULT_WEB);
    if (job != NULL)
    {
        job->enabled = true;
        qa_submit(job);
    }
done:
    printf("@VAULTQA bootstrap complete\n");
    fflush(stdout);
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
    ESP_ERROR_CHECK(extapp_sdk_init());
    /* Seed the DRBG before board ADC/RF initialization. */
    ESP_ERROR_CHECK(vault_crypto_init());
    sys_worker_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    esp_err_t storage = storage_init();
    if (storage != ESP_OK && storage != ESP_ERR_NOT_FOUND)
        ESP_LOGW("vault", "Storage unavailable");
    ESP_ERROR_CHECK(lv_port_init());
    clock_init();
    vault_alarm_prepare();
    ESP_ERROR_CHECK(wifi_service_init());
    ESP_ERROR_CHECK(vault_hid_init());
    ESP_ERROR_CHECK(vault_service_init());
    lv_port_lock();
    ui_init();
    vault_input_init();
    lv_obj_t *home = lv_obj_create(NULL);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(home, ui_color(0x090D15), 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
    vault_ui_create(home);
    app_manager_set_home(home, vault_ui_back);
    lv_port_unlock();
    ESP_ERROR_CHECK(power_init());
    app_manager_init();
    vault_alarm_start();
    /* Never start devcon: it supports screenshots and arbitrary commands. */
    sys_heap_log("Vault ready");
#if VAULT_QA
    if (xTaskCreate(qa_bootstrap, "vault_qa", 4096, NULL, 1, NULL) != pdPASS)
        ESP_LOGE("vault", "QA task unavailable");
#endif
}
