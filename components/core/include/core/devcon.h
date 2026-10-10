#ifndef CORE_DEVCON_H
#define CORE_DEVCON_H

/*
 * Development console: test a device from the PC (screenshots, open apps,
 * knob, swipes, GPIO and power readings). Only with CONFIG_WATCH_DEV_CONSOLE
 * (menuconfig > Watch OS); see devcon.c and tools/devcon.py. Call once at boot.
 */
void devcon_start(void);

#endif
