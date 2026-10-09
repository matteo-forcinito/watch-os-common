#ifndef HARDWARE_USB_CONSOLE_H
#define HARDWARE_USB_CONSOLE_H

/*
 * The native USB port used as serial console / flashing port (USB Serial/JTAG).
 *
 * In automatic light sleep the port stops answering. If the PC tried to talk
 * to it meanwhile, Windows gives up ("device not recognized") and never tries
 * again: no flashing, no monitor until a restart. The power manager keeps the
 * chip awake while the cable has power and calls usb_console_reconnect(),
 * which makes the PC see a fresh plug-in if the port is not working.
 */

#include <stdbool.h>

/* True while a PC is talking to the console port. */
bool usb_console_connected(void);

/* If no PC is talking to the port: simulate unplug/plug (~20 ms). Blocks ~120 ms. */
void usb_console_reconnect(void);

#endif
