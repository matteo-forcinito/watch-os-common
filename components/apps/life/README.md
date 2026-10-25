# Raccolta and Zampe

These internal apps load their local documents only when requested. Dashboard
access uses appkit pairing. Routine notifications are opt-in; an enabled reminder
may run while its app is closed.

## User data and branding

Zampe is the public app name. Pet names, dates of birth, family relationships,
medical records and other profile details are private user content. Never put
personal profile data in app names, descriptions, icons, screenshots, fixtures,
sample documents or public release assets. A fresh installation has no profiles.
Examples and automated tests must use clearly fictional, generic data.

The documents live in local configuration storage, outside firmware assets.
Export is an explicit authenticated action. No cloud upload or external
analytics is part of either app. Keep exported backups private.

Raccolta's optional Civita Castellana preset is public municipality information,
not the user's selected location. The user chooses cities and a preferred city.
The preset is the domestic timetable effective 13 January 2025, published at
https://www.satespa.it/images/domestico.pdf. Date-specific changes must be entered
as exceptions; the app does not infer holiday collections.

Zampe records the care plan entered by the user and their veterinarian. It does
not generate medication doses or a universal vaccination schedule.
