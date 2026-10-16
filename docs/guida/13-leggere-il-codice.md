# 13. Leggere il codice: navigazione, cicli di vita, memoria

## 1. Far funzionare "vai alla definizione" in VS Code

### Perché gli `#include` non portavano da nessuna parte

Un `#include "core/app.h"` non dice *dove* sta il file: lo decide il compilatore con
l'elenco delle cartelle di include (`-I...`) di quel componente. VS Code lo sa solo se
legge `build\compile_commands.json`, che ESP-IDF genera a ogni build e che contiene, per
**ogni** file `.c`, il comando di compilazione completo. Mancava la configurazione che
glielo dicesse.

### Cosa è configurato ora

In `C:\Users\matte\Documents\ESP-IDF\.vscode\c_cpp_properties.json` c'è una
configurazione per firmware (AMOLED 1.75C, AMOLED 1.75, AMOLED 1.8, Knob 1.8, Watcher),
ognuna puntata al suo `build\compile_commands.json`.

Per usarla:

1. Apri in VS Code la cartella **`ESP-IDF`** (quella che contiene tutti i progetti).
2. Il firmware scelto deve essere **compilato almeno una volta** (`idf.py build`):
   senza `build\compile_commands.json` non c'è nulla da leggere.
3. Apri un `.c`, in basso a destra nella barra di stato clicca il nome della configurazione
   (es. "AMOLED 1.75C") per sceglierne un'altra. Oppure `Ctrl+Shift+P` →
   **C/C++: Select a Configuration...**
4. La prima indicizzazione richiede qualche minuto (icona in basso). Se qualcosa non si
   aggiorna: `Ctrl+Shift+P` → **C/C++: Reset IntelliSense Database**.

Importante: le build fatte con `$env:WATCH_OS_COMMON_DIR` compilano i file da
`ESP-IDF\watch-os-common` (la copia di lavoro). Apri i file **da lì**, non dalle copie dentro
`<firmware>\watch-os-common` (il submodule), che VS Code non riconosce come compilati.
Per lo stesso motivo la ricerca di VS Code esclude le copie submodule.

### Comandi di navigazione

| Tasti | Cosa fa |
|---|---|
| `F12` (o Ctrl+clic) | Vai alla definizione della funzione/variabile/tipo sotto il cursore |
| `Alt+F12` | Mostra la definizione in un riquadro, senza lasciare il file |
| `Ctrl+clic` su un `#include` | Apre l'header |
| `Shift+F12` | Tutti i punti in cui è usata |
| `Ctrl+Shift+O` | Elenco delle funzioni del file corrente |
| `Ctrl+T` | Cerca una funzione per nome in tutto il progetto |
| `Ctrl+P` | Apre un file per nome (`app_manager.c`) |
| `Ctrl+Shift+F` | Cerca un testo ovunque (stringhe mostrate sullo schermo, messaggi di log) |
| `Alt+←` / `Alt+→` | Torna indietro / avanti nei salti |

`F12` su una funzione dichiarata in un header ti porta spesso alla **dichiarazione**
(nel `.h`). Premi di nuovo `F12` lì (o `Ctrl+F12`, "Go to Implementation") per andare al `.c`.

### Da dove viene un include: la tabella a mano

Se l'editor non aiuta, il prefisso dell'include dice dove guardare:

| Include | Cartella |
|---|---|
| `core/...`, `ui/...`, `services/...`, `apps/...`, `companion/...` | `watch-os-common/components/<nome>/include/<nome>/` |
| `hardware/...` (display, touch, pmu...) | dichiarazione in `watch-os-common/components/board_api/include/hardware/`, **implementazione** in `<firmware>/components/hardware/*.c` |
| `appkit.h`, `extapp_sdk.h` | `watch-os-common/components/appkit/include`, `.../extapp_sdk/include` |
| `lvgl.h`, `lv_*` | `<firmware>/managed_components/lvgl__lvgl/` (scaricato da ESP-IDF, vedi `idf_component.yml`) |
| `cJSON.h` | `<firmware>/managed_components/espressif__cjson/` |
| `esp_*.h`, `freertos/...`, `nvs*.h`, `driver/...` | ESP-IDF: `C:\esp\v6.1\esp-idf\components\` |
| `sdkconfig.h` (`CONFIG_...`) | generato: `<firmware>/build/config/sdkconfig.h` |

Regola dei nomi: l'header `components/core/include/core/power.h` è implementato in
`components/core/power.c`. Per i servizi c'è una sottocartella:
`services/include/services/wifi.h` → `services/wifi/wifi.c`.

Ultima risorsa, dal terminale:

```powershell
cd C:\Users\matte\Documents\ESP-IDF\watch-os-common
Select-String -Path components\*\*.c,components\*\*\*.c -Pattern "^\S.*\bpower_wake\s*\("   # la definizione (inizia a colonna 0)
```

Le funzioni `lv_...` sono di LVGL: documentazione su https://docs.lvgl.io/9.6/ . Le funzioni
`esp_...`, `xTask...`, `xSemaphore...`, `nvs_...` sono di ESP-IDF e FreeRTOS:
https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/ .

## 2. Come leggere il progetto (meglio che partire dal main)

Partire da `main.c` va bene **una volta**, per avere la mappa dell'avvio. Poi conviene
lavorare per "fili": si parte da una cosa che si vede e la si segue fino in fondo.

### Il metodo che uso io

1. **Parti da ciò che si vede.** Un testo sullo schermo ("Aggiornamento"), un messaggio
   di log (`power: Screen on`), una voce della pagina web. Cercalo con `Ctrl+Shift+F`.
   Le stringhe italiane sono nella UI, quelle inglesi nei log.
2. **Il log ti dice il file.** Ogni file ha `static const char *TAG = "power";` e i log
   escono come `I (1234) power: ...`. Cerca `TAG = "power"`.
3. **Leggi l'header prima del `.c`.** Gli `.h` di questo progetto hanno in cima un
   commento che spiega cosa fa il modulo, con esempi, e le regole (da quale task si può
   chiamare, se blocca). Spesso basta quello.
4. **Leggi il commento in cima al `.c`.** Spiega il *perché* delle scelte strane.
5. **Segui solo la catena che ti serve** con `F12` / `Shift+F12`, non tutto.
6. **Guardalo girare**: aggiungi temporaneamente un `ESP_LOGI(TAG, "qui x=%d", x);`,
   compila, flasha, guarda il log. È il modo più rapido per capire l'ordine reale.
7. **Chiedi a git perché è così**:
   ```powershell
   git log --oneline -- components/core/power.c        # storia di un file
   git log -S "set_mirror" --oneline                    # quando è comparsa una parola
   git blame components/core/power.c -L 200,240         # chi/quando per ogni riga
   ```

Esempio di "filo": *perché lo schermo si spegne dopo 30 secondi?*
`Ctrl+Shift+F` "screen_timeout" → `settings.h` (`screen_timeout_s`) → `power.c`
`power_task()` → `go_dim()` / `go_idle()` → `go_off()` → `display_sleep(true)` →
F12 → `<firmware>/components/hardware/display.c`.

### Cosa guardare quando apri un file `.c`

Tutti i file seguono lo stesso schema, dall'alto:

1. commento di intestazione (cosa fa, come);
2. `#include`;
3. `TAG` e `#define` (costanti: tempi, dimensioni);
4. tipi (`typedef struct`) e variabili `static s_...` (lo **stato** del modulo);
5. funzioni `static` (private, aiutanti);
6. funzioni pubbliche (quelle dichiarate nell'header), di solito in fondo.

Le variabili `s_...` dicono cosa ricorda il modulo; le `#define` dicono i numeri magici.
Leggi quelle prima delle funzioni.

## 3. Ciclo di vita del firmware (avvio)

Dal `main.c` di ogni firmware, in ordine:

```
sys_check_last_reset()   era un crash? (lo dirà con un toast)
sys_worker_init()        task "sys" per i lavori lenti
settings_init()          impostazioni da NVS
recovery_init()          modalità recovery?
board_init()             bus I2C, PMU, RTC, expander (hardware della scheda)
storage_init()           FAT interna /data, oppure microSD /sdcard
lv_port_init()           display + touch + task "lvgl" (in recovery: display virtuale)
clock_init()             ora
notify, battery, weather, sound, wifi_service_init()   servizi letti dalla UI
ui_init(); apps_init()   tema, app registrate, quadrante
power_init()             task "power": accende lo schermo, legge tasti e batteria
... time sync, Bluetooth, web (se serve)
fw_update_confirm_boot() ULTIMA: "questo firmware si avvia bene" (altrimenti rollback)
```

Poi `app_main()` termina: da lì in avanti tutto vive nei **task**, che dormono finché
non hanno qualcosa da fare (tocco, timer, rete).

## 4. Ciclo di vita di un'app interna

```
launcher ──app_open("timer")──►  [pause() dell'app sotto]
                                  nuovo screen ─► create(screen, arg) ─► animazione
utente: indietro ──────────────►  screen "ritirato" ─► animazione ─► resume() dell'app sotto
                                  ... poi, quando LVGL non lo usa più:
                                  lv_obj_delete(screen) ─► destroy()
```

Cose da sapere (da `core/app_manager.c`):

- **`create()`** costruisce i widget. **`destroy()`** arriva *dopo*, quando lo schermo viene
  davvero cancellato (fino ad allora widget e timer LVGL esistono ancora). In `destroy()`
  libera ciò che hai allocato tu (`malloc`, `esp_timer`, task) e metti a `NULL` i puntatori
  ai widget.
- **Una sola istanza** per app: aprire un'app già nella pila chiude prima quella vecchia
  (e quelle sopra). Per questo le app possono tenere i widget in variabili `static`.
- **Pila massima 8** schermi. Oltre: "Navigation stack full", l'apertura viene ignorata.
- **`pause()`/`resume()`**: chiamate quando un'altra app (o una sotto-pagina) copre la tua e
  quando torna visibile. Ferma lì animazioni e timer costosi.
- **Schermo spento** (timeout): l'app resta aperta ma LVGL smette di disegnare.
  Dopo **più di 2 minuti** spento, alla riaccensione si torna al quadrante e le app vengono
  chiuse, a meno di `APP_FLAG_STAY_ON_WAKE`.
- **`APP_FLAG_KEEP_SCREEN_ON`**: lo schermo non va in timeout finché l'app è aperta.
- Richieste di navigazione durante un'animazione vengono messe in coda (max 4).

Ciclo di vita di un'**app esterna**: è un firmware a parte. `extapp_sdk_init()` per prima
cosa rimette il launcher come avvio successivo; quindi l'app gira **una volta**: uscita,
crash, watchdog o batteria staccata riportano sempre al launcher.

## 5. Ciclo di vita dei servizi: le "prenotazioni"

Le risorse costose si accendono solo finché qualcuno le usa, con un **contatore**:

| Risorsa | Prendi | Rilascia | Si spegne quando |
|---|---|---|---|
| Wi-Fi | `wifi_acquire()` | `wifi_release()` | il contatore torna a 0 (circa 75 KB di RAM interna liberati) |
| Server web | `web_server_start()` (o `web_server_retain()`) | `web_server_stop()` | il contatore torna a 0 |
| Wi-Fi senza risparmio energetico | `wifi_set_fast(true)` | `wifi_set_fast(false)` | il contatore torna a 0 |
| CPU a 240 MHz | `power_cpu_boost(true)` | `power_cpu_boost(false)` | il contatore torna a 0 |
| Schermo sempre acceso | `power_keep_screen_on(true)` | `power_keep_screen_on(false)` | il contatore torna a 0 |

Regola: **ogni "prendi" ha il suo "rilascia"**, anche sui percorsi di errore. Un rilascio
dimenticato lascia il Wi-Fi acceso per sempre (batteria); uno di troppo lo spegne a
qualcun altro. Chiudere e fermare non deve mai allocare memoria: si usa `sys_post()`,
non un task nuovo.

## 6. La memoria

### I tipi di memoria dell'ESP32-S3

| Memoria | Quanta | Velocità | Chi la usa |
|---|---|---|---|
| **RAM interna** (DRAM) | ~300 KB, di cui ~50–90 KB liberi a regime | veloce | stack dei task, Wi-Fi, Bluetooth, TLS, buffer DMA del display |
| **PSRAM** | 8 MB | più lenta | widget e font LVGL, buffer grandi, schermo remoto, JSON |
| **Flash** | 16–32 MB | lettura | il firmware (2 slot), NVS, coredump, FAT `/data` |
| **NVS** (in flash) | 24 KB | lenta in scrittura | impostazioni, reti Wi-Fi, abbinamenti (chiave/valore) |
| **FAT `/data`** o **microSD `/sdcard`** | MB–GB | file | registrazioni, app esterne, configurazioni, cache |

La RAM interna è **la risorsa più scarsa**: quando finisce, il Wi-Fi o il Bluetooth non
partono, un task non viene creato, una connessione HTTPS fallisce.

### Dove finisce un'allocazione

```c
void *a = malloc(1000);     // < 2 KB  → RAM interna   (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048)
void *b = malloc(64000);    // ≥ 2 KB  → PSRAM, automaticamente
void *c = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);                    // PSRAM esplicita
void *d = heap_caps_malloc(4096, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);  // per il DMA: solo interna
static uint8_t big[100000] EXT_RAM_BSS_ATTR;   // array statico grande → PSRAM
lv_obj_t *label = lv_label_create(screen);     // LVGL → PSRAM (core/lv_mem_psram.c)
```

Le variabili locali di una funzione vanno nello **stack** del task (RAM interna, pochi KB):
mai array grandi come variabili locali. Esempio: lo stack del task `sys` è 6 KB
(`WORKER_STACK` in `core/sys.c`), quello di `lvgl` 12 KB (`TASK_STACK` in `core/lv_port.c`).

### Controllare la memoria

- Log di avvio e `health` ([pagina 8](08-console-e-diagnostica.md)): `internal free`,
  `largest` (il blocco più grande, conta più del totale), `min`, stack liberi.
- Nel codice: `sys_heap_log("dopo X");` stampa la stessa riga in quel punto.
- Leak: con heap tracing abilitato, `heapstart` → usa l'app → `heapstop` elenca le
  allocazioni non liberate.

### Dati che restano dopo il riavvio

| Dove | Come | Esempio |
|---|---|---|
| Impostazioni | `settings_get()` / `settings_save()` (NVS, scrittura ritardata ~2 s) | luminosità, timeout |
| Chiavi proprie | `nvs_open()` / `nvs_set_*()` / `nvs_commit()` | reti Wi-Fi in `wifi.c` |
| File | `storage_dir()` → `"/sdcard"` o `"/data"`; `storage_config_path()` per `config/<nome>` | memo vocali, configurazioni app |

Le scritture in flash bloccano la cache del chip per qualche millisecondo: non farle a
ogni tocco, e mai da callback di `esp_timer` (si passa da `sys_post()`).
