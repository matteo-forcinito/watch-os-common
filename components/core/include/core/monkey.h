#ifndef CORE_MONKEY_H
#define CORE_MONKEY_H

#include <stdbool.h>

/*
 * Monkey test: random swipes, apps, screen on/off for hours, to find crashes
 * and freezes (see monkey.c). Does nothing unless CONFIG_WATCH_MONKEY_TEST is
 * on (menuconfig > Watch OS > Monkey test). Call once, at the end of the boot.
 */
void monkey_start(void);

/*
 * In monkey builds the app manager opens only the safe apps (a swipe that
 * LVGL takes for a tap must never start the USB drive, a countdown, a power
 * off...). Always true in normal builds.
 */
bool monkey_app_allowed(const char *id);

#endif
