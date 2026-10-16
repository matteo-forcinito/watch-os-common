# watch-os-common

Il sistema operativo da smartwatch condiviso dai firmware:

- `ESP32-S3-Amoled-Touch-1.8` — Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448, touch)
- `ESP32-S3-Touch-AMOLED-1.75` — Waveshare ESP32-S3-Touch-AMOLED-1.75 (466x466 rotondo)
- `ESP32-S3-Touch-AMOLED-1.75C` — Waveshare ESP32-S3-Touch-AMOLED-1.75C (466x466 rotondo, scocca chiusa)
- `ESP32-S3-Knob-Touch-LCD-1.8` — Waveshare Knob 1.8 (360x360 rotondo, rotella)
- `SenseCap-Watcher-W1-A` — Seeed SenseCAP Watcher W1-A (412x412 rotondo, touch + rotella)

**Guida per lavorare sul progetto** (build, app, test, diagnostica, release, companion):
[docs/guida/README.md](docs/guida/README.md).

```
components\
├─ board_api\   il "contratto" con l'hardware: solo header (display.h, touch.h, knob.h, pmu.h...)
├─ core\        porting LVGL, gestione energia, app manager, impostazioni, stato condiviso
├─ ui\          tema, font, widget comuni
├─ services\    Wi-Fi, ora, sveglie, notifiche, meteo, suoni, pagina web, app esterne, update
├─ apps\        shell (quadrante, launcher, centro di controllo) e app incluse
├─ companion\   Bluetooth LE con Gadgetbridge
├─ extapp_sdk\  per le app esterne installate da microSD
├─ webradio\    motore web radio (solo app esterna)
└─ hid_link\    tastiera/mouse BLE e USB (solo app esterna)
```

Ogni firmware ha il suo componente `hardware` che implementa gli header di `board_api` per i
propri chip, e aggiunge questa cartella con:

```cmake
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../watch-os-common/components")
```

Il codice qui dentro non conosce pin né chip: chiede `board_info()` (dimensioni, schermo
rotondo, rotella, batteria...). Ogni modifica va compilata in **tutti** i firmware.
