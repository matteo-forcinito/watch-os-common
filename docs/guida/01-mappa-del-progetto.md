# 1. Mappa del progetto

## Le cartelle in `C:\Users\matte\Documents\ESP-IDF`

Ogni cartella è un **repository git separato** su GitHub (account `matteo-forcinito`).

| Cartella | Cosa contiene | Nome progetto (identità) | Catalogo OTA |
|---|---|---|---|
| `watch-os-common` | Il **sistema operativo condiviso**: grafica, servizi, app, web, Bluetooth | — | — |
| `ESP32-S3-Amoled-Touch-1.8` | Firmware Waveshare AMOLED 1.8 (368x448) — progetto "di riferimento" | `amoled_watch` | `amoled.json` |
| `ESP32-S3-Touch-AMOLED-1.75` | Firmware Waveshare AMOLED 1.75 (466 tondo) | `amoled175_watch` | `amoled175.json` |
| `ESP32-S3-Touch-AMOLED-1.75C` | Firmware Waveshare AMOLED 1.75C (466 tondo, scocca chiusa) | `amoled175c_watch` | `amoled175c.json` |
| `ESP32-S3-Knob-Touch-LCD-1.8` | Firmware Waveshare Knob (360 tondo, rotella, coprocessore) | `knob_os` | `knob.json` |
| `SenseCap-Watcher-W1-A` | Firmware Seeed SenseCAP Watcher (412 tondo, rotella, camera) | `watcher_os` | `watcher.json` |
| `Orbit-Companion` | App per PC e telefono (Flutter) che parla con i dispositivi | — | release GitHub proprie |
| `esp32-s3-epaper-1.54` | Sveglia e-paper: progetto **separato**, non usa `watch-os-common` | — | `epaper.json` |
| `esp32-app-skeleton`, `test` | Scheletro iniziale e prove vecchie | — | — |

Un repository in più vive solo su GitHub: `watch-firmware-releases` (**pubblico**).
Contiene solo i binari pubblicati e i cataloghi JSON che i dispositivi leggono
per aggiornarsi. Il codice resta privato.

## Come il firmware di un dispositivo usa le librerie comuni

Il firmware di una scheda contiene **solo ciò che riguarda quell'hardware**. Tutto il
resto lo prende da `watch-os-common`.

```
ESP32-S3-Touch-AMOLED-1.75C/
├─ CMakeLists.txt          ← dice a ESP-IDF dove trovare watch-os-common
├─ main/main.c             ← l'avvio: inizializza tutto nell'ordine giusto
├─ components/hardware/    ← i driver di QUESTA scheda (display, touch, PMU...)
│   └─ boards/waveshare_amoled_1_75c.h   ← i pin (unico posto dove stanno)
├─ watch-os-common/        ← submodule git: una copia "fissata" della libreria
├─ sdkconfig.defaults      ← le opzioni di compilazione (versionate)
└─ release/notes/X.Y.Z.md  ← note di rilascio
```

Nel `CMakeLists.txt` di ogni firmware c'è questa scelta:

```cmake
if(DEFINED ENV{WATCH_OS_COMMON_DIR})
    # Stai lavorando sulla libreria: usa la tua copia di lavoro
    set(WATCH_OS_COMMON "$ENV{WATCH_OS_COMMON_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/watch-os-common/components")
    # Caso normale (e GitHub Actions): il submodule
    set(WATCH_OS_COMMON "${CMAKE_CURRENT_LIST_DIR}/watch-os-common")
else()
    set(WATCH_OS_COMMON "${CMAKE_CURRENT_LIST_DIR}/../watch-os-common")
endif()
set(EXTRA_COMPONENT_DIRS "${WATCH_OS_COMMON}/components")
```

Quindi ci sono **due copie** di `watch-os-common`:

- `ESP-IDF\watch-os-common` — la **copia di lavoro**, quella che modifichi.
- `ESP-IDF\<firmware>\watch-os-common` — il **submodule**, cioè un puntatore a un
  commit preciso. È quello che compila GitHub quando fa una release.

Mentre sviluppi imposti `WATCH_OS_COMMON_DIR` e compili contro la copia di lavoro.
Quando hai finito fai commit e push della libreria e poi **sposti il puntatore** del
submodule in ogni firmware (vedi [pagina 12](12-flusso-di-lavoro.md)).

### Il "contratto" con l'hardware: `board_api`

`watch-os-common/components/board_api/include/hardware/` contiene solo **header**
(dichiarazioni di funzioni): `display.h`, `touch.h`, `pmu.h`, `knob.h`, `audio.h`...
Ogni firmware li **implementa** nel suo `components/hardware`. Il codice condiviso non
conosce pin o chip: chiede `board_info()`, che restituisce dimensioni, schermo tondo,
rotella, batteria, ecc.

```c
const board_info_t *b = board_info();
if (b->round) { /* centra i testi lontano dai bordi */ }
if (b->has_knob) { /* mostra il suggerimento "gira la rotella" */ }
```

## Dentro `watch-os-common`

```
watch-os-common/
├─ components/            ← componenti ESP-IDF condivisi
│  ├─ board_api/          contratto hardware (solo header)
│  ├─ board_drivers/      driver comuni a più schede (es. IMU QMI8658)
│  ├─ core/               porting LVGL, energia, app manager, impostazioni, stato, console di sviluppo
│  ├─ ui/                 tema, font, widget comuni (ui_page, ui_row, ui_button...)
│  ├─ services/           Wi-Fi, ora, sveglie, notifiche, meteo, suoni, web, aggiornamenti, Market
│  ├─ apps/               shell (quadrante, launcher, centro di controllo) e app incluse
│  ├─ companion/          Bluetooth LE (Gadgetbridge + Orbit Companion)
│  ├─ appkit/             ciò che serve alle app "hosted" (dashboard web, sveglie)
│  ├─ extapp_sdk/         per le app esterne (ritorno al launcher)
│  ├─ webradio/, hid_link/  solo per app esterne (radio, tastiera/mouse)
├─ pieno/, giornata/, news/, copertura/, ombra/, sentinel/...  ← codice delle app "hosted"
├─ docs/                  documentazione tecnica (questa guida è in docs/guida)
└─ tools/                 script: release_firmware.py, test, tools/dev (diagnostica)
```

### I livelli

Ogni livello usa **solo** quelli sotto di lui:

```
apps (shell + app)
   ↓
companion / webradio / hid_link
   ↓
services (Wi-Fi, web, sveglie...)
   ↓
ui (widget)
   ↓
core (LVGL, energia, impostazioni, stato)
   ↓
hardware (driver della scheda, via board_api)
```

Esempio: un servizio (Wi-Fi) non deve mai creare widget; pubblica uno stato con
`state_set()` e la UI lo osserva. Questo tiene il codice ordinato e senza blocchi.
