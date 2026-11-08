#ifndef SOLCO_ADPCM_H
#define SOLCO_ADPCM_H
/* Compatibility names for the shared, loss-resilient IMA codec. */
#include "../../components/core/include/core/adpcm.h"
typedef ima_adpcm_t solco_adpcm_t;
#define solco_adpcm_steps ima_adpcm_steps
#define solco_adpcm_index ima_adpcm_index
#define solco_adpcm_step ima_adpcm_step
#define solco_adpcm_encode ima_adpcm_encode
#define solco_adpcm_decode ima_adpcm_decode
#endif
