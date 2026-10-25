#ifndef SOLCO_PREPARE_H
#define SOLCO_PREPARE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct
{
    int deck;
    uint32_t generation;
    char path[128], error[64];
} solco_prepared_t;
void solco_prepare_cancel(int deck, uint32_t generation);
bool solco_prepare_request(int deck, uint32_t generation, const char *source);
bool solco_prepare_poll(solco_prepared_t *result);
int solco_prepare_percent(int deck);
#endif
