/* batlog.h - the tank measures its own battery, unattended (2026-09-11).
 * A ring of samples kept in RAM (drowse keeps RAM alive): state of charge,
 * VBAT, brightness, awake/asleep. Awake every 5 min, asleep at entry, every
 * 30 min and at wake. `batlog` on the director prints it later over USB
 * with the current derived between samples from the 200 mAh cell, so a
 * night on battery or an hour at a brightness level can be read back. */
#ifndef BATLOG_H
#define BATLOG_H
#include <stdbool.h>
#include <stdint.h>
#if defined(TANK_SQUARE)
#define BATLOG_CELL_MAH 1000   /* the square's pack (Strato, 2026-10-12) */
#elif defined(TANK_ROUND)
#define BATLOG_CELL_MAH 500    /* the pendant's (2026-10-03) */
#elif defined(TANK_WATCH)
#define BATLOG_CELL_MAH 400    /* the watch's (2026-10-03) */
#else
#define BATLOG_CELL_MAH 200    /* the 1.8's; the log's mA column and its mAh totals scale with it */
#endif
/* at boot, before the first sample: validates the RTC ring (magic + crc) and
   returns how many samples came through the reset - 0 after a power-on */
int  batlog_init(void);
void batlog_add(int pct, int mv, int bright, bool asleep, const char *why);
void batlog_print(void);
void batlog_clear(void);
#endif
