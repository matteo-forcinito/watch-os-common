# 4. App interne: crearle e modificarle

Un'**app interna** è compilata dentro il firmware: si apre all'istante dal launcher e
condivide RAM e servizi con il sistema. Esempi: Timer, Sveglia, Meteo, Impostazioni.

Tre tipi di app esistono nel progetto:

| Tipo | Dove sta il codice | Quando sceglierlo |
|---|---|---|
| **Interna** | `watch-os-common/components/apps/<nome>/` | Funzione leggera e utile su tutti i dispositivi |
| **Interna di una sola scheda** | `<firmware>/components/<nome>/`, registrata in `main.c` | Usa hardware che hanno solo alcune schede (es. altoparlante Bluetooth del Knob) |
| **Esterna** | `<firmware>/external_apps/<nome>/` (+ codice in `watch-os-common/<nome>/`) | Pesante (radio, tastiera HID) o da installare a parte: vedi [pagina 5](05-app-esterne.md) |

Le app "hosted" (Pieno, Giornata, Notizie, Copertura, Ombra, Sentinel) sono un
ibrido: lo stesso codice diventa app interna (con `CONFIG_APPKIT_HOSTED`) **e** app
esterna. Vedi [pagina 5](05-app-esterne.md).

## Come funziona un'app

Un'app è una descrizione costante (`app_t`) con alcune funzioni:

- `create(screen, arg)` — costruisci la UI dentro `screen`. Chiamata all'apertura.
- `destroy()` — libera la tua memoria (timer, buffer). I widget li cancella il sistema.
- facoltative: `resume`, `pause`, `back`, `button`.

Il sistema (app manager) crea lo schermo, lo fa scorrere, gestisce "indietro"
(swipe dal bordo sinistro, tasto). Tutte le funzioni girano nel task `lvgl`.

## Esempio completo: app "Contatore"

### 1. Crea il file `watch-os-common/components/apps/counter/counter_app.c`

```c
#include "apps_internal.h"

#include "core/settings.h"
#include "ui/ui.h"

/*
 * Counter: a row that counts taps and a button that resets it.
 * Minimal example of an internal app.
 */

static int s_count = 0;
static lv_obj_t *s_row = NULL;   /* the widget we update */

static void show(void)
{
    lv_label_set_text_fmt(ui_row_value(s_row), "%d", s_count);
}

static void on_tap(lv_event_t *e)
{
    (void)e;
    s_count++;
    show();
}

static void on_reset(lv_event_t *e)
{
    (void)e;
    s_count = 0;
    show();
    ui_toast("Azzerato");
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    lv_obj_t *page = ui_page(screen, "Contatore");
    ui_text(page, "Tocca la riga per contare.", true);
    s_row = ui_row(page, LV_SYMBOL_PLUS, UI_COLOR_GREEN, "Tocchi", "0", on_tap, NULL);
    ui_button(page, "Azzera", UI_COLOR_RED, on_reset, NULL);
    show();
}

static void destroy(void)
{
    s_row = NULL;   /* the widget is deleted by the app manager: forget it */
}

const app_t counter_app = {
    .id = "counter",
    .name = "Contatore",
    .icon = LV_SYMBOL_PLUS,
    .color = UI_COLOR_GREEN,
    .create = create,
    .destroy = destroy,
    .about = "Conta i tocchi. Esempio di app interna.",
};
```

### 2. Dichiara l'app in `components/apps/apps_internal.h`

```c
extern const app_t counter_app;
```

### 3. Aggiungila all'elenco in `components/apps/apps.c`

```c
static const app_t *const s_apps[] = {
    &alarms_app,
    &weather_app,
    &timer_app,
    &counter_app,     /* ← nuova: l'ordine è quello del launcher */
    ...
```

### 4. Aggiungi il file al `components/apps/CMakeLists.txt`

```cmake
        "timer/timer_app.c"
        "counter/counter_app.c"     # ← nuovo
```

### 5. Compila in tutti i firmware, prova su un dispositivo

```powershell
$env:WATCH_OS_COMMON_DIR = "C:/Users/matte/Documents/ESP-IDF/watch-os-common"
cd ..\ESP32-S3-Touch-AMOLED-1.75C
idf.py build
idf.py -p COM10 app-flash
```

Con la console di sviluppo attiva ([pagina 8](08-console-e-diagnostica.md)) puoi aprirla e
fotografarla dal PC:

```powershell
python tools\devcon.py COM10 "open counter" "shot:counter.png"
```

Controlla lo screenshot su **tutte le forme** di schermo (tondo 360/412/466 e
rettangolare 368x448): niente testo tagliato dal cerchio.

## Sotto-pagine

Una sotto-pagina è un'app nascosta, aperta con un argomento:

```c
const app_t counter_detail_app = {
    .id = "counter.detail", .name = "Dettaglio", .flags = APP_FLAG_HIDDEN,
    .create = detail_create,
};

/* dalla pagina principale */
app_open("counter.detail", NULL);
```

L'argomento passato ad `app_open()` deve restare valido (l'apertura può essere messa
in coda): passa memoria allocata con `malloc` e liberala in `create()`.

## Lavori lenti (rete, file): mai nella UI

```c
static void download_job(void *arg)       /* gira nel task sys, può attendere */
{
    int *result = malloc(sizeof(int));
    *result = do_slow_download();
    ui_async(show_result, result);        /* torna nella UI per mostrarlo */
}

static void show_result(void *arg)        /* gira nel task lvgl */
{
    int *result = arg;
    if (s_row != NULL) lv_label_set_text_fmt(ui_row_value(s_row), "%d", *result);
    free(result);
}

static void on_tap(lv_event_t *e)
{
    sys_post(download_job, NULL);         /* non blocca la UI */
}
```

Nota: quando torna `show_result`, l'app potrebbe essere già chiusa. Per questo si
controlla `s_row != NULL` (messo a `NULL` in `destroy`).

## Modificare un'app esistente

1. Trova il file: l'`id` è nell'`app_t`. Cerca il nome mostrato nel launcher:
   ```powershell
   Select-String -Path ..\watch-os-common\components\apps\*\*.c -Pattern '"Timer"'
   ```
2. Leggi l'header di commento in cima al file: spiega cosa fa.
3. Modifica, compila **tutti** i firmware, prova sul dispositivo con screenshot.
4. Se cambi la **dashboard web** (`components/services/web/index.html` o le
   `*_dashboard.html`), controlla la sintassi JS (un errore svuota tutte le pagine):
   ```powershell
   node ..\watch-os-common\tools\test_remote_frame.cjs
   ```
   e per un controllo generico estrai lo script ed esegui `node --check`
   (vedi [pagina 7](07-test.md)).

## App di una sola scheda

Esempio reale: l'altoparlante Bluetooth del Knob (`ESP32-S3-Knob-Touch-LCD-1.8/components/bt_speaker`).
Si registra in `main/main.c` del firmware **prima** di `apps_init()`:

```c
app_register(&speaker_app);   /* this board's own app, first in the launcher */
apps_init();
```

## Errori tipici

| Sintomo | Causa probabile |
|---|---|
| Crash a caso aprendo/chiudendo l'app | UI modificata da un altro task; usa `ui_async()` |
| La UI si blocca qualche secondo | Rete o attesa in un callback UI; usa `sys_post()` o un task |
| `undefined reference to counter_app` | File non aggiunto a `SRCS` nel `CMakeLists.txt` |
| Testo tagliato sugli schermi tondi | Usa `ui_page/ui_title/ui_text`, non posizioni fisse |
| Sulla rotella (Knob/Watcher) un elemento non si seleziona | Oggetto cliccabile personalizzato: `lv_port_set_focusable(obj)` |
