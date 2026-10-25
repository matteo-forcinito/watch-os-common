# Watch Lite

An optional external app for the six color-display watch OS targets. The full
system remains in the other OTA slot. Hold BOOT or choose **Torna al sistema**
to return. Do not flash a partition table or erase NVS to install this app.

Lite links the board drivers, a small direct RGB565 renderer and the essential
data services. It does **not** link or initialize LVGL, the companion, Bluetooth,
the radio player or the app launcher. The renderer uses a small DMA strip rather
than an application framebuffer. Some board drivers still need their own panel
buffers; the app cannot remove hardware requirements.

## On the device

Touch the clock to open settings. On knob devices, turn the knob to select and
click to activate. Settings provide always-on, Wi-Fi, brightness and return to
the full system. An alarm can be dismissed with a click/tap; tapping its left
side snoozes for five minutes. Hold BOOT to return to the full system.

Always-on is optional and off initially. It shows time, date, battery and the
next alarm. It redraws at minute changes, with small pixel shifts and brightness
8/255. LCD panels still consume power while lit. The active screen times out
after ten seconds. No measured battery-life claim is made.

## Dashboard and services

Enable Wi-Fi from device settings, then open the displayed address and pair
using the six-digit code. Previously paired system browsers retain access via
the same local token. Lite uses an already configured Wi-Fi network. Configure
networks in the full system first. There is no automatic network connection on
production boot. A failed connection retries at most every fifteen seconds.
The Wi-Fi driver and HTTP server are deinitialized when switched off, or after
the configured interval without authenticated requests (default ten minutes).

The dashboard controls display preferences, time synchronization from the
browser, existing alarms, local events, live task stacks, and return to the full
system. Raccolta and Zampe have their own authenticated dashboard folders and
share their existing private documents with the full system.

Clock and alarm scheduling remain active with Wi-Fi off. Giornata keeps local
events and the previously downloaded calendar cache, including reminders; it
does not download new calendars in Lite. Open Giornata in the full system to
refresh that cache. Optional Zampe notifications remain scheduled only if the
user enabled them. Sounds create a task only when requested by an alert. A
radio-based alarm uses the local melody in Lite. Hardware without a speaker
provides the visual alert. If a board has no valid retained clock, set it from
the dashboard before relying on alarms.

## Build

Software handovers preserve a valid clock through a small NVS checkpoint and
the ESP32 RTC counter, even without a working external RTC chip. Checkpoints
are rejected after power loss, counter rewind or more than seven days without
a fresh checkpoint. Setting trusted UTC and returning to the full system
refresh the checkpoint. This does not turn a board without a battery-backed
RTC into a clock that can keep time while physically unpowered.

ESP-IDF 6.1. Set `LITE_BOARD_DIR` to the supported firmware project and use its
sdkconfig.defaults followed by lite/sdkconfig.defaults. Preserve its partition
CSV and board configuration. `WATCH_OS_COMMON_DIR` can specify this repository.
Build `lite/` as an ESP-IDF project. `tools/build_lite.py` prepares separate
per-board build directories and verifies the dependency closure and linker map.

`LITE_QA=1` is a local test build that starts Wi-Fi and emits a pairing code on
serial. It is never a release artifact. Packaging must reject that definition.
The regular build leaves Wi-Fi off until the user requests it.
