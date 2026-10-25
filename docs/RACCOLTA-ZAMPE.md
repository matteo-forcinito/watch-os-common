# Raccolta and Zampe

Two internal apps, with matching device pages and separate offline dashboards.
Open an app and choose Dashboard to enable networking; opening the app alone
registers its page but does not turn on Wi-Fi. Browser access uses the existing
device pairing. Each app releases its document cache after closing and finishing
pending saves. File writes run in the short-lived `care_save` task.

## Raccolta

Configure up to eight cities, twelve freely named collections per city,
weekday masks, week intervals (1–8), an anchor date, colors and date-specific
exceptions. An empty exception cancels all pickups on that date. Select a
preferred city and optionally show its next-day collections in the main menu.
Cities can group materials in any way, including glass with other materials.

The optional Civita Castellana domestic preset follows SATE's timetable in force
from 13 January 2025: organic Monday/Thursday/Saturday, residual Tuesday,
plastic and metal Wednesday, glass Thursday, paper Friday. Source:
https://www.satespa.it/images/domestico.pdf. Check the operator's notices for
changes; holidays are not guessed. Fresh installations have no selected city.

## Zampe

Up to eight private dog profiles, with birth date, sex, breed, family relation,
veterinary contacts, microchip, food, sensitivities and notes. Routines and dated
care activities support recurrence, postponement, completion history and
optional local notifications. Weight measurements and a diary track changes
over time; the dashboard plots weight history. Medical categories record the
user's veterinarian plan without inventing treatment or dosage schedules.

Fresh installations contain no pet profiles. All personal details are user data,
never part of app branding, firmware examples or public release assets. Export
is an explicit authenticated download, with no cloud upload. Profile deletion
asks for confirmation and removes its linked activities, weights and diary.

## Persistence and validation

`config/raccolta-v1.json` and `config/zampe-v1.json` live in available local file
storage. Writes use a temporary file, flush, sync and backup rename. Missing
primary files can be recovered from the backup. An unreadable or invalid
existing document is not replaced with empty defaults. Failed writes retain
their pending data in RAM and expose a retry control; do not power off until a
retry succeeds. Lightweight notification metadata lives in NVS. File storage
is required for durable app data.

There is no always-running app task. Only explicitly enabled care reminders
arm a one-shot timer. Time and timezone changes replan those reminders. The
system activity page reports actual tasks, their purpose and unused minimum
stack; this is not a per-service total heap or CPU attribution.

Validation includes the actual C date/calendar/care model on the host and
authenticated device API tests for persistence, exceptions and linked deletion.
