/* PSA authenticated encryption; the library DRBG is seeded once with real
 * startup entropy before ADC/RF peripherals start. No secret is logged. */
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "crypto.h"
#include "mbedtls/private/sha256.h"
#include "bootloader_random.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/private/ctr_drbg.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "mbedtls/platform.h"
#include "sha/sha_core.h"
#include <stdio.h>
#include <string.h>

static mbedtls_ctr_drbg_context s_random;
static SemaphoreHandle_t s_rng_mutex;
static unsigned s_requests;
static bool s_seeded;
bool vault_kdf_hardware = true;

__attribute__((weak)) bool vault_crypto_should_cancel(void)
{
    return false;
}

typedef union
{
    size_t length;
    max_align_t alignment;
} crypto_allocation_t;

static void *secure_calloc(size_t count, size_t size)
{
    if (count && size > (SIZE_MAX - sizeof(crypto_allocation_t)) / count)
        return NULL;
    size_t length = count * size;
    uint32_t caps = MALLOC_CAP_8BIT | (length <= 256 ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM);
    crypto_allocation_t *header = heap_caps_calloc(1, sizeof(*header) + length, caps);
    if (header == NULL)
        return NULL;
    header->length = length;
    return header + 1;
}

static void secure_free(void *pointer)
{
    if (pointer == NULL)
        return;
    crypto_allocation_t *header = (crypto_allocation_t *)pointer - 1;
    vault_wipe(header, sizeof(*header) + header->length);
    heap_caps_free(header);
}

static int startup_entropy(void *unused, unsigned char *output, size_t length)
{
    (void)unused;
    esp_fill_random(output, length);
    return 0;
}

esp_err_t vault_crypto_init(void)
{
    if (s_seeded)
        return ESP_OK;
    if (mbedtls_platform_set_calloc_free(secure_calloc, secure_free) != 0)
        return ESP_FAIL;
    s_rng_mutex = xSemaphoreCreateMutex();
    if (s_rng_mutex == NULL)
        return ESP_ERR_NO_MEM;
    mbedtls_ctr_drbg_init(&s_random);
    static const unsigned char domain[] = "Watch OS Vault RNG v1";
    bootloader_random_enable();
    int result =
        mbedtls_ctr_drbg_seed(&s_random, startup_entropy, NULL, domain, sizeof(domain) - 1);
    bootloader_random_disable();
    if (result != 0 || psa_crypto_init() != PSA_SUCCESS)
    {
        mbedtls_ctr_drbg_free(&s_random);
        vSemaphoreDelete(s_rng_mutex);
        s_rng_mutex = NULL;
        return ESP_FAIL;
    }
    /* Never call the startup ADC callback again after peripherals start.
     * Fail closed after 16K requests; reopening the app obtains a fresh seed. */
    mbedtls_ctr_drbg_set_reseed_interval(&s_random, 1000000000);
    s_seeded = true;
    return ESP_OK;
}

esp_err_t vault_random(void *output, size_t size)
{
    if (!s_seeded || output == NULL || size > 256)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_rng_mutex, portMAX_DELAY);
    int result = -1;
    if (s_requests < 16384)
    {
        s_requests++;
        result = mbedtls_ctr_drbg_random(&s_random, output, size);
    }
    xSemaphoreGive(s_rng_mutex);
    if (result != 0)
    {
        vault_wipe(output, size);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static psa_status_t import_aes(const uint8_t key[32], psa_key_id_t *id)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 256);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_status_t result = psa_import_key(&attributes, key, 32, id);
    psa_reset_key_attributes(&attributes);
    return result;
}

/* RFC 2104 HMAC with cached ipad/opad states. SHA-256 itself is the
 * pinned Mbed TLS implementation; no custom cipher/hash implementation. */
static int cached_hmac(const mbedtls_sha256_context *inner, const mbedtls_sha256_context *outer,
                       const uint8_t *input, size_t length, uint8_t output[32])
{
    mbedtls_sha256_context context;
    uint8_t digest[32];
    mbedtls_sha256_init(&context);
    mbedtls_sha256_clone(&context, inner);
    int result = mbedtls_sha256_update(&context, input, length);
    if (result == 0)
        result = mbedtls_sha256_finish(&context, digest);
    mbedtls_sha256_clone(&context, outer);
    if (result == 0)
        result = mbedtls_sha256_update(&context, digest, 32);
    if (result == 0)
        result = mbedtls_sha256_finish(&context, output);
    mbedtls_sha256_free(&context);
    vault_wipe(digest, sizeof(digest));
    return result;
}

/* Hardware PBKDF2 loop. Every HMAC-SHA256 iteration on a 32-byte value is two
 * one-block compressions starting from the cached ipad/opad states; the S3 SHA
 * engine resumes from a written state, ~10x faster than software. The engine's
 * state words are the digest bytes (as in IDF's own SHA-256 driver). */
typedef union
{
    uint32_t words[16];
    uint8_t bytes[64];
} sha_block_t;

static void hw_compress(const uint8_t state[32], const sha_block_t *block, uint8_t out[32])
{
    uint32_t copy[8];
    memcpy(copy, state, 32);
    esp_sha_write_digest_state(SHA2_256, copy);
    esp_sha_block(SHA2_256, block->words, false);
    esp_sha_read_digest_state(SHA2_256, copy); /* waits for the engine */
    memcpy(out, copy, 32);
    vault_wipe(copy, sizeof(copy));
}

/* Single-block message tail: data, 0x80, zeros, total length in bits (one 64-byte key block before). */
static void tail_block(sha_block_t *block, const uint8_t *data, size_t length)
{
    memset(block, 0, sizeof(*block));
    memcpy(block->bytes, data, length);
    block->bytes[length] = 0x80;
    uint32_t bits = (uint32_t)(64 + length) * 8;
    block->bytes[62] = (uint8_t)(bits >> 8);
    block->bytes[63] = (uint8_t)bits;
}

static void hw_hmac(const uint8_t inner[32], const uint8_t outer[32], const uint8_t *data, size_t length,
                   uint8_t out[32])
{
    sha_block_t block;
    uint8_t digest[32];
    tail_block(&block, data, length);
    hw_compress(inner, &block, digest);
    tail_block(&block, digest, 32);
    hw_compress(outer, &block, out);
    vault_wipe(&block, sizeof(block));
    vault_wipe(digest, sizeof(digest));
}

/* ipad/opad states from the engine: first block with the standard initial values. */
static void hw_pad_state(const uint8_t normalized[64], uint8_t value, uint8_t out[32])
{
    sha_block_t block;
    uint32_t state[8];
    for (unsigned i = 0; i < 64; i++)
        block.bytes[i] = normalized[i] ^ value;
    esp_sha_block(SHA2_256, block.words, true);
    esp_sha_read_digest_state(SHA2_256, state);
    memcpy(out, state, 32);
    vault_wipe(&block, sizeof(block));
    vault_wipe(state, sizeof(state));
}

esp_err_t vault_derive(const char *password, const uint8_t salt[16], uint32_t iterations,
                       uint8_t key[32], void (*progress)(unsigned))
{
    if (password == NULL || salt == NULL || key == NULL || iterations == 0 ||
        iterations > VAULT_KDF_MAX_ITERATIONS || !vault_text_valid(password, 129, false))
        return ESP_ERR_INVALID_ARG;
    mbedtls_sha256_context inner, outer;
    mbedtls_sha256_init(&inner);
    mbedtls_sha256_init(&outer);
    uint8_t normalized[64] = {0}, pad[64], input[20], u[32], next[32];
    size_t password_length = strlen(password);
    int result = 0;
    if (password_length > 64)
        result = mbedtls_sha256((const uint8_t *)password, password_length, normalized, 0);
    else
        memcpy(normalized, password, password_length);
    for (unsigned i = 0; i < 64; i++)
        pad[i] = normalized[i] ^ 0x36;
    if (result == 0)
        result = mbedtls_sha256_starts(&inner, 0);
    if (result == 0)
        result = mbedtls_sha256_update(&inner, pad, 64);
    for (unsigned i = 0; i < 64; i++)
        pad[i] = normalized[i] ^ 0x5c;
    if (result == 0)
        result = mbedtls_sha256_starts(&outer, 0);
    if (result == 0)
        result = mbedtls_sha256_update(&outer, pad, 64);
    memcpy(input, salt, 16);
    input[16] = input[17] = input[18] = 0;
    input[19] = 1;
    if (result == 0)
        result = cached_hmac(&inner, &outer, input, 20, u);
    if (result == 0)
        memcpy(key, u, 32);
    int64_t started = esp_timer_get_time();
    UBaseType_t priority = uxTaskPriorityGet(NULL);
    if (priority == tskIDLE_PRIORITY)
        vTaskPrioritySet(NULL, 2);
    /* Engine check: the first iteration must match the software result exactly,
     * otherwise the whole derivation stays in software (same key either way). */
    uint8_t inner_state[32], outer_state[32];
    bool hardware = false;
    if (result == 0 && vault_kdf_hardware)
    {
        esp_sha_acquire_hardware();
        esp_sha_set_mode(SHA2_256);
        hw_pad_state(normalized, 0x36, inner_state);
        hw_pad_state(normalized, 0x5c, outer_state);
        hw_hmac(inner_state, outer_state, input, 20, next);
        hardware = vault_equal(next, u, 32);
        if (!hardware)
            esp_sha_release_hardware();
    }
    for (uint32_t i = 1; i < iterations && result == 0; i++)
    {
        if (hardware)
            hw_hmac(inner_state, outer_state, u, 32, next);
        else
            result = cached_hmac(&inner, &outer, u, 32, next);
        if (result != 0)
            break;
        memcpy(u, next, 32);
        for (unsigned j = 0; j < 32; j++)
            key[j] ^= u[j];
        if ((i & 4095) == 0)
        {
            /* Lend the engine to TLS and other users while we report progress. */
            if (hardware)
                esp_sha_release_hardware();
            if (vault_crypto_should_cancel())
            {
                result = -1;
                hardware = false;
                break;
            }
            if (progress != NULL)
                progress((unsigned)((uint64_t)i * 100 / iterations));
            vTaskDelay(1);
            if (hardware)
            {
                esp_sha_acquire_hardware();
                esp_sha_set_mode(SHA2_256);
            }
        }
    }
    if (hardware)
        esp_sha_release_hardware();
#if VAULT_QA
    printf("@VAULTKDF %s %u iterations %lld ms\n", hardware ? "hardware" : "software", (unsigned)iterations,
           (long long)((esp_timer_get_time() - started) / 1000));
#endif
    vault_wipe(inner_state, sizeof(inner_state));
    vault_wipe(outer_state, sizeof(outer_state));
    if (priority == tskIDLE_PRIORITY)
        vTaskPrioritySet(NULL, priority);
    mbedtls_sha256_free(&inner);
    mbedtls_sha256_free(&outer);
    vault_wipe(normalized, sizeof(normalized));
    vault_wipe(pad, sizeof(pad));
    vault_wipe(input, sizeof(input));
    vault_wipe(u, sizeof(u));
    vault_wipe(next, sizeof(next));
    if (result != 0)
    {
        vault_wipe(key, 32);
        return ESP_FAIL;
    }
    if (progress != NULL)
        progress(100);
    return ESP_OK;
}

esp_err_t vault_encrypt(const uint8_t key[32], const vault_header_t *header, const uint8_t *plain,
                        uint8_t *file, size_t capacity)
{
    if (key == NULL || plain == NULL || file == NULL || header == NULL ||
        capacity < VAULT_HEADER_SIZE + VAULT_TAG_SIZE + (size_t)header->payload_size ||
        !vault_header_encode(file, header))
        return ESP_ERR_INVALID_ARG;
    psa_key_id_t id = 0;
    psa_status_t status = import_aes(key, &id);
    size_t length = 0;
    if (status == PSA_SUCCESS)
        status = psa_aead_encrypt(id, PSA_ALG_GCM, header->nonce, 12, file, VAULT_HEADER_SIZE,
                                  plain, header->payload_size, file + VAULT_HEADER_SIZE,
                                  capacity - VAULT_HEADER_SIZE, &length);
    if (id != 0)
        psa_destroy_key(id);
    if (status != PSA_SUCCESS || length != header->payload_size + VAULT_TAG_SIZE)
    {
        vault_wipe(file, capacity);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t vault_decrypt(const uint8_t key[32], const uint8_t *file, size_t size, uint8_t *plain,
                        size_t capacity, vault_header_t *header)
{
    if (key == NULL || plain == NULL || header == NULL ||
        !vault_header_decode(file, size, header) || capacity < header->payload_size)
    {
        if (plain != NULL && capacity <= VAULT_MAX_PAYLOAD)
            vault_wipe(plain, capacity);
        return ESP_ERR_INVALID_ARG;
    }
    psa_key_id_t id = 0;
    psa_status_t status = import_aes(key, &id);
    size_t length = 0;
    if (status == PSA_SUCCESS)
        status = psa_aead_decrypt(id, PSA_ALG_GCM, header->nonce, 12, file, VAULT_HEADER_SIZE,
                                  file + VAULT_HEADER_SIZE, size - VAULT_HEADER_SIZE, plain,
                                  capacity, &length);
    if (id != 0)
        psa_destroy_key(id);
    if (status != PSA_SUCCESS || length != header->payload_size)
    {
        vault_wipe(plain, capacity);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t vault_generate(char *output, size_t length, bool symbols)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!"
                                   "#$%&()*+,-./:;<=>?@[]^_{|}~";
    size_t count = symbols ? sizeof(alphabet) - 1 : 62;
    if (output == NULL || length < 12 || length > 128)
        return ESP_ERR_INVALID_ARG;
    /* Rejection of complete candidates makes all accepted strings equiprobable. */
    for (unsigned attempt = 0; attempt < 128; attempt++)
    {
        unsigned classes = 0;
        uint8_t pool[256];
        if (vault_random(pool, sizeof(pool)) != ESP_OK)
        {
            vault_wipe(output, length + 1);
            return ESP_FAIL;
        }
        size_t accepted = 0;
        for (size_t i = 0; i < sizeof(pool) && accepted < length; i++)
        {
            if (pool[i] >= 256 - (256 % count))
                continue;
            unsigned index = pool[i] % count;
            output[accepted++] = alphabet[index];
            classes |= index < 26 ? 1 : index < 52 ? 2 : index < 62 ? 4 : 8;
        }
        vault_wipe(pool, sizeof(pool));
        if (accepted == length && classes == (symbols ? 15U : 7U))
        {
            output[length] = 0;
            return ESP_OK;
        }
    }
    vault_wipe(output, length + 1);
    return ESP_FAIL;
}

esp_err_t vault_totp(const char *base32, uint64_t seconds, unsigned digits, unsigned period,
                     unsigned hash_bits, char output[9])
{
    if (output == NULL)
        return ESP_ERR_INVALID_ARG;
    output[0] = 0;
    if ((digits != 6 && digits != 8) || (period != 30 && period != 60) ||
        (hash_bits != 1 && hash_bits != 256 && hash_bits != 512))
        return ESP_ERR_INVALID_ARG;
    uint8_t secret[64], message[8], mac[64];
    size_t secret_size = 0, mac_size = 0;
    if (!vault_base32_decode(base32, secret, sizeof(secret), &secret_size))
        return ESP_ERR_INVALID_ARG;
    uint64_t counter = seconds / period;
    for (unsigned i = 0; i < 8; i++)
        message[7 - i] = (uint8_t)(counter >> (i * 8));
    psa_algorithm_t algorithm = PSA_ALG_HMAC(hash_bits == 1     ? PSA_ALG_SHA_1
                                             : hash_bits == 256 ? PSA_ALG_SHA_256
                                                                : PSA_ALG_SHA_512);
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_algorithm(&attributes, algorithm);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_key_id_t id = 0;
    psa_status_t status = psa_import_key(&attributes, secret, secret_size, &id);
    psa_reset_key_attributes(&attributes);
    if (status == PSA_SUCCESS)
        status = psa_mac_compute(id, algorithm, message, 8, mac, sizeof(mac), &mac_size);
    if (id != 0)
        psa_destroy_key(id);
    if (status == PSA_SUCCESS && mac_size >= 20)
    {
        unsigned offset = mac[mac_size - 1] & 15;
        uint32_t value = (uint32_t)(mac[offset] & 127) << 24 | (uint32_t)mac[offset + 1] << 16 |
                         (uint32_t)mac[offset + 2] << 8 | mac[offset + 3];
        snprintf(output, 9, digits == 6 ? "%06lu" : "%08lu",
                 (unsigned long)(value % (digits == 6 ? 1000000U : 100000000U)));
    }
    vault_wipe(secret, sizeof(secret));
    vault_wipe(message, sizeof(message));
    vault_wipe(mac, sizeof(mac));
    return status == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
}
