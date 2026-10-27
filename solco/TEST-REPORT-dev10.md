# Solco 0.1.0-dev.10 - AMOLED 1.8 landscape

27 ottobre 2026. Build locale e installazione USB eseguite; nessuna release
pubblica o installazione sugli altri device.

## Cambiamento

Geometria UI estratta in main/layout.h. Mixer a due colonne; pad 4x2; platter
circolare intero e trasporti negli angoli. Font 16 nella vista wide. Dimensioni
UI lette dal display logico, non dalle dimensioni fisiche native.
API core lv_port_init_orientation: orientamento dell'app indipendente dalla
preferenza persistente del launcher. Il secondo buffer esistente diventa
uscita DMA ruotata, mentre LVGL usa un solo buffer: nessun terzo buffer RAM.
Motore audio, sintesi, WSOLA, loop e formati file non modificati.

## Evidenza

- Test host C: sei geometrie, deck e pad senza sovrapposizioni, disco entro i bordi.
- Build ESP-IDF completa: 1337 passaggi, exit 0; progetto solco_amoled.
- Binario 1495136 byte, SHA-256
  589fa4676d59d14ec1a6c29cb1ab809bc8220c69a1a29e651a98d35fb4727ea7.
- Device COM3, MAC confrontato con la configurazione privata. Backup fresco
  dei metadati e dei 6 MiB dello slot secondario prima della scrittura.
- Installer: confronto partizioni salvate/live, verifica identita, scrittura
  solo ota_1 a 0x620000 e selezione OTA. Bootloader, tabella, firmware ota_0
  e partizione dati/NVS non riscritti dall'installer. L'app registra la propria
  voce NVS e la copia /apps/Solco sulla SD; stato launcher=1 osservato.
- Console sul device: versione dev.10, dimensioni 448x368, landscape=1.
- Prova tramite touch sintetici USB: Studio aperto dal tasto, tutti gli otto
  pad (8 hit aggiuntivi), deck A/B, PLAY/PAUSA, menu/vinile, ritorno mixer,
  apertura e chiusura libreria. PASS. Nessuna registrazione/pattern cancellata.
- Heap integro, RAM interna libera 170555 byte, blocco massimo 64512 byte,
  minimo storico 89268 byte; PSRAM libera circa 4.2 MB.
- Volume rimasto 0, playback finale 0/0; underrun=0, errori audio=0, clip=0.
  Dopo acquisizioni: ciclo massimo 12004 us, late=0. Non e un test audio
  professionale o uno stress prolungato, ne una verifica acustica dello speaker.
- Screenshot completi del device mixer, pad e vinile acquisiti e ispezionati:
  progress-files/solco-amoled-dev10-{mixer,pads,vinyl}.png (workspace privato).
  Rapporto touch: progress-files/solco-amoled-dev10-acceptance.json.

Il touchscreen fisico con il dito richiede ancora conferma dell'utente; i
comandi sintetici percorrono gli eventi LVGL, non il sensore touch fisico.
Il launcher principale non e stato aggiornato. Pet/Petland e relativa
regressione restano un'attivita separata, rinviata su richiesta dell'utente.
