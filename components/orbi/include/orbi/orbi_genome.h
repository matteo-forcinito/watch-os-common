#ifndef ORBI_GENOME_H
#define ORBI_GENOME_H

/*
 * Orbi genome: a child is a 64-bit seed; this rebuilds the creature exactly
 * like the companion does (docs/ORBI-GENOME.md, reference tools/orbi_genome.py,
 * vectors checked by tools/orbi_vectors.json).
 */

#include <stdbool.h>
#include <stdint.h>

/* Capabilities: the high byte of the seed. */
enum
{
    ORBI_CAP_ROUND = 1 << 0,
    ORBI_CAP_SQUARE = 1 << 1,
    ORBI_CAP_MIC = 1 << 2,
    ORBI_CAP_SPEAKER = 1 << 3,
    ORBI_CAP_IMU = 1 << 4,
    ORBI_CAP_KNOB = 1 << 5,
    ORBI_CAP_CAMERA = 1 << 6,
};

typedef enum { ORBI_BODY_DROP, ORBI_BODY_CUBE, ORBI_BODY_CAPSULE } orbi_body_t;
typedef enum { ORBI_EAR_NONE, ORBI_EAR_ROUND, ORBI_EAR_POINTY, ORBI_EAR_NUBS } orbi_ear_t;
typedef enum { ORBI_CREST_NONE, ORBI_CREST_SPROUT, ORBI_CREST_HORN, ORBI_CREST_ANTENNA, ORBI_CREST_NOTE } orbi_crest_t;
typedef enum { ORBI_EYES_ROUND, ORBI_EYES_OVAL, ORBI_EYES_WIDE, ORBI_EYES_SLEEPY, ORBI_EYES_SPARKLE, ORBI_EYES_CYCLOPS } orbi_eyes_t;
typedef enum { ORBI_PATTERN_NONE, ORBI_PATTERN_SPOTS, ORBI_PATTERN_STRIPES, ORBI_PATTERN_BELLY, ORBI_PATTERN_FRECKLES } orbi_pattern_t;

typedef struct
{
    uint64_t seed;
    uint8_t caps;
    uint16_t hue, hue2;          /* degrees */
    uint8_t body, stretch;       /* stretch: height/width % */
    uint8_t ear, crest, eyes, eye_gap, eye_size, pattern;
    bool cheeks;
    uint8_t curiosity, energy, sociability, appetite, playfulness;
    char name[16];
} orbi_genome_t;

uint32_t orbi_fnv1a32(const uint8_t *data, int len);
uint64_t orbi_seed(uint8_t caps, const uint8_t mac[6], const char *model, uint8_t index, uint32_t birth);
void orbi_genome(uint64_t seed, orbi_genome_t *out);

/* 16 lowercase hex digits, and back (false if not a code). */
void orbi_code(uint64_t seed, char out[17]);
bool orbi_parse(const char *code, uint64_t *seed);

#endif
