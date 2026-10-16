# 3. C, ESP-IDF e LVGL in breve

Il minimo per leggere il codice e fare modifiche senza paura.

## Il C che trovi nel progetto

```c
#include "core/app.h"        // "importa" le dichiarazioni di un altro file

static int s_count = 0;      // variabile globale visibile solo in questo file (prefisso s_)
static const char *TAG = "timer";   // etichetta per i log di questo file

/* Una funzione: tipo di ritorno, nome, parametri. */
static int doppio(int x)
{
    return x * 2;
}

void esempio(void)
{
    char testo[32];                                   // array di 32 caratteri (stringa)
    snprintf(testo, sizeof(testo), "Valore %d", doppio(s_count));   // scrive senza sforare
    ESP_LOGI(TAG, "%s", testo);                       // log: I (1234) timer: Valore 0
}
```

Concetti chiave:

- **`.h` e `.c`**: l'header (`.h`) dichiara cosa esiste, il `.c` lo implementa.
- **`static`** davanti a una funzione o variabile = privata del file.
- **Puntatori** (`lv_obj_t *label`): un "riferimento" a un oggetto. `NULL` = nessuno.
- **Memoria**: `malloc`/`calloc` chiedono memoria, `free` la restituisce. Se dimentichi
  `free`, la memoria si perde (leak); se la usi dopo `free`, crash.
- **`struct`**: un gruppo di campi, ad esempio `settings_t` con tutte le impostazioni.
- **`esp_err_t`**: quasi tutte le funzioni ESP-IDF restituiscono `ESP_OK` o un codice d'errore.
  Si stampa con `esp_err_to_name(err)`.

Stile del progetto: graffe su riga propria (Allman), 4 spazi, nomi `snake_case`,
statiche con `s_`, commenti in inglese. La documentazione per persone è in italiano.

## ESP-IDF e FreeRTOS

ESP-IDF è il "kit" di Espressif: driver, Wi-Fi, Bluetooth, HTTP, flash. Dentro c'è
**FreeRTOS**, un sistema che fa girare più **task** (attività) in parallelo sui due core
dell'ESP32-S3.

Task importanti nel nostro firmware:

| Task | Cosa fa |
|---|---|
| `lvgl` | Disegna lo schermo, gestisce touch e app. **Unico che può toccare LVGL.** |
| `power` | Accende/spegne lo schermo, legge batteria e tasti |
| `sys` | Il "lavoratore" di sistema: esegue lavori lenti (scritture flash, Wi-Fi on/off) |
| `httpd` | Il server web (un solo task: una richiesta alla volta) |
| `nimble_host` | Bluetooth |

Regole che ne derivano:

- Da un task diverso da `lvgl`, per cambiare la UI: `ui_async(funzione, argomento)`.
- Mai bloccare il task `lvgl` (rete, attese lunghe): avvia un task o usa `sys_post()`.
- Le callback di `esp_timer` non fanno lavoro vero: lo passano a `sys_post()`.
- Nuovi task con `sys_task_create()` (registra eventuali fallimenti).

La **RAM interna** (circa 300 KB) è scarsa e serve a Wi-Fi/Bluetooth. La **PSRAM** (8 MB)
è abbondante: LVGL e i buffer grandi vanno lì (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`,
oppure `EXT_RAM_BSS_ATTR` per gli array statici grandi).

## I componenti ESP-IDF

Un **componente** è una cartella con un `CMakeLists.txt` che dichiara i suoi sorgenti:

```cmake
idf_component_register(
    SRCS "timer/timer_app.c" "weather/weather_app.c"   # i file .c da compilare
    INCLUDE_DIRS "include"                             # dove stanno gli header pubblici
    REQUIRES core ui services                          # altri componenti usati
)
```

**Se crei un nuovo file `.c`, aggiungilo a `SRCS`**, altrimenti non viene compilato.

## LVGL, la grafica

LVGL crea **oggetti** (label, bottoni, contenitori) dentro uno **schermo**.

```c
lv_obj_t *label = lv_label_create(screen);   // crea un testo dentro screen
lv_label_set_text(label, "Ciao");            // imposta il testo
lv_obj_center(label);                        // lo centra
```

Nel progetto si usano quasi sempre i widget già pronti di `ui/ui.h`, che rispettano
il tema e gli schermi tondi:

```c
lv_obj_t *page = ui_page(screen, "Titolo");          // pagina scorrevole con titolo
ui_text(page, "Una spiegazione", true);              // testo (true = grigio)
lv_obj_t *row = ui_row(page, LV_SYMBOL_BELL, UI_COLOR_BLUE, "Sveglia", "07:00", on_click, NULL);
lv_label_set_text(ui_row_value(row), "08:00");       // aggiorna il valore a destra
ui_button(page, "Salva", UI_COLOR_GREEN, on_save, NULL);
ui_toast("Salvato");                                 // messaggio temporaneo
```

Sui pannelli tondi usa sempre `ui_page()`, `ui_title()` e `ui_text()`: centrano il
contenuto. Mai testo allineato a sinistra vicino al bordo superiore.

### Eventi (tocchi)

```c
static void on_click(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target_obj(e);   // chi è stato toccato
    lv_label_set_text(ui_row_value(row), "Toccato!");
}
```

### Stato condiviso: i servizi pubblicano, la UI osserva

```c
// in un servizio (qualsiasi task, non blocca mai):
state_set(STATE_BATTERY, 87);

// nella UI: la label si aggiorna da sola, e l'osservatore sparisce con la label
lv_label_bind_text(label, state_subject(STATE_BATTERY), "%d%%");
```

### Impostazioni

```c
settings_t s = *settings_get();   // copia le impostazioni correnti
s.brightness = 200;               // cambia
settings_save(&s);                // applicate subito, scritte in flash dopo ~2 s
```

Un campo nuovo va aggiunto **in fondo** a `settings_t` (in `core/settings.h`), con il
suo valore iniziale in `settings.c`. Così le impostazioni salvate dalle versioni
precedenti restano valide.
