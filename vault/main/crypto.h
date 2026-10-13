#ifndef VAULT_CRYPTO_H
#define VAULT_CRYPTO_H

#include "format.h"
#include "esp_err.h"

/* Call before board_init(): the entropy source temporarily uses the ADC. */
esp_err_t vault_crypto_init(void);
bool vault_crypto_should_cancel(void);
esp_err_t vault_random(void *output, size_t size);
/* PBKDF2 on the SHA engine (default) or in software; QA benchmarks switch it. */
extern bool vault_kdf_hardware;
esp_err_t vault_derive(const char *password, const uint8_t salt[16], uint32_t iterations,
                       uint8_t key[32], void (*progress)(unsigned));
esp_err_t vault_encrypt(const uint8_t key[32], const vault_header_t *header, const uint8_t *plain,
                        uint8_t *file, size_t capacity);
esp_err_t vault_decrypt(const uint8_t key[32], const uint8_t *file, size_t size, uint8_t *plain,
                        size_t capacity, vault_header_t *header);
esp_err_t vault_generate(char *output, size_t length, bool symbols);
esp_err_t vault_totp(const char *base32, uint64_t seconds, unsigned digits, unsigned period,
                     unsigned hash_bits, char output[9]);

#endif
