#ifndef SOLCO_WAV_H
#define SOLCO_WAV_H

/* Strict RIFF reader. The prototype accepts PCM16 at the engine's 44.1 kHz. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct
{
    uint32_t offset;
    uint32_t bytes;
    uint16_t channels;
} solco_wav_t;

bool solco_wav_open(FILE *file, solco_wav_t *wav);
#endif
