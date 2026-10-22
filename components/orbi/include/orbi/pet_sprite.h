#ifndef PET_SPRITE_H
#define PET_SPRITE_H

/*
 * Pixel-art animals for the Orbi children (docs/ORBI-PETS.md).
 *
 * Pure C, no LVGL: the seed gives species, breed and colours; the level gives
 * the growth; a pose and a frame give a 32x32 grid of palette indices that the
 * caller paints. Everything is deterministic: the same seed, level, pose and
 * frame draw the same pixels on every device (and in tools/).
 */

#include <stdbool.h>
#include <stdint.h>

#define PET_GRID 32

typedef enum
{
    PET_DOG,
    PET_CAT,
    PET_FISH,
    PET_CHAMELEON,
    PET_GECKO,
    PET_HEN,
    PET_GOAT,
    PET_BUNNY,
    PET_SPECIES
} pet_species_t;

typedef enum { PET_MARK_NONE, PET_MARK_SPOTS, PET_MARK_STRIPES, PET_MARK_SOCKS, PET_MARK_PATCH, PET_MARKS } pet_mark_t;

typedef enum { PET_POSE_IDLE, PET_POSE_WALK, PET_POSE_SLEEP, PET_POSE_EAT, PET_POSE_HAPPY, PET_POSE_SAD } pet_pose_t;

/* Palette indices in the grid. */
enum
{
    PET_C_CLEAR,
    PET_C_OUTLINE,
    PET_C_BODY,
    PET_C_SHADE,
    PET_C_BELLY,
    PET_C_MARK,
    PET_C_EYE,
    PET_C_SHINE,
    PET_C_ACCENT,     /* nose, beak, feet */
    PET_C_CHEEK,
    PET_C_GLASS,      /* the fish's water bubble, translucent */
    PET_C_ACCENT2,    /* comb, horns, hooves, crest */
    PET_COLORS
};

typedef struct
{
    uint8_t species;       /* pet_species_t */
    uint8_t variant;       /* colour variant of the species */
    uint8_t mark;          /* pet_mark_t */
    uint8_t growth;        /* 0 (just born) .. 255 (fully grown) */
    uint8_t level;
    uint8_t stage;         /* 0 cucciolo, 1 giovane, 2 adulto, 3 saggio */
    bool cheeks;
} pet_look_t;

/* Level from experience points (same formula as the page and the companion). */
int pet_level(int32_t xp);
void pet_look(uint64_t seed, int level, pet_look_t *out);

/* Breed key for the discovered-breeds list: species, variant, mark. */
uint16_t pet_breed_key(const pet_look_t *look);
const char *pet_species_name(int species);      /* "Cane" */
const char *pet_variant_name(const pet_look_t *look);   /* "Biscotto" */
const char *pet_mark_name(int mark);            /* "a macchie" or "" */
const char *pet_stage_name(int stage);          /* "Cucciolo" */
int pet_variant_count(int species);

/* 0xAARRGGBB per palette index; mood_hue shifts the chameleon (0 = calm). */
void pet_palette(const pet_look_t *look, int mood, uint32_t out[PET_COLORS]);

/* frame: any counter, each pose cycles its own frames. Facing right. */
void pet_draw(const pet_look_t *look, pet_pose_t pose, uint32_t frame, bool blink, uint8_t grid[PET_GRID * PET_GRID]);

/* Personality qualities (docs/ORBI-PETS.md). Trait values from the genome. */
#define PET_TRAITS 5
typedef struct
{
    const char *name;     /* "Goloso" */
    const char *about;    /* one line */
} pet_quality_t;
/* values: curiosity, energy, sociability, appetite, playfulness (20..100).
 * order[i] = axis revealed i-th; returns how many are revealed. */
int pet_qualities(uint64_t seed, const uint8_t values[PET_TRAITS], int level, int days,
                  pet_quality_t out[PET_TRAITS], int order[PET_TRAITS]);

#endif
