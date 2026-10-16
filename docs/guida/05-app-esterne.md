# 5. App esterne e Market

## Cos'è un'app esterna

Un'**app esterna** è un firmware completo a sé, salvato su microSD in
`/sdcard/apps/<Nome>/<Nome>.bin`. Il launcher la scrive nel **secondo slot** della flash
e riavvia dentro di lei. Quando esce (o crasha) si torna sempre al launcher.

```
flash:  [ ota_0: Watch OS (launcher) ] [ ota_1: app esterna di turno ]
```

Vantaggi: ha tutta la RAM per sé (radio, tastiera HID, giochi). Svantaggi: avvio di
qualche secondo, serve una microSD, e va compilata **per ogni scheda**.

App esterne presenti: `external_apps/` di ogni firmware (Remote, WebRadio, Orbit, Pieno,
Giornata, Copertura, Ombra, Sentinel, Casa, Cielo, Deck, Radar, Vault, Anello...).

## Esempio minimo: `examples/hello_app` (AMOLED 1.8)

```
examples/hello_app/
├─ CMakeLists.txt      ← riusa hardware della scheda + watch-os-common
├─ main/main.c         ← app_main() dell'app
├─ partitions.csv
└─ sdkconfig.defaults
```

`CMakeLists.txt`: prende i componenti ma **esclude** ciò che non serve:

```cmake
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components"   # hardware di questa scheda
    "${WATCH_OS_COMMON}/components")                # sistema condiviso
set(EXCLUDE_COMPONENTS "apps" "services" "companion" "webradio" "hid_link")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hello_app)
```

`main/main.c` (semplificato):

```c
void app_main(void)
{
    extapp_sdk_init();      /* PRIMA COSA: il prossimo riavvio torna al launcher */
    sys_worker_init();
    settings_init();
    board_init();
    lv_port_init();

    lv_port_lock();
    ui_init();
    lv_obj_t *home = lv_obj_create(NULL);
    lv_obj_t *page = ui_page(home, "Hello!");
    ui_row(page, LV_SYMBOL_PLUS, UI_COLOR_GREEN, "Tocca qui", "0", on_tap, NULL);
    ui_button(page, "Torna al launcher", UI_COLOR_BLUE,
              (lv_event_cb_t)extapp_return_to_launcher, NULL);
    app_manager_set_home(home, home_back);   /* "indietro" sulla home = esci */
    lv_port_unlock();

    app_manager_init();
    power_init();
}
```

### Creare una nuova app esterna

1. Copia `ESP32-S3-Amoled-Touch-1.8/examples/hello_app` in `external_apps/<miaapp>`.
2. Cambia `project(hello_app)` → `project(miaapp)` e il testo in `main.c`.
3. Aggiungi `manifest.json` e un `icon.png` (96x96, come le altre app), come in `external_apps/remote`:
   ```json
   {"name":"MiaApp","bin":"MiaApp.bin","version":"1.0.0"}
   ```
4. Compila:
   ```powershell
   cd external_apps\miaapp
   idf.py set-target esp32s3
   idf.py build
   ```
5. Copia su microSD: `build\miaapp.bin` → `/sdcard/apps/MiaApp/MiaApp.bin`, più
   `manifest.json` e `icon.png` nella stessa cartella. Apri da "App esterne".
   Senza togliere la SD: app **Unità USB** sull'orologio (schede con USB OTG) oppure la
   pagina web → File.
6. Ripeti la build negli altri firmware che devono averla (ogni scheda ha il suo
   `components/hardware`).

Se il codice deve essere comune a tutte le schede, mettilo in `watch-os-common/<miaapp>/main/`
e nei vari `external_apps/<miaapp>/main/CMakeLists.txt` puntalo da lì, come fa Pieno:

```cmake
set(PIENO_SHARED "${WATCH_OS_COMMON}/pieno/main")
idf_component_register(
    SRCS "${PIENO_SHARED}/main.c" "${PIENO_SHARED}/feed.c" "${PIENO_SHARED}/fuel.c" ...
    INCLUDE_DIRS "${PIENO_SHARED}"
    PRIV_REQUIRES hardware core ui extapp_sdk appkit services ...
    EMBED_TXTFILES "${PIENO_SHARED}/pieno_dashboard.html")
```

## App "hosted": la stessa app interna ed esterna

Pieno, Giornata, Notizie, Copertura, Ombra e Sentinel hanno il codice in
`watch-os-common/<app>/main`:

- `main.c` — usato solo quando è **app esterna** (contiene `app_main`).
- `app.c` — l'`app_t`, usato quando è **app interna** (con `CONFIG_APPKIT_HOSTED=y`).
- `ui.c`, `web.c`, logica (`fuel.c`...) — comuni ai due casi.
- `<app>_dashboard.html` — la pagina web dell'app, servita dall'orologio su `/<app>/`.

La parte comune di dashboard e sveglie la dà `components/appkit` (`appkit.h`):
`appkit_web_start()` avvia Wi-Fi e pagina, `appkit_alarms_prepare/start()` mantiene
le sveglie attive anche dentro l'app.

Le app interne hosted lanciano i lavori in background solo se ci sono almeno 115 KB
di RAM interna libera.

## Il Market

Il Market sull'orologio legge un catalogo pubblico:

```
https://raw.githubusercontent.com/matteo-forcinito/watch-firmware-releases/main/market/catalog.json
```

e scarica binari e icone dalle release GitHub, verificando dimensione e SHA-256
(installazione su microSD).

Pubblicazione (strumenti nel repo Watcher, documentati in
`SenseCap-Watcher-W1-A/docs/PERSONAL-MARKETPLACE.md`):

```powershell
cd C:\Users\matte\Documents\ESP-IDF\SenseCap-Watcher-W1-A
python tools/build_market_catalog.py apps.json output-market --base-url https://github.com/matteo-forcinito/watch-firmware-releases/releases/download/apps-v1/
python tools/test_market_catalog.py
python tools/publish_market.py output-market --tag apps-v1
```

`apps.json` elenca un pacchetto **per scheda** (`board` = nome progetto del firmware:
`amoled_watch`, `amoled175_watch`, `knob_os`, `watcher_os`...).

Inoltre la release del firmware AMOLED 1.8 compila e impacchetta già Remote, Orbit e
WebRadio come zip negli artifact della GitHub Action ([pagina 10](10-automazioni-github.md)).
