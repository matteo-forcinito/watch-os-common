# Pet di Orbi: piano tecnico dei prossimi sviluppi

Piano operativo, passo per passo, per chi riprende il lavoro dopo la 1.10.0.
Architettura e stato attuale: [ORBI-PETS.md](ORBI-PETS.md). Archivio e viaggi
via Orbit: [ORBI-CHILDREN.md](ORBI-CHILDREN.md). Genoma: [ORBI-GENOME.md](ORBI-GENOME.md).

Ogni fase si chiude solo quando **tutte** le sue verifiche passano. Non
saltare l'ordine: ogni fase usa quella precedente.

## Regole che non si discutono

1. **RAM interna.** Il Bluetooth ha bisogno di 40 KiB contigui all'avvio.
   Ogni variabile statica nuova più grande di 64 byte va in PSRAM con
   `EXT_RAM_BSS_ATTR` (se usata solo da task, mai da ISR). Ogni `malloc` di
   buffer va in PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`) o passa da
   LVGL (che è già in PSRAM). Dopo ogni fase controlla nel log di avvio:
   `sys: [ble start] ... largest >= 44 KB` su AMOLED 1.75C e Knob.
2. **Archivio.** Il blob `orbi_children/state` (`pet_store_t`) **non cambia
   formato**. Dati nuovi = blob nuovi con `magic` e versione propri (come
   `orbi_children/dex`). Un blob illeggibile si azzera, non blocca.
3. **Protocollo dei viaggi.** I passi `reserve → receive → depart → arrive →
   forget` (e `cancel`) restano identici per Orbit. ESP-NOW li **trasporta**,
   non li reinventa. Un pet non deve mai esistere attivo su due device.
4. **Fluidità.** Un solo `lv_timer` per pagina. Nessun task nuovo per la UI.
   Il tick non lavora se pagina nascosta, schermo spento o dito sullo schermo.
5. **Build.** Si compila da PowerShell, mai da Git Bash (idf.py esce con 0
   senza compilare). Ogni modifica a `watch-os-common` deve compilare su
   tutti e 6 i firmware (`release_*_manual.py` lo fa).
6. **Test sul PC prima del device.** Il modello (`pet_sprite.c`,
   `pet_store.c`, nuovi moduli puri) ha test host in `tools/`.

## Fase 1 - Viaggi ESP-NOW tra device (1.11.0)

Obiettivo: mandare un pet da un device a un altro vicino, senza Orbit e senza
Wi-Fi configurato.

### 1.1 Modulo `components/services/espnow_link/` (nuovo)

- API:
  ```c
  esp_err_t espnow_link_start(void);          /* idempotente */
  void espnow_link_stop(void);
  /* Invia un messaggio affidabile (ack + 3 ritrasmissioni, 300 ms). */
  esp_err_t espnow_link_send(const uint8_t mac[6], uint8_t type, const void *data, size_t len);
  typedef void (*espnow_link_rx_t)(const uint8_t mac[6], uint8_t type, const void *data, size_t len);
  void espnow_link_on_receive(espnow_link_rx_t cb);   /* chiamata dal task sys, non dall'ISR */
  int espnow_link_peers(espnow_peer_t *out, int max);  /* vicini visti negli ultimi 30 s */
  ```
- Frame (max 250 byte): `magic 'OP' (2) | versione (1) | tipo (1) | seq (2) |
  ack di seq (2) | len (1) | payload`.
- Tipi: `HELLO` (beacon ogni 5 s solo con la pagina pet aperta o un viaggio
  in corso: nome, seed nativo, specie), `PET_STEP` (un passo del viaggio, JSON
  compatto), `ACK`, `GAME` (fase 3).
- Canale: quello del Wi-Fi STA se connesso, altrimenti canale 1. Due device
  si vedono solo sullo stesso canale: documentarlo nella UI ("stessa rete o
  Wi-Fi spento su entrambi").
- Sicurezza: peer cifrati con LMK. PMK fissa di famiglia derivata dal token
  di accoppiamento Orbit (`sha256("orbit-pmk" | token)[0..16]`). Device mai
  accoppiati: solo `HELLO` in chiaro, nessun viaggio.
- Callback ESP-NOW (contesto Wi-Fi task): copiare il frame in una coda
  FreeRTOS da 8 elementi in PSRAM e processare nel task sys. **Mai** LVGL o
  NVS nella callback.
- Coesistenza: ESP-NOW usa la radio Wi-Fi; con Bluetooth attivo va bene
  (coex già attivo). Avviare ESP-NOW richiede `esp_wifi_start`: se il Wi-Fi è
  spento per risparmio, accenderlo solo per la durata del viaggio e
  rispegnerlo.

### 1.2 Viaggio via ESP-NOW (`pet_travel.c` in `apps/pet`)

- Macchina a stati **sulla sorgente**, salvata in NVS (`orbi_children/trip`,
  blob con magic) a ogni passo: `IDLE → RESERVED → SENT → DEPARTED → DONE`.
- Passi (riusano `pet_command`):
  1. sorgente: `reserve` (locale) → invia `PET_STEP{receive, snapshot}`;
  2. destinazione: `receive` (locale) → `ACK`;
  3. sorgente: `depart` (locale) → invia `PET_STEP{arrive}`;
  4. destinazione: `arrive` → `ACK` (parte l'animazione di entrata);
  5. sorgente: `forget` (locale).
- Riavvio a metà: all'avvio, se `trip` non è `IDLE`, riprova il passo
  corrente quando il peer torna visibile. Prima di `depart` si può annullare
  (`cancel` su entrambi). Dopo `depart` si va solo avanti.
- Identificativo viaggio: 16 caratteri hex casuali (`esp_random`), uguale al
  campo `transfer` di Orbit.
- UI: nel menu del pet "Manda da un amico vicino" → lista dei peer da
  `espnow_link_peers` → conferma → il pet esce dal portale. Errore leggibile
  se il peer sparisce: "Viaggio in pausa: riavvicina i device".

### 1.3 Verifiche fase 1

- Prima delle prove: **Impostazioni → Bluetooth → Ecosistema = Automatico**
  (oppure Background) su entrambi. Con "Spento fuori recovery" l'abbinamento
  può riuscire, ma il collegamento Famiglia resta spento e i peer sono offline.
  La lista e "Cerca di nuovo" devono indicare che Ecosistema è spento.
- Il controllo dei frame Famiglia deve accettare anche `PET` (7) e
  `PET_REPLY` (8): il test host del codec verifica tutti i tipi e le lunghezze.
- Test host: `tools/test_pet_travel.py` simula due store e un canale con
  perdita del 30%, duplicati e riavvio a ogni passo. Deve sempre finire con
  il pet attivo su **un solo** device.
- Device: AMOLED 1.75C ↔ Knob. Mandare, spegnere la sorgente dopo `depart`,
  riaccendere: il viaggio si completa da solo.
- Log avvio: blocco contiguo ≥ 44 KB invariato.

## Fase 2 - Ritorno a casa e visite (1.11.x)

Build di prova `1.12.0-dev.2`: dal menu del pet ospite è disponibile
**Torna a casa**. La ricerca interroga il seed nativo dei peer abbinati
(indipendente dal pet selezionato e dal nome del device), confronta la famiglia
e usa la transazione di viaggio esistente. Aprire la pagina pet sul device di
origine; Ecosistema deve essere attivo su entrambi, con firmware aggiornato.
Se casa non risponde, il pet rimane sul device ospite e si può riprovare.
Il ritorno automatico differito e le visite a tempo restano da implementare.

- **Torna a casa**: sul device ospite, menu → "Torna a casa". Il device
  nativo è riconosciuto dal campo `family` del seed (bit 32-55) confrontato
  con il seed nativo dei peer in `HELLO`. Se il nativo è vicino: viaggio
  ESP-NOW. Altrimenti il pet resta con lo stato `homesick` (blob
  `orbi_children/plans`) e parte appena il nativo compare, o quando Orbit
  lo trasferisce.
- **Visita**: viaggio con `return_at` (es. 2 h). Allo scadere la
  destinazione avvia il ritorno da sola. Al rientro il pet porta un
  **ricordo**: una razza scoperta in più (quella dei pet incontrati) e un
  oggetto (fase 4).
- Verifiche: test host con orologio simulato (visita scaduta con device
  spento, ritorno al riavvio); sul device, visita di 2 minuti in debug.

## Fase 3 - Minigiochi (1.12.0)

Implementati: **Fuga sulla neve**, **Pioggia di stelle** e **Tocca la stella**.
Menu pet > Minigiochi. Il pet selezionato diventa il giocatore, con i propri
colori e la propria razza. Il pet deve essere sveglio.

- Colline: pressione in discesa, rilascio sulla cresta, volo dalla tangente,
  velocita proiettata sulla pendenza all'atterraggio; fuga dalla valanga per
  60 secondi. Il terreno usa coordinate subpixel.
- Stelle: tre corsie, stelle da prendere e rocce da evitare; tre vite,
  massimo 60 secondi. Tocco sinistra/destra o rotazione del Knob.
- Tocca la stella: esempio completo per chi inizia; 20 secondi per colpire
  bersagli. Touch oppure corsia del pet selezionata col Knob e clic.
- Esci annulla senza premio. Riprova azzera lo stato senza nuove allocazioni.
- Risultato intero 0-100, richiesta premio una sola volta. XP e gioia vengono
  salvati dal worker di sistema; il gioco non scrive NVS dal timer grafico.

Tutti usano `components/apps/minigames`: ciclo a 33 ms, integrazione a passi
fissi da 16 ms sul tempo reale, pausa esplicita a schermo spento o app nascosta,
input normalizzato, stato e framebuffer in PSRAM, disegno RGB565_SWAPPED
con clipping, rilascio automatico di timer, callback e buffer alla chiusura.
I renderer delle colline e delle stelle sono adattatori del medesimo host:
non creano widget o timer propri. Il catalogo guida menu e validazione premi.

[Guida per creare un gioco](guida/14-minigiochi.md): il comando
`python tools/new_minigame.py nome --title "Titolo"` crea e registra una copia
dell'esempio. Test motore, pausa, Riprova, risultati, clipping, tutti i renderer
su tutte le dimensioni e generatore: `python tools/test_minigames.py`.
Regole/fisica: `python tools/test_pet_game.py`; store/viaggi/premi:
`python tools/test_pet_store.py`.

Misure precedenti all'estrazione del motore, build dev.2: circa 29 e 30
aggiornamenti/s su AMOLED 1.75C e Knob, refresh medio 20,7 e 18,1 ms,
logica/raster 8,4 e 5,0 ms. I log del nuovo host riportano gli stessi dati
per verificare regressioni; non sono una garanzia di frequenza costante in
ogni scena o durante screenshot.

Restano futuri: Acchiapparella con movimento libero, Memory dei versi,
minigiochi in coppia via ESP-NOW, inventario e visite con ritorno automatico.
Il ritorno manuale a casa e disponibile dalla fase 2; il device d'origine
deve essere abbinato, vicino e raggiungibile.

## Fase 4 - Oggetti e passatempo (1.12.x)

- Inventario (`orbi_children/items`, 32 oggetti, 1 byte ciascuno): ciotola,
  cuccia, palla, fiore, conchiglia (dai viaggi), cappellino.
- Gli oggetti si posano sul prato (sprite 16x16 procedurali come le icone) e
  il pet li usa: dorme nella cuccia, mangia dalla ciotola.
- Eventi a sorpresa a bassa frequenza (max 1 ogni 2 ore): una farfalla da
  inseguire, un regalo dopo 7 giorni di cure, il compleanno del pet.
- Suoni brevi sui device con speaker (abbaio, miagolio, chioccia) generati a
  toni, volume dalle impostazioni, mai di notte.

## Fase 5 - Companion 0.6

- Porting Dart di `pet_sprite.c` in `lib/ecosystem/pet_sprite.dart`, con
  vettori: `tools/pet_sprite_preview.py --vectors` scrive l'hash FNV-1a di
  ogni griglia (specie × crescita × posa × fotogramma); il test Dart deve
  riprodurli tutti.
- Children: stessi animali, razze scoperte unite tra device, avvio dei viaggi
  ESP-NOW da telefono come alternativa.

## Checklist di rilascio (ogni fase)

1. `python tools/test_pet_store.py` e gli altri test host: tutti OK.
2. `python tools/pet_sprite_preview.py`: controllare l'anteprima PNG.
3. Commit in `watch-os-common`, push su `main`.
4. Copiare l'ultimo `.release-work/release_*_manual.py`, cambiare versione e
   note, lanciarlo **da PowerShell** (prepara, compila i 6 firmware, verifica).
5. Flash su AMOLED 1.75C (COM10) e Knob (COM9: il reset USB lo riporta in
   download mode, avviarlo con `--after watchdog-reset`). Controllare il log:
   versione, `[ble start] largest >= 44 KB`, `pet: scene ready`.
6. Prova manuale della pagina pet e della funzione nuova.
7. `release_*_manual.py --publish`, poi controllare il catalogo OTA di ogni
   prodotto (versione nuova in `channels.stable`).
