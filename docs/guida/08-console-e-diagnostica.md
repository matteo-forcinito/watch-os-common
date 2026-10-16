# 8. Console di sviluppo e diagnostica

Per provare il firmware senza toccare il dispositivo: aprire app, simulare tocchi,
fare screenshot, leggere salute e RAM, analizzare i crash, misurare le prestazioni.

## Build "di manutenzione": abilitare la console di sviluppo

La console è un'opzione **solo locale**: non va mai in `sdkconfig.defaults`, quindi
non finisce nelle release.

Due modi per abilitarla nel tuo `sdkconfig`:

```powershell
idf.py menuconfig      # Watch OS → Development console
```

oppure al volo (è quello che uso io):

```powershell
(Get-Content sdkconfig) -replace '^# CONFIG_WATCH_DEV_CONSOLE is not set$','CONFIG_WATCH_DEV_CONSOLE=y' | Set-Content -Encoding ascii sdkconfig
idf.py build
idf.py -p COM10 app-flash
```

Per tornare alla build normale: rimetti `# CONFIG_WATCH_DEV_CONSOLE is not set`
(o cancella `sdkconfig`, che viene rigenerato da `sdkconfig.defaults`).

Altre opzioni utili nello stesso menu:

| Opzione | A cosa serve |
|---|---|
| `CONFIG_WATCH_DEV_CONSOLE` | Comandi da PC via USB (questa pagina) |
| `CONFIG_WATCH_MONKEY_TEST` | "Scimmia": swipe, app e schermo on/off casuali per ore, per trovare crash e blocchi |
| `CONFIG_HEAP_TRACING_STANDALONE` (Component config → Heap memory debugging) | Abilita `heapstart`/`heapstop` per cercare memory leak |

## I comandi della console

Si mandano con `tools/devcon.py` di ogni firmware (AMOLED 1.8, 1.75, 1.75C, Knob):

```powershell
python tools\devcon.py COM10 apps                        # elenca le app (id e nome) e quella aperta
python tools\devcon.py COM10 "open settings" shot:s.png  # apre Impostazioni e salva uno screenshot PNG
python tools\devcon.py COM10 "swipe 230 100 230 380"     # swipe da (230,100) a (230,380)
python tools\devcon.py COM10 health pmu                  # salute del sistema e registri batteria
```

| Comando | Effetto |
|---|---|
| `apps` | Elenco app (`@APP id "Nome"`), e `@CURRENT` = app aperta |
| `open <id>` | Apre un'app (`open web`, `open settings.wifi`...) |
| `back`, `home`, `button` | Indietro, home, pressione del tasto di navigazione |
| `swipe x1 y1 x2 y2` | Swipe sintetico |
| `press x y ms` | Tocco tenuto per `ms` millisecondi |
| `touch x y 1/0` | Dito giù / su |
| `knob n` | Gira la rotella di `n` scatti (anche negativi) |
| `shot` (o `shot:file.png` da devcon.py) | Screenshot di schermo + overlay |
| `wake`, `sleep` | Accende / spegne lo schermo |
| `health` | RAM interna/PSRAM, pass UI più lento, stack liberi dei task |
| `pmu` | Registri del PMU (batteria, carica) |
| `wifi-status`, `wifi-drop` | Stato Wi-Fi, simula una disconnessione |
| `recovery on/off` | Modalità recovery (schermo virtuale) |
| `loglevel n` | Livello log (0 nessuno … 5 verbose) |
| `wav`, `wavlist`, `wavtone` | Audio di prova |
| `gpio n` | Legge un pin |
| `heapstart`, `heapstop` | Traccia allocazioni (con heap tracing abilitato) |

Il **Watcher** ha una console sua (`main/devtools.c`) con comandi in maiuscolo:

```powershell
cd SenseCap-Watcher-W1-A
python tools/screenshot.py COM6 shot.png "OPEN settings"
python tools/screenshot.py COM6 - "KNOB 3" "CLICK"
```

### Attenzione: aprire la porta seriale può riavviare la scheda

Su USB nativa aprire la porta con DTR/RTS attivi resetta l'ESP32. `devcon.py` lo fa:
ogni invocazione riparte da zero (l'app aperta si chiude). Per una sequenza di comandi
**senza reset** usa:

```powershell
python -I ..\watch-os-common\tools\dev\devcon_noreset.py COM10 tools\devcon.py "open web" wait:12 "shot:web.png"
```

`wait:N` aspetta N secondi tra un comando e l'altro.

## Script di diagnostica in `watch-os-common/tools/dev`

Richiedono il profilo ESP-IDF attivo (forniscono `pyserial`).

| Script | Uso |
|---|---|
| `bootlog.py COM10 6 "filtro\|altro"` | Riavvia e stampa il log di avvio (filtrato) |
| `serlog.py COM10 20 [file]` | Legge il log per 20 s **senza** riavviare |
| `devcon_noreset.py COM10 devcon.py cmd...` | Comandi console senza reset, con `wait:` e `shot:` |
| `remote_mirror_test.py IP token.txt [codice]` | Collaudo dello schermo remoto ORF2: abbina, misura i frame, invia swipe/home, chiude la sessione |

Esempio reale (collaudo schermo remoto sulla 1.75C):

```powershell
# 1. apri la pagina web sull'orologio e leggi il codice dallo screenshot
python -I ..\watch-os-common\tools\dev\devcon_noreset.py COM10 tools\devcon.py "open web" wait:12 "shot:web.png"
# 2. collaudo: la prima volta con il codice a 6 cifre, poi basta il file del token
python -I ..\watch-os-common\tools\dev\remote_mirror_test.py 192.168.1.11 token.txt 002571
```

## Leggere la salute del sistema

Righe `sys:` nel log (all'avvio e con `health`):

```
sys: [health] internal 48 KB free (largest 31 KB, min 26 KB), PSRAM 7129 KB free
sys: [health] up 2 min, slowest UI pass 425 ms, lost flush IRQs 0
sys: [health] stack free: lvgl:5488 power:4780 tcpip:1736 ...
```

- **internal free / largest**: RAM interna libera e blocco più grande. Sotto ~30 KB di
  "largest" Wi-Fi o Bluetooth possono non partire.
- **min**: minimo raggiunto dall'avvio (il punto peggiore).
- **slowest UI pass**: il giro più lento del task `lvgl`. Centinaia di ms = scatti visibili.
  Oltre 8 s il sistema si riavvia da solo con coredump ("lvgl task stuck").
- **lost flush IRQs**: il pannello non ha confermato un trasferimento (problema display).
- **stack free**: byte liberi nello stack di ogni task. Sotto ~500 rischio di overflow.

`sys_heap_log("etichetta")` nel codice stampa la stessa riga in un punto preciso: utile
per capire quanta RAM consuma una funzione (prima/dopo).

## Analizzare un crash

Al riavvio dopo un crash il log dice `sys: PREVIOUS RUN CRASHED` e l'orologio mostra
"Riavviato dopo un errore". Il dump è salvato nella partizione `coredump`:

```powershell
idf.py -p COM10 coredump-info
```

Mostra task, causa (es. `LoadProhibited` = puntatore non valido) e la **pila delle
chiamate** con file e riga. Attenzione: funziona solo se il `build\` contiene lo **stesso
firmware** che è crashato (altrimenti: `coredump SHA256 != app SHA256`). Se il crash è
di una release, ricompila quel commit (tag `<prodotto>-vX.Y.Z`) prima di leggerlo.

Cause più comuni in questo progetto:

| Sintomo nel coredump | Causa tipica |
|---|---|
| Crash in `lv_...` da un task diverso da `lvgl` | LVGL toccata fuori dal suo task |
| `Stack overflow in task X` | Stack troppo piccolo (es. lavoro pesante in `esp_timer` o in `sys`) |
| `abort()` da `ESP_ERROR_CHECK` | Una chiamata ha restituito errore all'avvio |
| `lvgl task stuck` | Attesa bloccante nella UI (rete, mutex) |

## Misurare le prestazioni

- **Fluidità UI**: `health` dopo aver usato l'app → `slowest UI pass`.
- **Tempi di rete**: `tools/dev/remote_mirror_test.py` stampa ms e byte per ogni frame.
- **Tempo di una funzione** nel codice:
  ```c
  int64_t t0 = esp_timer_get_time();
  fai_qualcosa();
  ESP_LOGI(TAG, "fai_qualcosa: %lld ms", (esp_timer_get_time() - t0) / 1000);
  ```
- **Consumi**: la batteria va misurata sul dispositivo nel tempo; `Impostazioni → Batteria`
  e la pagina web Batteria mostrano stime, non misure.

## Prove di diagnostica "in RAM" (senza scrivere la flash)

Per leggere registri hardware senza toccare il firmware installato si può compilare un
piccolo programma ed eseguirlo direttamente in RAM (`esptool --no-stub load-ram`).
È stato usato per la diagnosi della batteria della 1.75: vedi
`docs/AMOLED175-BATTERIA-DIAGNOSI.md` e i sorgenti in `build/battery-diag/`.
Dopo la prova un reset riporta il firmware normale.
