/* model_trailer.h - the model partition's validity marker (docs/OTA.md,
 * "Model updates", 2026-09-30).
 *
 * A 7.5 MB model cannot be double-buffered in 16 MB, so an over-the-air
 * model write is in place. The LAST sector of the model partition (the body
 * stays at offset 0, nothing moves) holds this trailer: written LAST by a
 * model write, erased FIRST, so a half-written model is never mistaken for
 * a good one. The cable installer writes it as a 4 KB part
 * (tools/make_installer.py, model_trailer.bin). A tank that has a model
 * but no trailer (a cable install that skipped it, QEMU) hashes the body
 * once at boot: the hash the build shipped with (PT_MODEL_SHA256) matches ->
 * the trailer is written; anything else runs as it is (a keeper's own model)
 * and says so in the log. Phase two (the model download) adds the "dirty"
 * flag: a write that began and never finished. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_partition.h"

#define MODEL_TRAILER_MAGIC 0x54524d50u        /* "PMRT" */
#define MODEL_TRAILER_SIZE  0x1000
typedef struct {
    uint32_t magic;
    uint32_t version;                          /* the trailer's layout: 1 */
    uint32_t model_len;                        /* bytes of body */
    uint8_t  sha256[32];                       /* of the body */
    char     tag[16];                          /* the model's name ("v3m") */
    uint32_t crc;                              /* FNV-1a over everything above */
} model_trailer_t;

bool model_trailer_read(const esp_partition_t *mp, model_trailer_t *out);   /* true = present and consistent */
bool model_trailer_write(const esp_partition_t *mp, model_trailer_t *t);    /* seals (crc) and writes the last sector */
bool model_trailer_erase(const esp_partition_t *mp);
/* at boot, with the partition mmap'd: the trailer's verdict, healing a
 * missing trailer when the body is the model this build shipped with.
 * Returns true when the model is known good (a trailer stands, or was just
 * written); false = no trailer and not the shipped model (still used). */
bool model_trailer_check(const esp_partition_t *mp, const void *map);
/* after the check: no trailer, and the body matches no model we ever shipped
 * (notice.h NOTICE_MODEL_DAMAGED). A tank with a good trailer is never hashed. */
bool model_trailer_unknown_body(void);
/* sha256 of len bytes at p, hex into out[65] (a flash-mapped model takes ~1 s) */
void model_sha256_hex(const void *p, size_t len, char out[65]);
