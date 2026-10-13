#ifndef VAULT_FORMAT_H
#define VAULT_FORMAT_H

/* The binary header is explicitly encoded: no packed structs or native ABI. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VAULT_HEADER_SIZE 64U
#define VAULT_TAG_SIZE 16U
#define VAULT_MAX_PAYLOAD (1024U * 1024U)
#define VAULT_KDF_ITERATIONS 600000U
#define VAULT_KDF_MAX_ITERATIONS 2000000U

typedef struct
{
    uint32_t iterations;
    uint64_t generation;
    uint32_t payload_size;
    uint8_t salt[16];
    uint8_t nonce[12];
} vault_header_t;

bool vault_header_decode(const uint8_t *file, size_t size, vault_header_t *header);
bool vault_header_encode(uint8_t output[VAULT_HEADER_SIZE], const vault_header_t *header);
void vault_wipe(void *memory, size_t length);
bool vault_equal(const void *left, const void *right, size_t length);
bool vault_base32_decode(const char *text, uint8_t *output, size_t capacity, size_t *length);
bool vault_text_valid(const char *text, size_t capacity, bool empty_allowed);

#endif
