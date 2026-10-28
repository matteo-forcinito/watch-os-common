#ifndef HARDWARE_AUDIO_H
#define HARDWARE_AUDIO_H

/*
 * Speaker output through the audio codec (ES8311 on the supported boards).
 *
 *   samples -> I2S -> codec DAC -> amplifier -> speaker
 *
 * audio_start() powers the codec and the amplifier, audio_stop() turns them
 * off again. While audio runs the I2S clock must keep going, so automatic
 * light sleep is blocked until audio_stop().
 *
 * Only one user at a time: the sound service and the radio hand over the
 * speaker explicitly (see services/sound).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Open the output (8000..48000 Hz, 1 or 2 channels, 16 bit). Reopens if the format changed. */
esp_err_t audio_start(uint32_t sample_rate, int channels);

/* Like audio_start(), with an explicit DMA queue of desc_num descriptors of
 * frames_per_desc frames each: output latency = desc_num * frames_per_desc / rate.
 * 0 keeps the board default for that field (audio_start() uses both defaults).
 * Writers should use blocks that divide frames_per_desc, so that each write
 * returns at a regular period. A different queue reopens the output. */
esp_err_t audio_start_ex(uint32_t sample_rate, int channels, int frames_per_desc, int desc_num);

/* Frames in the open DMA queue (descriptors x frames), 0 when unknown/closed. */
int audio_queue_frames(void);

/* 0..100 %. */
esp_err_t audio_set_volume(int percent);

/* Play interleaved 16-bit samples (count = frames x channels). Blocks while the DMA queue is full. */
esp_err_t audio_write(const int16_t *samples, size_t count);

void audio_stop(void);

bool audio_is_running(void);

#endif
