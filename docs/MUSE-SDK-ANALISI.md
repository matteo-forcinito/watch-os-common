# Meta Muse Gadget SDK: cosa possiamo usare per i nostri device

Analisi del 27 ottobre 2026 su `facebookincubator/muse-gadget-sdk`, commit
`729d0c8` (26/10/2026), licenza Apache 2.0. Firmware ESP-IDF **6.0.1**
(noi siamo su 6.1). Supporta ufficialmente quattro nostre schede: Waveshare
AMOLED 1.75 e 1.75C, SenseCAP Watcher e e-paper 1.54.

Il firmware Muse in sé non ci serve: richiede token SDK, app Muse e il loro
cloud, e sostituirebbe il nostro OS. Utili sono le scelte di sistema e le
misure di consumo, documentate con una cura che vale la pena sfruttare.

## Cosa facciamo già come Muse

Allocatore LVGL in PSRAM (`core/lv_mem_psram.c`, identico al loro
`muse_lv_mem.c`), memoria NimBLE in PSRAM, stack in PSRAM consentiti,
tickless idle, modem sleep BT, spegnimento CPU in light sleep, mbedTLS in PSRAM.
Nessun intervento necessario su questi punti.

## Differenze, in ordine di valore

### 1. Interrupt SPI del display sul core sbagliato (affidabilità) — da verificare

Muse documenta (`boards/muse_lcd_bands.h`) un difetto del lock del bus SPI di
ESP-IDF: se l'interrupt di fine trasferimento gira su un core diverso dal task
che aspetta il bus, il risveglio può andare perso e il task resta bloccato con
lo schermo disegnato a metà. L'interrupt si installa sul core del task che
inizializza il bus.

Da noi: `board_init()` (e quindi `spi_bus_initialize` del pannello) gira in
`app_main`, sul core 0; il task `lvgl` è fissato al core 1 (`lv_port.c`,
`TASK_CORE 1`). È esattamente la combinazione descritta. `flush_wait_cb` ha già
un timeout di 100 ms e conta `s_flush_timeouts`: un risveglio perso diventa
uno scatto, ma un blocco dentro il driver SPI diventerebbe un reset del watchdog.
Il blocco al primo avvio sulle AMOLED rotonde (corretto in 1.17.0 limitando gli
archi) potrebbe avere anche questa concausa.

**Proposta:** inizializzare il bus del display da un task breve fissato al core 1
(oppure installare l'interrupt con affinità esplicita), poi confrontare
`flush_timeouts` prima/dopo su 1.75C e AMOLED 1.8. Costo basso, rischio basso.

### 2. Codec del Watcher lasciati accesi (consumo) — misurato da Meta

Misure PPK2 su Watcher (`docs/power/sensecap-watcher*.md`):

- i codec restano alimentati anche dopo reflash o reset del solo ESP32: un
  firmware precedente che li lascia aperti costa **~33 mA** finché non si
  spegne del tutto la scheda;
- inizializzare il codec senza aprirlo/chiuderlo costa **+11,9 mA** (accende
  l'ADC ES7243E); sospeso esplicitamente consuma 0,7 mA meno del default;
- l'isolamento GPIO in sleep peggiora (+0,1 mA): da lasciare disattivato;
- light sleep con CPU spenta: −15%, l'unica leva software grande (già attiva).

Da noi il Watcher apre ES8311 solo su richiesta e lo chiude, ma non tocca mai
l'ADC ES7243E e non sospende i codec all'avvio. **Proposta:** all'avvio mettere
esplicitamente in standby entrambi i codec. Pochi registri I2C, nessun effetto
sull'audio; da verificare con le stesse misure.

### 3. Codice eseguito dalla PSRAM (fluidità durante scritture flash)

Muse abilita su 1.75C `SPIRAM_FETCH_INSTRUCTIONS`, `SPIRAM_RODATA`,
`SPIRAM_XIP_FROM_PSRAM`. Con codice e costanti copiati in PSRAM, le scritture
in flash (OTA, NVS, coredump) non sospendono più la cache: oggi durante l'OTA
mostriamo una schermata ferma proprio per questo (vedi `docs/RELEASE.md`).
Costo: PSRAM pari alla dimensione dell'immagine (alcuni MB su 8) e banda PSRAM
condivisa con le immagini LVGL. **Proposta:** prova A/B su 1.75C (32 MB flash,
8 MB PSRAM) misurando RAM libera, fluidità e durata dell'OTA.

### 4. Cache più grandi

Muse: instruction cache 32 KB, data cache 64 KB con linee da 64 B.
Noi: 16 KB / 32 KB / 32 B. Su S3 la data cache più grande aiuta molto il
rendering da PSRAM; costa RAM interna (circa 48 KB in meno). Da misurare con
Wi-Fi attivo, perché la RAM interna è il nostro collo di bottiglia.

### 5. Rendering a bande alte e su due core

- Noi disegnamo in bande da 16 righe in RAM interna DMA (~29 bande per uno
  schermo 466×466). LVGL percorre l'albero dei widget **per ogni banda**.
  Muse disegna 4 bande alte in PSRAM e un task copia poche righe alla volta in
  due piccoli buffer interni mentre l'altro è in trasmissione. Effetto: swipe
  che tengono il passo del dito, con meno RAM interna di oggi.
- `LV_DRAW_SW_DRAW_UNIT_CNT=2` (noi 1): rendering su entrambi i core.
- TE (tearing effect): accendiamo il segnale sul pannello 1.75C (`0x35`) ma non
  lo usiamo per sincronizzare l'invio; Muse lo fa e logga le bande "in ritardo".

È la modifica più grande (nuovo percorso di flush in `lv_port.c`), da fare dopo
1–3 e con le misure di `progress-files/performance` come base.

## Ordine proposto

1. Affinità interrupt SPI del display (1) — piccola, utile su tutte le schede.
2. Standby codec all'avvio sul Watcher (2).
3. Prova XIP da PSRAM e cache su 1.75C (3, 4), solo con misure A/B.
4. Flush a bande alte + due draw unit + TE (5).

Nessuna di queste modifiche è nella release 1.17.0.
