# 6. Aggiungere un nuovo dispositivo

Per supportare una nuova scheda si crea un **nuovo repository firmware** che contiene
solo il livello hardware e l'avvio, e riusa `watch-os-common`. Il modo più rapido è
copiare il firmware della scheda più simile.

| Se la nuova scheda ha... | Parti da |
|---|---|
| AMOLED tondo con PMU AXP2101 | `ESP32-S3-Touch-AMOLED-1.75C` |
| Schermo rettangolare, microSD | `ESP32-S3-Amoled-Touch-1.8` |
| Rotella, niente PMU | `ESP32-S3-Knob-Touch-LCD-1.8` |
| IO expander, rotella con tasto | `SenseCap-Watcher-W1-A` |

## Passo 1 — Copia e rinomina

1. Copia la cartella (senza `build*`, `managed_components`, `sdkconfig`) in
   `ESP-IDF\<NuovaScheda>`.
2. Nel `CMakeLists.txt` cambia `project(...)` con un nome nuovo e definitivo,
   per esempio `project(round2_watch)`. **Non lo cambierai più**: è l'identità che
   l'aggiornatore controlla.
3. Riparti da `PROJECT_VER "0.1.0"` e svuota `release/notes/`.

## Passo 2 — Il livello hardware (`components/hardware`)

Qui va implementato ogni header di `watch-os-common/components/board_api/include/hardware/`:

| Header | Cosa deve fare | Se la scheda non ce l'ha |
|---|---|---|
| `board.h` | `board_init()` (bus, chip sempre accesi) e `board_info()` | obbligatorio |
| `display.h` | init pannello, disegno di un'area (DMA), luminosità, sleep, `display_round_area()` | obbligatorio |
| `touch.h` | lettura punto, interrupt, low power | `has_touch = false` |
| `pmu.h` | batteria %, carica, USB, spegnimento | stub che restituiscono "sconosciuto", `has_pmu = false` |
| `rtc.h` | ora hardware | stub, `has_rtc = false` |
| `button.h` | tasti BOOT/PWR → eventi | stub, `has_buttons = false` |
| `knob.h` | rotella | stub, `has_knob = false` |
| `audio.h`, `microphone.h` | codec | stub, `has_audio = false` |
| `sdcard.h` | microSD | stub, `has_sdcard = false` |
| `imu.h` | accelerometro | stub |
| `usb_console.h` | console USB | quasi sempre copiabile così com'è |

I **pin** vanno in un solo file: `components/hardware/boards/<scheda>.h`, incluso da
`boards/board_config.h`. Nessun altro file contiene numeri di pin.

`board_info()` descrive la scheda al sistema condiviso:

```c
static const board_info_t s_info = {
    .name = "Produttore Modello 1.9",   /* Impostazioni > Info */
    .product = "Round Watch",            /* titolo della pagina web, rete Wi-Fi di setup */
    .short_name = "ROUND",               /* nome Bluetooth */
    .width = 410, .height = 502,
    .round = false,
    .has_touch = true, .has_knob = false, .has_pmu = true, .has_rtc = true,
    .has_audio = true, .has_sdcard = false, .has_usb_otg = true, .has_buttons = true,
    .display_corner_radius = 40,
};

const board_info_t *board_info(void) { return &s_info; }
```

Comportamenti particolari della scheda (allineamenti del pannello, pin da tenere in
light sleep, polarità) restano **dentro** `components/hardware`. Se il sistema condiviso
deve comportarsi diversamente, si aggiunge un flag a `board_info_t` (in fondo) o una
funzione a `board_api`, e si implementa in **tutti** i firmware.

## Passo 3 — `sdkconfig.defaults` e partizioni

- Flash e PSRAM giuste per il modulo (`CONFIG_ESPTOOLPY_FLASHSIZE_...`, `CONFIG_SPIRAM_...`).
- `CONFIG_BOARD_DRAW_BUFFER_LINES` (righe del buffer di disegno in RAM interna).
- URL del catalogo OTA del nuovo prodotto:
  ```
  CONFIG_WATCH_UPDATE_MANIFEST_URL="https://raw.githubusercontent.com/matteo-forcinito/watch-firmware-releases/main/round2.json"
  ```
- `partitions.csv`: due slot app (`ota_0`, `ota_1`) della stessa dimensione, `coredump`,
  eventuale `storage` FAT.

## Passo 4 — Prima accensione

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash        # la prima volta flash completo (bootloader + partizioni)
idf.py -p COMx monitor
```

Ordine consigliato: prima il **display** (anche solo un colore pieno), poi touch,
poi PMU/batteria, poi audio. Abilita la console di sviluppo ([pagina 8](08-console-e-diagnostica.md))
e verifica ogni app con `shot`.

## Passo 5 — Collegarlo al resto

1. **Git e GitHub**: nuovo repository privato `matteo-forcinito/<NuovaScheda>`, con
   `watch-os-common` come submodule:
   ```powershell
   git init
   git submodule add https://github.com/matteo-forcinito/watch-os-common.git watch-os-common
   git add -A; git commit -m "First board bring-up"
   git remote add origin https://github.com/matteo-forcinito/<NuovaScheda>.git
   git push -u origin main
   ```
2. **Secret** nel repository (Settings → Secrets and variables → Actions):
   `REPOS_TOKEN` (lettura di `watch-os-common`), come negli altri firmware.
3. **Workflow**: copia `.github/workflows/release.yml` e cambia `PRODUCT`,
   `PRODUCT_NAME`, `APP_BIN`, il gruppo di `concurrency` e i nomi degli artifact.
4. **Script di release**: in `watch-os-common/tools/release_firmware.py` aggiungi il
   prodotto alla tabella:
   ```python
   PRODUCTS = {'amoled175': 'amoled175_watch', ..., 'round2': 'round2_watch'}
   ```
   e copia `tools/release.ps1` cambiando `--repo` e `--product`.
5. **Companion**: in `Orbit-Companion/lib/devices.dart` aggiungi un `DeviceModel`
   (id, nome, foto in `assets/devices/`, risoluzione, area dello schermo nella foto,
   tondo/rettangolare). Vedi [pagina 11](11-companion-flutter.md).
6. **Documentazione**: `CLAUDE.md` e `README.md` del nuovo repo, e la tabella in
   [pagina 1](01-mappa-del-progetto.md).

Da questo momento ogni modifica a `watch-os-common` va compilata anche qui.
