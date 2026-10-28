/* psram_plan.h — the PSRAM/flash memory plan, asserted at boot.
 *
 * ESP32-S3R8: 512 KB SRAM, 8 MB octal PSRAM, 16 MB flash.
 *   flash  : model partition (raw, 8 MB) holds model_q4.bin (~7.6 MB), read
 *            through the flash cache via esp_partition_mmap — NOT copied to RAM
 *   PSRAM  : two RGB565 framebuffers, KV cache, activations, tank state
 *   SRAM   : FreeRTOS, hot loop scratch, stacks
 * See docs/memory_budget.md for the arithmetic. */
#ifndef PSRAM_PLAN_H
#define PSRAM_PLAN_H

#include <stdint.h>
#include "tank.h"

#define PLAN_FB_W            TANK_W                               /* 448 x 368; the round build's bowl is 466 x 466, the watch's tank 410 x 502, the square's 480 x 480 */
#define PLAN_FB_H            TANK_H
#define PLAN_FB_BYTES        (PLAN_FB_W * PLAN_FB_H * 2)          /* 329,728 (434,312; 411,640; 460,800) */
#define PLAN_FB_ALLOC        ((PLAN_FB_BYTES + 63) & ~63)         /* what a frame buffer is allocated as: the scene prefetch's
                                                                     DMA copies whole 64-byte lines (the bowl's 434,312 is not one) */
#define PLAN_FB_COUNT        2                                    /* double buffer */
#define PLAN_FB_TOTAL        (PLAN_FB_BYTES * PLAN_FB_COUNT)      /* 659,456 */

/* model: dim 384, 8 layers, 8 heads, seq 64 (word tokens) */
#define PLAN_MODEL_DIM       384
#define PLAN_MODEL_LAYERS    8
#define PLAN_MODEL_SEQ       64
#define PLAN_KV_BYTES        (2 * PLAN_MODEL_LAYERS * PLAN_MODEL_SEQ * PLAN_MODEL_DIM * 4) /* 1,572,864 fp32 */
#define PLAN_ACT_BYTES       (1024 * 1024)                        /* run state + batched-prefill buffers (64 tok) */
#define PLAN_MODEL_FLASH_MAX (8u * 1024 * 1024)                   /* model partition size */

#define PLAN_PSRAM_MIN_FREE_AFTER (1u * 1024 * 1024)              /* headroom we insist on (QEMU has 4 MB) */

#endif
