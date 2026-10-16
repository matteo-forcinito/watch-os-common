# 2. Ambiente, build e flash

## Cosa è installato

| Strumento | Dove | A cosa serve |
|---|---|---|
| ESP-IDF 6.1 | `C:\esp\v6.1\esp-idf`, strumenti in `C:\Espressif` | Compilatore e strumenti per ESP32 |
| Profilo PowerShell | `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` | Attiva l'ambiente (`idf.py`, Python con `pyserial`...) |
| Flutter 3.47.7 | `C:\Users\matte\Documents\flutter` | Per la companion |
| GitHub CLI `gh` | `ESP32-S3-Knob-Touch-LCD-1.8\.companion-tools\gh\bin\gh.exe` | Release (già autenticato come `matteo-forcinito`) |
| Node.js | nel PATH | Controllo sintassi JS della dashboard, test JS |

## Aprire un terminale "pronto"

In PowerShell, **ogni nuova finestra**:

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
```

(il punto iniziale e lo spazio sono importanti: "esegui nel terminale corrente").

## Compilare un firmware

```powershell
cd C:\Users\matte\Documents\ESP-IDF\ESP32-S3-Touch-AMOLED-1.75C
idf.py build
```

- La prima build dura parecchi minuti (compila tutto ESP-IDF); le successive solo i file cambiati.
- Il risultato è in `build\`: `amoled175c_watch.bin` (l'app) e `build\firmware.bin` (copia con nome uguale per tutti).
- Il nostro codice è compilato con `-Werror`: **un warning è un errore**. I warning che
  vedi provenire da `C:/esp/v6.1/esp-idf/...` (es. `Atomic_XOR_u32 defined but not used`)
  sono di ESP-IDF e si ignorano.

### Compilare contro la copia di lavoro della libreria

Se hai modificato `ESP-IDF\watch-os-common`:

```powershell
$env:WATCH_OS_COMMON_DIR = "C:/Users/matte/Documents/ESP-IDF/watch-os-common"
idf.py build
```

La variabile vale finché resta aperta la finestra. Senza di lei si compila il submodule.

### Versione nell'immagine

La versione la decide la release (dal nome della versione). In locale si usa il
valore di `set(PROJECT_VER "...")` nel `CMakeLists.txt`. Per forzarla:

```powershell
$env:FW_VERSION = "1.8.5-dev.1"
idf.py reconfigure build     # reconfigure serve perché CMake rilegga la variabile
```

## Trovare la porta COM giusta

```powershell
python -I ..\watch-os-common\tools\dev\bootlog.py COM10 6 "Project name|App version|board"
```

Riavvia la scheda e stampa le righe utili, per esempio:

```
I (554) app_init: Project name:     amoled175c_watch
I (554) app_init: App version:      1.8.4
I (637) board: Waveshare AMOLED 1.75C (466x466)
```

**Controlla sempre il nome progetto prima di flashare**: un firmware sbagliato sulla
scheda sbagliata non si avvia (si recupera via USB, ma è tempo perso).

## Caricare il firmware (flash)

| Comando | Cosa scrive | Quando usarlo |
|---|---|---|
| `idf.py -p COM10 app-flash` | Solo l'app | **Il normale**: impostazioni, Wi-Fi, abbinamenti restano |
| `idf.py -p COM10 flash` | Bootloader + tabella partizioni + app | Quando cambia `partitions.csv` o il bootloader |
| `idf.py -p COM10 erase-flash` | Cancella **tutto** | Quasi mai. Sul Watcher è **vietato**: cancellerebbe `nvsfactory` (identità Seeed) |

## Leggere il log

```powershell
idf.py -p COM10 monitor          # interattivo, si esce con Ctrl+]
python -I ..\watch-os-common\tools\dev\serlog.py COM10 20 log.txt   # 20 s senza reset, salva su file
```

Righe da conoscere nel log di avvio:

- `main: Ready` — avvio completato.
- `sys: [...] internal 87 KB free (largest 48 KB...)` — RAM interna libera (la risorsa più scarsa).
- `sys: PREVIOUS RUN CRASHED` — il giro precedente è finito in crash: vedi [pagina 8](08-console-e-diagnostica.md).

## Configurazione: `sdkconfig` e `sdkconfig.defaults`

- `sdkconfig.defaults` è **versionato** ed è ciò che usa la release (che cancella
  `sdkconfig` e lo rigenera da qui).
- `sdkconfig` è la tua copia locale (ignorata da git). Puoi cambiarla con
  `idf.py menuconfig` per prove (es. abilitare la console di sviluppo).
- Un'opzione che deve arrivare ai dispositivi va messa in `sdkconfig.defaults`.

## Compilare tutti i firmware (dopo una modifica alla libreria)

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
$env:WATCH_OS_COMMON_DIR = "C:/Users/matte/Documents/ESP-IDF/watch-os-common"
foreach ($d in "ESP32-S3-Amoled-Touch-1.8","ESP32-S3-Touch-AMOLED-1.75","ESP32-S3-Touch-AMOLED-1.75C","ESP32-S3-Knob-Touch-LCD-1.8","SenseCap-Watcher-W1-A") {
    Set-Location "C:\Users\matte\Documents\ESP-IDF\$d"
    idf.py build > build-local.log 2>&1
    "$d -> exit $LASTEXITCODE"
    Select-String -Path build-local.log -Pattern "error:|FAILED" | Select-Object -First 5
}
```

`exit 0` per tutti = ok. Altrimenti apri `build-local.log` della cartella fallita.
