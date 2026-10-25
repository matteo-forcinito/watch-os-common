#ifndef SOLCO_MP3_H
#define SOLCO_MP3_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef bool (*solco_prepare_progress_fn)(int percent, void *context);
/* Sequential, gapless MP3 decode to PCM16 stereo 44.1 kHz. Caller publishes atomically. */
bool solco_mp3_decode(FILE *input, FILE *output, uint64_t byte_limit, solco_prepare_progress_fn progress,
                      void *context, char *error, size_t error_size);
#endif
