# ORBIT ESP-NOW — checkpoint operativo

Aggiornamento: 7 novembre 2026. Nessun flash o pubblicazione autorizzati.
Preservare le modifiche preesistenti, inclusa `apps/market/market_app.c`.

## Checklist effettiva

| Attività | Stato | Evidenza / prossimo passo |
|---|---|---|
| Fase 1: misure radio | Completato e verificato | Tabelle storiche in ORBIT-ESPNOW.md |
| Fase 1b: buffer Wi-Fi e ordine BLE | Parzialmente verificato | 1.8 e 1.75C provate; Watcher da rimisurare |
| Fase 2: bus unico EC01/EL02 | Completato e verificato software | Matrice OS6 superata; attribuzione TX e maschere legacy host pass; hardware1.8/1.75C storico |
| Fase 2: accettazione Watcher | Bloccato da hardware | Watcher non disponibile; build non equivale a prova radio |
| Fase 3: scene/eventi/timer/memo/frammenti | Implementato, verifica hardware aperta | Richieste1024B/risposte144B; host e OS6 pass; ultima estensione livelli memo da ricompilare |
| Fase 3: stato replicato/coordinatore | Implementato e verificato software | NVS Lamport, gossip differenze, fanout UI, OS6 pass; scena3device da provare |
| Fase 3: instradamento autonomo eventi | Implementato e verificato software | LWW4tipi, coda8 PSRAM, input UI senza loop; test produzione e OS6 pass |
| Fase 3: interfono ADPCM | Implementato e verificato software | RX/TX, sessione, jitter, busy/stop/errori; test produzione e OS6 pass; qualita hardware aperta |
| Fase 4: Remote e Solco | Parzialmente implementato | Migrati bus e test rollback pass; build esterne in corso; canale dinamico e credenziali Remote comuni mancanti |
| Fase 5: companion BLE nodo/orchestratore | Parzialmente implementato e verificato software | Relayfamiglia, Flutter24test e buildWindowsdebug pass; GATT EB01/identita ECOS propria mancanti |
| Fase 6: HTTP solo su richiesta | Parzialmente implementato | API Orbit BLE dirette senza server, fallback legacy locale esplicito, peer EL02 senza IP |
| Sensori condivisi | Implementato e verificato software | Sensor RPC cached mg/mdps/mC, HTTP/BLE/relay, host e OS6 pass; hardware aperto |


## Milestone iniziale

- Letto integralmente ORBIT-ESPNOW.md e istruzioni workspace/repository.
- Repository condiviso a `f7706f5`; fase 2 già presente nel working tree.
- Verifica bus affidata a subagente economico, dispatcher Orbit a secondo subagente economico.
- I dispositivi 1.8 e 1.75C sono segnalati in download mode: nessuna nuova verifica hardware eseguita.
- Test correnti: ancora da eseguire; non attribuire ai nuovi sviluppi gli esiti storici.
- Prossima attività esatta: aggiungere RPC Orbit correlata e autenticata a EL02, riutilizzando task, chiavi e lifecycle esistenti; collegare dispatcher comune.

## Milestone: primo ingresso Orbit sul bus

- Test host bus MSVC superati (lease, dispatch, rollback, potenza, drenaggio callback).
- Test host codec C in emulatore RISC-V superati: envelope Orbit 8+1..144 byte,
  ID non nullo, limiti e correlazione, tipi EL02 9/10; EC01/EL02 precedenti conservati.
- Modificati `services/ecos/ecos_link.c`, `ecos_link_wire.h`, `include/services/ecos.h`;
  aggiunto `ecos/ecos_orbit_wire.h`. RPC serializzata, ID e MAC della risposta verificati,
  replay persistito prima degli effetti, retry dello stesso ciphertext e cache risposta in PSRAM.
- Dispatcher condiviso con HTTP in `apps/link/link.c`, `link_orbit.c`, `link_orbit.h`: scene,
  eventi con cursore, timer e operazioni memo. Nessun worker Orbit permanente.
- `apps/ecos/ecos_app.c`: aura/cancella scena verso un membro dal dispositivo,
  task transitorio e notifica dell'esito, senza riferimenti a schermate chiuse.
- Verifica dispatcher e build delle sei board ancora in corso: queste modifiche NON
  sono ancora dichiarate verificate. Il primo avvio build ha fallito per policy PowerShell;
  rilanciato con policy Process Bypass e ambiente IDF corretto.
- Restano aperte le attività successive della checklist, inclusi coordinamento,
  replica, interfono, Remote/Solco e companion. Non sono blocchi hardware: sono codice da sviluppare.
- Prossima attività esatta: terminare test dispatcher e build; correggere eventuali
  errori, poi misurare simboli statici/mappe prima di proseguire con coordinamento e stato.

## Milestone: test del dispatcher e budget statico

- Superato `python components/apps/link/tests/run_link_orbit_tests.py`, compilando
  il dispatcher di produzione con il vero cJSON: JSON malformati/trailing/NUL,
  ownership scene, limiti esatti di risposta, validazione timer, memo non supportati
  e letture fallite, paginazione eventi e cursori maggiori di INT32_MAX.
- Ripetuti e superati i test codec dopo aggiunta cache: risponde solo a epoca/sequenza
  esatte; sostituzione cache e rifiuto tipi futuri inclusi.
- Prime build 1.8 e 1.75C superate. NON sono le build finali dopo tutte le correzioni.
- Oggetti AMOLED 1.8: bus ancora 190 byte statici interni (somma simboli, escluso
  padding linker). RPC: 28 byte di metadati interni e 288 byte statici PSRAM;
  dispatcher 4 byte per puntatore ops, UI 1 byte per guardia atomica.
  Non è una misura di heap runtime. Le cache per peer e il frame di retry sono
  nella sessione PSRAM, liberata e azzerata con la Famiglia. Il comando UI crea
  solo durante l'uso un task con stack interno da 4096 byte, oltre al task Famiglia
  già esistente da 6144 byte. Nessun nuovo task persistente.
- Build finale 1.8 avviata in parallelo alle altre cinque; script:
  `.release-work/orbit_phase2_final_builds.ps1` (esclude 1.8).
- Prossima attività esatta: attendere gli exit reali delle sei build, leggere
  dimensioni immagini e mappe, completare revisione timeout/retry, poi aggiornare checklist.

## Milestone: cleanup Wi-Fi e fallimenti di avvio Orbit

- Revisione indipendente: corretto `start_link()` quando policy/sospensione o
  creazione task impediscono l'avvio. La richiesta Orbit viene conclusa senza
  aspettare il timeout completo e libera lo slot tramite il chiamante.
- Trovato un difetto preesistente reale della fase 1b in `services/wifi/wifi.c`:
  fallimenti init/mode/start lasciavano netif o driver; `radio_off()` usciva subito
  se la radio non era partita. Aggiunto rollback e cleanup anche senza start.
- Un errore di stop/deinit conserva l'ownership e riprova col timer esistente;
  una coda sys piena riprogramma lo spegnimento. Nessun nuovo task in chiusura.
  Un cambio modalità fallito conserva una radio già usata da altri client.
  Flag di setup da 1 byte impedisce di riusare un driver con configurazione storage fallita.
- Test con fault injection sulle funzioni C di produzione in preparazione:
  NON dichiarare ancora verificato il nuovo cleanup. Build finali precedenti a
  questa correzione vanno rieseguite (incrementali) su tutte le sei board.
- Prossima attività esatta: eseguire `services/wifi/tests` appena il runner è pronto,
  correggere regressioni, poi raccogliere gli exit delle build finali.

- Verifica cleanup ora superata: `python components/services/wifi/tests/run_driver_tests.py`.
  Il runner estrae le sei funzioni C di produzione e le compila con stub MSVC;
  prova rollback init/storage/mode/start, conservazione ownership, retry stop/deinit,
  riferimenti attivi, stato AP e coda sys piena. Monitor job
  `7abf4f8e-246a-4407-a166-464d48bdf93f`, exit 0.
- Inventario board/companion eseguito: preservate le modifiche `.vscode/settings.json`
  di 1.8 e 1.75C; nessuna modifica alla companion. Build finale deve verificare
  `project_description.json -> build_component_info.services.dir` sul checkout
  `ESP-IDF/watch-os-common/components/services`. Scartata una build preliminare 1.75
  che usava il submodule locale: script corretto con env esplicito e reconfigure.

- Superato anche `python components/services/ecos/tests/run_start_link_tests.py`:
  estrae `start_link()` dal codice di produzione; verifica policy, sospensione,
  creazione task fallita, primo avvio e worker già attivo. Job
  `f0fda7cc-3726-4ef4-adbe-776e5770df53`, exit 0.
- Build 1.8 precedente `d7366cc5-2a39-4ea7-bff2-4d6616db0b8b` exit 0, ma anteriore
  all'ultima correzione Wi-Fi/UI. Build finale con reconfigure e verifica directory
  di services/apps/core: `cfa9f8d2-08d6-474c-b8d1-2266cb5ef014` (in corso).

## Sviluppo successivo durante le build

- Nuovo `services/ecos/orbit_model.h` in sviluppo isolato, NON incluso nelle build:
  LWW con Lamport uint64 + MAC autore, tombstone, massimo 24 chiavi da 48 byte,
  valori da 64 byte, storage fornito dal chiamante (PSRAM nell'integrazione).
  Codec esplicito massimo 131 byte per restare dentro la RPC da 144 byte.
- Coordinatore: companion, alimentazione, batteria nota/maggiore, nome, MAC;
  solo nodi svegli. Test del modello in preparazione. Non dichiarare replica o
  coordinamento funzionanti finché mancano wrapper/lifecycle/gossip e integrazione.
- Prossima attività esatta dopo le build correnti: verificare test modello e
  integrare storage PSRAM, contatore durevole, scambio riassunti/differenze su RPC,
  poi ripetere le build per questa nuova milestone.
- Nota per la prossima integrazione: una RPC con canale memorizzato obsoleto
  oggi fallisce senza riscoperta automatica; il refresh Famiglia ripara il canale.
  Prima del gossip, verificare il canale prima di sigillare il comando, così
  la ricerca non avanza la sequenza oltre un comando già sigillato.

## Ripresa del 6/11 pomeriggio

- Ripreso dopo limite d'uso dei subagenti; preservato tutto il working tree.
- Build finale 1.8 `cfa9f8d2-08d6-474c-b8d1-2266cb5ef014`: exit 0 confermato.
- Build corretta 1.75C `451a7c97-6840-4307-adf4-c89ebfead2a0`: exit 0 dal registro
  monitor, servizi/apps/core sul checkout condiviso. Binario 4.262.944 byte;
  DIRAM statico 205.809/341.760, PSRAM BSS 52.452 byte. Non è heap runtime.
- Le altre quattro build sono ripartite con script corretto: 1.75, Knob, Watcher,
  4848. Nessun flash. La precedente 1.75 da submodule resta scartata.
- Modello `orbit_model.h` e `run_orbit_model_tests.py` superati: LWW/tie/convergenza,
  tombstone, capacità/overflow/codec, coordinatore/failover. Ancora non integrato.
- In sviluppo il wrapper `orbit_state.c`: PSRAM, riserva contatore NVS in blocchi
  da 256 (anche l'orologio osservato deve sopravvivere ai reboot), RPC OS01,
  riassunti per indice con chiave completa, gossip transitorio senza RPC sotto mutex.
- Nuovi file isolati, NON ancora collegati alle build: `ecos_orbit_fragment.h`,
  `ecos_orbit_large.c`, `include/services/ecos_orbit.h`, `link/link_orbit_json.h`.
  Test frammenti e client grande superati: 1 KiB in 9 frame, ordine/overlap/bounds,
  duplicati senza seconda esecuzione, timeout, concorrenza del client, errore parziale.
  JSON diretto senza copia sullo stack, limite profondità 12 e rifiuto NUL/trailing
  verificati con il vero cJSON (`run_link_orbit_tests.py --test test_orbit_json.c`).
- Prossima integrazione esatta: aggiungere stato di reassembly per peer nella
  sessione PSRAM, dispatch OF01/OS01 prima del JSON, collegare client grande nel
  CMake, sostituire parser con quello limitato. Prima di sequenziare RPC, attendere
  anche la fine dell'elaborazione della richiesta precedente: oggi il client può
  ricevere la risposta mentre `s_busy` è ancora vero e la richiesta seguente fallisce.

## Milestone preparatoria: replica, comandi grandi e voce

- `orbit_state.c` / `services/orbit_state.h` implementati e testati contro stub SDK,
  ancora esclusi dal CMake. Revisione corretta: task termina con vTaskDelete,
  mutex statico, tick senza snapshot/NVS nel timer, hold condizionale radio attiva,
  riassunti in PSRAM e invio solo differenze, risposte di dimensione esatta.
  Test `29e9c625-5883-4147-b69f-51d18d2b47bf` superato. Il parent deve implementare
  `ecos_family_hold_active()` e collegare init/tick/dispatch.
- Client `ecos_family_orbit_large()` testato: 9 frammenti per 1024 byte,
  timeout globale, guardia contro trasferimenti concorrenti e nessuna ripetizione
  cieca dopo errore. Test `767deae9-d457-403a-92d7-7e94804f4b19` superato.
- Parser limitato verificato anche contro profondità >12 e `\u0000` nei comandi;
  test `b62d5158-63cf-48fb-b651-56d5e0e8e953` superato. Da collegare al dispatcher.
- Codec ADPCM Solco spostato in `core/include/core/adpcm.h`, wrapper compatibile
  in `solco/main/adpcm.h`. Nuovo `ecos_audio_wire.h`: 240 campioni/15 ms, stato
  ADPCM per pacchetto, 142 byte massimi, controllo sessione/ordine/dimensioni.
  Test Orbit e test SNR Solco esistenti entrambi superati
  (`5771e61b-abe9-4134-8989-95bab44832b6`). Audio ancora senza trasporto/lifecycle:
  NON dichiarare implementato l'interfono.
- In sviluppo isolato `apps/link/link_house.c/.h`: coordinatore e job transitorio
  per scena/timer di famiglia, ACK "in coda" distinto dall'esito aggregato.
- Matrice prima milestone: 1.8, 1.75C, 1.75 e Knob superate sul checkout corretto;
  Watcher in compilazione, 4848 successiva. Nessuna di queste build include ancora
  i nuovi moduli stato/frammentazione/voce/house.

## Milestone integrazione replica e famiglia (6/11, 17:30)

- Prima matrice software completa: tutte sei le board exit 0. Ultimo 4848
  `a459f0db-3665-4565-aa2e-337960bbb8b3`, binario 4.273.360 B, slot 6 MiB.
  Watcher OS 4,38 MB entra nei due slot OS 7 MiB: i warning sui tre slot
  4 MiB riguardano EXTERNAL APP BAY; nessuna modifica alle partizioni.
- Integrati in CMake `ecos_orbit_large.c`, `orbit_state.c`, `link_house.c`.
  Reassembly OF01 per peer in sessione PSRAM, dispatch OS01 prima JSON,
  parser JSON 1024/depth12 senza copia stack. Stato init e tick collegati.
- `ecos_family_hold_active()` non avvia radio; shutdown idle ricontrolla hold
  sotto lock. RPC attende anche `s_orbit_processing=false`, ricezione a step
  10 ms e preflight canale PRIMA del seal, anche per canale memorizzato.
- Stato: snapshot famiglia del gossip spostato in PSRAM per ridurre stack.
- `link_house` coordinatore deterministico e fanout scena/timer, ACK queued
  distinto da esito; UI Aura famiglia / timer60 / stop. Flag POWERED bit6
  compatibile; USB ora indica davvero USB, non assenza PMU.
- Build integrata AMOLED1.8 `1dd21ad0-7fac-44e8-a46e-6b9f34f482e6` exit0.
  NON include ultimi cambi snapshotPSRAM, timer_remote e info/gateway.
- Test integrati JSON `5436dcc9...` e stato `a0c0bbb1...` exit0.
  Nuovi test produzione `ecos_family_orbit`: `526e86ab-e8db-4f8b-a1ac-ddbc01da62cc` exit0.
  Timer fault injection: `8e608da2-7e61-4cc9-a6d5-f30f9163d2f4` exit0;
  timer_remote ora non lascia running falso su start fallito e conserva stato
  su stop fallito. Stato dopo snapshotPSRAM `9d4e1a83...` exit0.
- Gateway preparatorio isolato `link_gateway.[ch]` traduce path consentiti in
  RPC e normalizza timer/events. Test `fd024417-d978-435a-a75c-9d0de3e953f2` exit0.
  Non ancora in CMake/rotte. Op info aggiunta al dispatcher e callback board
  in link.c, ancora da compilare/testare. Companion non ancora modificata.
- Entrambi subagenti economici hanno raggiunto limite d'uso fino 21:50;
  file lasciati applicati, root continua. Nessun flash, commit o release.
- PROSSIMO: completare gateway HTTP locale autenticato `/api/link/family`
  GET e `/api/link/relay` POST {id,p,b?}, su ecos_family_orbit_large,
  allowlist link_gateway, risposta max144, nessun retry nuovoID su timeout.
  Poi virtual DeviceLinks companion via unico BLE; test e nuova matrice6board.
- Restano implementabili: instradamento eventi/sensori, interfono (solo codec
  pronto), migrazione Remote/Solco, companion nodo ECOS completo, EB01.
  Prima di altri mittenti audio serve attribuzione TX nel bus, oggi broadcast
  callback a tutti. Non dichiarare chiusa fase3 o successive.

## Ponte famiglia: verifica e riduzione server (6/11 sera)

- Companion virtual family links applicati in `lib/link/device_link.dart`, engine
  include i membri della famiglia. Test singolo `3fa9b3fd...` exit0; regressioni
  BLE/family/Orbit/bootstrap 24 test superati `755d3d3f-effb-4c82-b7ce-815fc232346c`.
  Il failover a un altro gateway forza la riscoperta senza attendere throttle30s.
- Analisi Flutter `5aa497b8...` trova 11 lint sulle nuove if senza graffe: subagente
  orbit_phase3 le corregge prima di proseguire interfono. Build Windows debug avviata,
  esito ancora da raccogliere, nessuna pubblicazione.
- Relay refattorizzato in `link_family.[ch]`, incluso in CMake. Rifiuta target non
  abbinati e path fuori allowlist prima della radio, bilancia lease su errori.
  Test SDK + vero cJSON `d575668a-e61a-49d8-9d78-51b48aa1020b` exit0.
- API BLE diretta registrabile in `companion/ble_companion.h`, `ble_bridge.c`;
  link.c gestisce family/relay e API Orbit senza HTTP. Server locale ora avviato
  solo per fallback; chiuso dopo8s senza richieste HTTP anche se prosegue traffico
  diretto. Risolta race arrivo richiesta durante uscita worker.
  Test funzioni C produzione dispatch/ownership/bounds/handoff
  `9cae8060-d4df-4b4d-8494-4d130c948193` exit0. Build firmware successiva necessaria.
- Formatter sensori in nuovi `link_sensor.[ch]`: snapshot cached, unità intere
  mg/mdps/mC, fresh/age, no avvio hardware. Test `1c2e84cd...` exit0; NON ancora
  collegato a dispatcher/CMake. Mailbox stream lossy nuova `ecos_stream_slot.h`,
  PSRAM max142B, test `3aeae6cd...` exit0; NON ancora collegata a EL02.
- Matrice gateway pre-estrazione: AMOLED1.8 `b42c4e71...`,1.75C `50d3e83f...`,
  1.75 `e82c8c8c...` exit0; Knob `72d5cf98...` in corso, poi Watcher/4848.
  Root ha poi modificato link.c/family/ble_bridge: ripetere tutte6 al freeze.
- Subagenti disponibili nuovamente: bus_verify raccoglie matrice/mappe poi
  implementa attribuzione TX nel solo ecos_bus+tests. orbit_phase3 corregge lint
  e implementa SOLO nuovo link_intercom+test. Root integra trasportostream/sensori.
- Prossimo esatto: raccogliere esiti analisi/build Windows; collegare stream EL02
  type11, slot PSRAM+callback, park su canale sessione, cadence5ms; poi intercom
  worker app, testautomatici e matricefinale. Nessun flash.


## Verifiche ponte diretto e stream (6/11 sera)

- Build AMOLED1.8 diretta+stream `1b138200-4353-41dd-88ae-8bf56ecb7975` exit0.
  Precede ultima integrazione sensori e intercom app. Build Windows debug companion
  `948328d4-afb2-4247-93db-1d21e5bcafbe` exit0, nessun rilascio.
- Family gateway test `d575668a...` exit0: membership, path allowlist, unknown outcome,
  singlecall/no retry, balancedleases, timeoutdiscovery, failurealloc.
- Transporttype11, mailboxPSRAM142, park targetchannel e workerreceive5ms integrati.
  Testwire `e49c0518...`, APIproduction `bc203346...`, mailbox `3aeae6cd...` exit0.
- Gossip sospeso durante stream (anche worker in corso si ferma tra richieste),
  evitando scansioni background che interrompono il canale audio.
  Test `f2e5d2ac-de52-4cac-852d-7340f128da44` exit0.
- Sensori collegati a dispatcher/HTTP/BLE/relay, letturecached senzaattivazione;
  test dispatcher `635c346e...`, gateway `d0755278...`, parser `f2495d23...` exit0.
  Endpoint `/api/link/sensors?part=accel|gyro|temperature`, unit? mg/mdps/mC,
  et? ms e fresh<=1s espliciti. In attesa build prossima.
- Revisione intercom worker ha trovato mancata apertura stream RX e perdita
  campioni sulle letture microfono parziali: agente corregge con test prima
  dell'integrazione. Root deve poi collegare busy-check HTTP/memo, dispatcher
  intercom e UIstart/stop, initcallback e CMake. Non dichiarare interfono concluso.

## Milestone 7 novembre: intercom e rotte, migrazioni in verifica

- Intercom integrato in UI, dispatcher e lifecycle: controllo autenticato, token
  sessione, stream EL02 type11, ADPCM15ms, jitter limitato in PSRAM, esclusione
  memo/HTTP audio, timeout e cleanup. Test produzione con fault injection
  12519b1c-e762-4830-a4fa-17ea5fa10208 exit0. Qualita/latenza radio NON misurate.
- Rotte autonome LWW per tap/dial/dial_done/orbi applicate alla UI del destinatario;
  identita sorgente conservata negli eventi, niente reinoltro. Test route8985df67,
  UI e642c6e3 e dispatcher15e997 exit0. Validazione config ora irrigidita: ripetere test.
- Bus: TX attribuita al solo proprietario con generazione e singolo invio pendente;
  maschere legacy RH/RP e SL02, mutex serializza send/close. Test72cbb8a3 exit0.
- Remote migrato al bus con rollback startup, broadcast peer ripristinato e lease
  unica. Test startup produzione2a011ad7 e hub_state6381df00 exit0. Le sei build
  Remote sono assegnate a routes_finish; canale dinamico e chiavi comuni restano aperti.
- Solco migrato al bus e adapter radio standalone; test4209bf22 e5026e71c exit0.
  bus_finish verifica uscita tardiva worker/cleanup e concorrenza prima delle build.
- Companion: 24 test passati, analyze02f0299c exit0 e Windows debug948328d4 exit0.
  Identita ECOS/GATT EB01 propria ancora mancante; ponte API BLE attuale operativo
  nel codice, NON collaudato su hardware aggiornato.
- Ultima build OS1.8 intercom91746a3e exit0 precede rotte e ultimi fix. Matrice
  precedente: Watcher9ca401ff exit2 per link_sensor_json mancante durante integrazione;
  le altre5 exit0, ma fonti variate. Ripetere tutte6 dopo freeze, raccogliere mappe.
- Prossimo esatto: test rotte dopo validazione, build OS6 in parallelo alla matrice
  Remote (massimo2 build), review Solco. Poi chiudere canale/credenziali e nodo BLE.
- Nessun flash, commit, pubblicazione. Hardware1.8/1.75C in download mode non prova
  le nuove funzioni; Watcher non disponibile per il collaudo richiesto.

## Milestone 7 novembre: OS6 pass, memo companion, credenziali Remote

- Matrice OS6 dopo intercom/rotte/sensori: a176c7e3,717625ab,256119a6,
  9072bc6c,05c4864f,a0602447 tutti exit0, fonte shared provata. Mappe in
  .release-work/orbit-os-memory.json (script orbit_map_report.py). DIRAM statico
  usato: 1.8 203797B,1.75C206113B,1.75 203909B,Knob210165B,Watcher214561B,
  4848 207941B; BSS PSRAM53116B per tutte. NON heap runtime. Delta1.8 +320B
  DIRAM rispetto prima matrice203477B, BSS PSRAM +664B. Flash tutte entro slot OS.
- Successivamente aggiunta storia livelli memo lossless lv48hex in status RPC,
  worst case144B verificato ac901826-e121-41aa-8c22-bde8a97350b0 exit0.
  Modificati link_orbit.[ch] e link.c: ripetere matrice OS6 dopo questa estensione.
- Companion raccoglie nomi memo paginati, cache30s invalidata su comandi, singola
  deadline, zero retry su timeout; normalizza elapsed/duration e livelli. Test
  family180a9282-3cee-4ce3-9c34-617555be5bee exit0, analyzead8959c0 exit0.
  Windows debug da ricompilare dopo queste ultime modifiche.
- Rotte: configurazioni duplicate/extra, MAC multicast, init con identita diversa
  rifiutati. Test produzione13716579-91eb-44f0-9fe0-7ab5818da5ca exit0 include
  stringhe con escape preservate e ACK negativo riconosciuto, senza retry.
- Remote: hid_hub_ecos.h, API hid_hub_family_peers/authorize e UI Usa membro ECOS.
  Consenso HID esplicito su entrambi; HMAC dominio RHUB-ECOS-v1 + MAC ordinati,
  grant legato alla credenziale, rimozione/rotazione ECOS blocca uso al restart.
  Legacy ECDH preservato. Test produzione NVS/grantceb85073 exit0; startup e
  revoca/rotazione07501ec4 exit0. In attesa build target; non dichiarare concluso.
- Script Remote .release-work/remote_orbit_matrix.ps1 e remote_orbit_builds.ps1:
  corretto PYTHONUTF8, backslash env, stderr warning PowerShell; build-orbit
  evita cache copiate da altra board. Prime1.8/1.75C fallite configurazione,
  ripetere al termine della matrice. Attualmente1.75 sta compilando dipendenze.
- Solco prima build e64abdff exit2: include wifi mancante e adapter usava sys_post
  inesistente in lwIP/convenzione0. Corretti includeecos_bus.h+services/wifi.h,
  adapter usa core/sys.h e bool !sys_post. Stub ora coerente col vero header.
  Test cf404c89-fa5e-43a3-b169-75c631cb6542 exit0. Build corretta26fcc3d3 in corso.
  Cleanup tardivo gia eseguito dal worker prima di azzerare s_task; nessun UAF.
- Subagenti bus_finish/routes_finish hanno raggiunto limite uso. Root continua.
- Prossimo esatto: raccogliere26fcc3d3, correggere eventuali errori; OS6 incrementale
  dopo memo mentre Remote completa (massimo2 build); poi Solco altre5 e Remote
  ripetizione1.8/1.75C. Nodo BLE EB01 proprio resta implementabile e aperto.
- Nessun flash, pubblicazione, commit. Download mode non equivale a collaudo.

## Freeze firmware 7 novembre: RAM Remote e matrici esterne

- Remote1.75 job3e686ae3-bf30-4e7f-9a4a-52053ece88a2 exit0 con UIgrant e shared
  services/hid_link/core verificati. Precede tuning WiFi seguente: da ripetere.
- Nuove code Remote RX16/local8/ACK8 ora xQueueCreateWithCaps PSRAM, cancellazione
  WithCaps in rollback. Test produzione c863e32b-11fc-455e-9ede-dfd898659b03 exit0.
- Test grant ECOS/NVS produzione ceb85073 exit0; legacy e consenso esplicito su
  entrambe le parti preservati. Memoria chiave derivata confrontata con grant
  salvato: cambio credenziale richiede nuovo consenso. Non e trust automatico HID.
- Trovati vecchi16RX/16TX/32cache/32dynamic/BA16 nelle configurazioni Remote.
  Allineati defaults e sdkconfig locali delle sei board a4/4/8/16/6 come OSfase1b
  (4848 gia configurata). File defaults Remote modificati nei repo board.
  Helper build verifica anche sdkconfig.json finale e main.sources shared.
  Knob era gia in build con config precedente: se fallisce verifica finale,
  ripetere una volta dopo il termine; anche1.75 richiede nuovo build per tuning.
- Solco1.8 job26fcc3d3-ce85-414c-8479-74c65a0761ce exit0 e sourceproof3file shared.
  Nuovo script solco_orbit_matrix.ps1 per sei target; non flasha ne pubblica.
- Sessione exec7981 esegue orbit_os_then_solco.ps1: OS6 aggiornati dopo memo e
  refactor route_discard, poi map_report, poi Solco6. OS1.8 job017a2c9d exit0 e
  1.75C c1afaaba exit0, altre4 in sequenza. Non duplicare queste build.
- Sessione exec90480: matrice Remote corrente (prime2 fallite config Windows,
  1.75pass prima tuning, Knobfullbuild in corso, poi Watcher/4848). Al termine
  eseguire NUOVA matrice Remote6 ora che env/config sono corretti; ccache riusa
  dipendenze gia compilate. Necessario verificare tutti6 dopo tuning.
- Companion regressioni BLE/family/Orbit/control/pending-close 25test passati
  job8b68ac26-08a8-4fef-a40f-9681d7204d66 exit0. Ultimo analyzerad8959c0 exit0.
- Route cleanup leggibile/helper drenaggio, nessun cambio contratto: ab2d498b
  -6545-4f53-8cf0-8de5b73b0526 exit0. Source OS congelato per la matrice attuale.
- Resta codice implementabile: BLE nodo ECOS proprio/EB01 e coordinatorecompanion,
  gestione canale comune dinamica. Non classificare questi come blocchi hardware.
  Ricerca libreria crittografica: PointyCastle4.0.0 supporta P256/AESGCM in Dart
  Windows/Android (docs pub.dev), nessuna dipendenza o sorgente aggiunta ancora.
- Prossimo esatto: esiti matrici7981/90480, ripetizione Remote6 finale, Windowsdebug
  dopo memo, mappe esterne. Poi implementare nodo BLE sopra handshake EC01 esistente
  riusando commitment/SAS/NVS, senza doppie chiavi o bypass del consenso.

## Fase5 avviata: endpoint EC01 companion (ancora non cablato al BLE)

- Aggiunta dipendenza Dart pointycastle4.0.0, pubget0ee9e85f exit0; pubspec e
  lock modificati (nessun aggiornamento generale). Documentazione primaria:
  https://pub.dev/packages/pointycastle/versions/4.0.0
- Nuovo Orbit-Companion/lib/ecosystem/ecos_pair.dart: endpoint EC01 bytecompatibile
  con ecos.c/ecos_wire.h, P256 ECDH library, SHA256 commitment prima hello, transcript
  ordinato, SAS6cifre, tagCONFIRM/SAVED, conferma associata a peer+codice, duratamax180s,
  nessun timer/radio proprio. Credenziale esposta solo dopo consenso di entrambi;
  markSaved va chiamato solo DOPO persistenza riuscita. Controllo punto on-curve,
  encoding canonico e MAC unicast; dispose impedisce riuso e azzera keybuffer.
- Nuovi test/ecos_pair_test.dart (lifecycle, invalidi, commitment immutabile,
  off-curve, timeout, interop) e test/support/ec01_reference.py. Riferimento Python
  hashlib/hmac indipendente con peer testprivato1/publicG; nessuna chiave reale.
- Job testb620f1ab-7493-4285-8774-00e669a45cd0 in corso, sessione83086: hook
  flutter_soloud ha ricompilato molte sorgenti C++; ora ha iniziato caricamento test.
  Source aggiornato durante hook: assicurarsi che include test interoperabilita.
- Primo analyzer7f399566 exit1 (cast superflui e quattro if senza graffe): corretti;
  rieseguire dopo test. Non dichiarare endpoint verificato prima exit0.
- Non ancora collegati: EC01 viaNUS/GATT sul firmware, identita companion persistente,
  trasporto frame cifrati famiglia, priorita coordinatore. Questi restano lavoro software.
- Prossimo tecnico: dopo OS6 corrente (fonte congelata) estendere ecos.c con trasporto
  BLE callback+RXqueue PSRAM, riusando lo stesso worker/commitment/SAS/NVS. La coda BLE
  va unpublish+refcount prima delete, cosi receive concorrente non usa memoria libera.
  Handler NimBLE solo valida/copia e avvia API nonblocking; output dal pairing worker,
  legato a connessione+generation; disconnect cancella SOLO la finestra BLE propria.
  Poi driver Dart+UIconsenso, persistenza e framebridge. Nessun bypass del consenso.

## Checkpoint 7 novembre: pairing BLE nodo companion integrato

- OS6 dopo memo/route e Solco6: tutte build exit0; risultati in .release-work/orbit-replica-gateway-builds.json e solco-orbit-results.json. Queste OS precedono EC01 BLE.
- ecos.c: trasporto BLE riusa worker EC01, commitment/SAS/NVS; RX PSRAM con unpublish/refcount, contesto sessione vincolato, disconnect non cancella finestre successive. Ripristino policy prima rilascio busy. Test API 40b9cba9-eda5-42f5-a529-12aa0ae20f07 exit0.
- Nuovo companion/ble_ecos.c: messaggi NUS autenticati e cifrati dal collegamento, q+epoch, frame pubblici EC01 base64, nessuna credenziale trasmessa. Nessun task aggiunto. Test produzione affidati a bus_finish.
- OS1.8 con integrazione completa C/NUS: a72e73dd-df1f-4e9d-8a89-1972909ad5fc exit0. Altre cinque da ricompilare dopo freeze.
- Dart ecos_pair: sei test inclusa interoperabilita Python indipendente b620f1ab exit0. Driver CompanionNode, identita/credenziali persistenti e pagina consenso integrati; undici test driver+endpoint 483df736 exit0 PRIMA serializzazione storage.
- Serializzazione IO aggiunta contro cancel/restart durante primo salvataggio. Nuovi test concorrenza e archivio corrotto da eseguire. Analyzer913d ha rilevato getter UI dim inesistente e lint: correzione a muted e fix lint in corso; nessuna dichiarazione analyzer passato.
- Matrice Remote sessione77170 in corso: primo1.8 fallito per sdkconfig generato MSC disabilitato; corretto MSC=y. Watcher flash generato corretto32MB. routes_finish raccoglie esiti e ripete fallimenti, non duplicare matrice.
- Prossimo: test storage/regressioni e analyzer, test C metadata, build Windows debug e OS6 aggiornate. Restano software: ponte frame cifrati famiglia, precedenza companion e canale comune dinamico. Non sono blocchi hardware.
- Nessun flash, release, commit; prove hardware e heap runtime ancora non eseguite.

## Milestone EC01 verificato e ponte cifrato in sviluppo

- Companion regressioni38 test, inclusi storage concorrente e archivio corrotto: 74693382-c081-43b7-8d48-e53f5f1af895 exit0. Analyzer b018d22b-cdbe-4167-93b3-5dd174e3b786 exit0 (prima ponte EL02).
- Test host ble_ecos produzione+cJSON reale: bd89ae19-d375-4a75-b2e6-3eb877b7f803 exit0. Include terminalcallback con reference concorrente e fault alloc/sys_post.
- Nuovi ecos_cipher.dart e openCipher su CompanionNode: AEAD EL02, chiavi direzionali HMAC ELNK, epoch riservato su storage prima send, replay persistito prima delivery e serializzato. Fixture Python AESGCM indipendente test-only. Tre test crypto e otto driver/UIconsenso: 1036f11a-0734-4697-aae0-27f6138b4f8d exit0.
- Primo widget test falliva per fake clock (non bug UI); corretto usando runAsync per scambio BLE e pump per render. Test ora verifica entrambi consensi prima salvataggio.
- In sviluppo: companion/ble_ecos_bus.c + h, CMake e autenticazione ecosbus in ble_companion.c; singleton sessione MAC/q/epoch, mutex richieste-stop e callback drain nel servizio. Non ancora compilato: dipende API ecos_family_ble_* che bus_finish implementa SOLO in ecos_link.c + ecos.h, con host test. Root possiede metadata BLE e Dart.
- Nuovo ecos_bus.dart lease EL02: RPC singola max144, timeout esito ignoto senza retry, status15s, close su disconnect, cancellazione waiters. Non ancora cablato al motore/UI; non dichiarare utilizzabile. Test integrazione513a8e1b in corso, sessione23032.
- Prossimo esatto: esito513a8e1b, fix analyzer, test metadata host, integrare bus/UI e precedenza companion con handler reale. Poi OS6 e Windows debug aggiornati. Remote retry e mappe assegnati routes_finish; Solco6 gia exit0.

## Freeze EL02 BLE e verifica finale gruppo

- API ecos_family_ble_open/receive/close implementate in ecos_link.c + ecos.h. Riusa worker/AEAD/replay e coda8 PSRAM; TX callback e queueRX drenate prima teardown. Test produzione MSVC760454eb exit0 (aperture concorrenti, context estraneo, routing e drain).
- Review root ha corretto tramite bus_finish la race reservation/hold (incremento s_holds atomico con pubblicazione) e notifica finelease NULL/0 su startupfail/policy/suspend. Test aggiornato d1c8fa09-7a42-465e-b936-99a816430ff3 exit0. C ora congelato.
- ble_ecos_bus.c inoltra terminale end:true. Test produzione+cJSON125b1e0d-4587-41dc-9ed3-2bad61aaf2ed PASS (attendere actual EXIT monitor se non ancora raccolto): limiti205/Base64, alloc/openfail, q/epoch, ACK fallito e drain callback concorrente.
- Dart bridge integrato nel motore e pagina famiglia: Collega al bus/Verifica nodo/Scollega. Pronto solo dopo STATUS EL02 autenticato; probe500ms durante startup, heartbeat15s e timeout45s; niente task/timer a lease chiuso. Handler coordinatore non ancora implementato e nessun flag priorita annunciato.
- Test14 crypto+bus+driver/UI0bf4145d-1a2a-4e64-ad9e-50c351b11434 exit0. Analyzer finale8f9dea7f-4403-4b0d-a877-28fc62f4f72f exit0; script companion_ecos_verify.ps1/sessione67396 passa ora a regressioni45 e Windowsdebug. Piccola guardia generation aggiunta dopo avvio analyzer ai callback timer/onDone: da includere nella verifica successiva.
- OS1.8 EC01+EL02: sessione27566/job78315563-6e20-4079-99c0-08d8f5800781 in corso, C nuovo compilato, fase link. Non dichiarare build passata prima EXIT0.
- Avvio accidentale duplicato matrice (jobfbec1430, sessione12903) fermato esplicitamente sull'albero processi della SOLA nuova matrice: NON conteggiare come build riuscita. Causa rimozione prima board fallita per CRLF/LF; script orbit_ble_matrix.ps1 ora esclude1.8 verificabilmente. Non ancora rilanciato; attendere1.8 poi avviare altre5.
- Remote matrice77170 continua; attendere freeze e completamento prima ripetizione6 incrementale con sharedcompanion aggiornato. routes_finish raccoglie esiti e mappe.
- Canale: analisi mirata bus_finish conferma scan Famiglia puo spostare radio mentre HID/Solco attivi. Servono prenotazione centralizzata canale, scansione limitata sotto pin, annuncio stabile ecos/channel (non ogni hop). Nessun codice canale ancora cambiato. Router/AP restano autorita; due router diversi limite fisico da segnalare.
- Prossimo esatto: esiti27566/67396, test metadata actualexit, avviareOSaltre5 corretto eRemote6 finale. Poi priorita/handler companion e canale: restano lavoro software, non blocchi HW. Nessun flash/release.

## Verifiche concluse e coordinatore companion in implementazione

- OS1.8 ponte EC01/EL02 job78315563-6e20-4079-99c0-08d8f5800781 actualEXIT0 e sourceproofshared. OS1.75C0a1612f5-4b52-4acf-abb1-55c96df51042 actualEXIT0 (letto jobs DB sola lettura, non dedotto da fine processo).
- Companion analyzer8f9dea7f exit0, regressioni45 job374b3c8e-1ec5-46fc-880c-999c9bd42b10 exit0, Windowsdebug8acf71e3-d10f-4def-9988-6ce9cc78ecbe actualEXIT0. Questi precedono handler/coordinatore seguente. Nuovo test cancel/read/reconnect aggiunto: incluso nella prossima esecuzione.
- Metadata BLE bus125b1e0d actualEXIT0 confermato jobs DB.
- Sessione47432 esegue finish_os_ble.py: ha atteso actualexit1.75C, poi matricealtre4 orbit_ble_four.ps1. PrimoOS1.75 job6f76ccab-1495-4d22-ad30-0776815f417f. Result finale orbit-ble-final-results.json al termine. Fonti possono includere parte nuovo coordinatore: ripetereOS6 dopo freeze del gruppo seguente, non assumere questi artefatti finali.
- Nuovi Dart ecos_commands.dart (OF01 max1024, ordini/duplicati/risposte cache) ed ecos_orchestrator.dart (un jobcasa, scene/timer locali senza rielezione, conteggi accettati/rifiutati/ignoti, nessunretry). Engine integra handler ma tutto ancora in test. Job7317193c/session13701 include commands+bus; analyzer successivo necessario.
- Architettura priorita: phone annunciaflag128 solo con handler e target disponibili. Gateway riconosce SOLO statusAEAD del peerBLE attivo/fresco<=30s, annuncia a sua volta flagproxy128 ai suoi pairedneighbors. Cosi gli altri nodi possono scegliere il gateway senza possedere la chiave phone. house_worker sul gateway delega a phone prima di discovery/applylocal, anche per house direct:true; phone fanout SOLOscene/timer locali, nessunloop di election. Disconnect invalida subito reachabilityphone e proxy locale.
- Ownership corrente: bus_finish SOLO ecos_link.c/ecos.h/ecos_link_wire.h +test presenza; routes_finish SOLO house test/stubs e matriciRemote/log, root apps/link_house.c +Dart. Header nuovo gia presente, implementazione/test C presenza in corso. Richiesta finale di nuove build SOLO dopofreeze.
- Punto aperto: evitare broadcast di scena al recordphone come displayfisico; il flagcompanion deve escludere phone dai targetfanout fisici quandocoordinatorehardware. Canalecomune ancora softwareaperto; nessun flash/release.

## 8 novembre: priorita companion verificata host; freeze finale gruppo

- Regressioni companion49 job9906315a-5afb-49da-baf8-d3391b990dce concluse con tutti test passati; comprendono comando casa cifrato in ingresso, dedup con nonce nuovo e stesso RPC ID, OF01 e fanout, cancel/read/reconnect e filtro nodo virtuale dalla lista dispositivi fisici.
- Presenza/proxy C implementata da bus_finish prima del limite quota: flag128, status_seen_at separato dalle altre attivita, matching peerBLE attivo/fresco30s, invalidazione immediata su close e finelease su reload credenziali. Test produzione rieseguito root c12390b7-47be-4938-a8e2-a92e9b8429c0 EXIT0.
- House priority/proxy test produzione root7f23ac5c-c35b-454b-bd29-178abb54b5e1 EXIT0. Delega prima scan, anche direct:true; timeout senza retry, priorita proxy e fallback locale. Il nodo virtuale non e un display e viene escluso dal fanout fisico; gateway fisico proxy resta incluso.
- Matrice OS BLE precedente tutte6 EXIT0: orbit-ble-final-results.json. Include snapshot misti precedenti priorita: NUOVA matrice finale sessione3488 in corso, primo1.8 bbdb32bc-30aa-4bda-9aab-cdac4e0e0f1d, script orbit_replica_gateway_builds.ps1, risultati orbit-replica-gateway-builds.json al termine.
- Remote vecchia matrice:1.8 eWatcher fallite, altre4exit0. Nuova matrice6 finale sessione33421, primo1.8 ad216b7b-ab6f-4757-b233-05e4e8913294. CorrezioniMSC/flash32 gia applicate; verificare eventuali ulteriori errori effettivi. Non avviare altre build firmware parallele.
- Analyzer companion finale coordinatore e3331beb-8f82-4dc9-971c-1a02b1c2e5a4/session63894 in corso. Windowsdebug precedente passata ma precede orchestratore: ripetere dopo analyzer/test correnti e freeze Dart.
- bus_finish quota esaurita alla fine del sottotask; root ha recuperato sorgenti e verificato test, nessun lavoro considerato passato per sola dichiarazione agente.
- Prossimo esatto: analyzer e matrici attive, correggere solo errori, Windowsdebug aggiornata e mappe memoria. Poi gestione canale comune (ancora lavoro software aperto). No flash/release.

## 8 novembre: gruppo canale comune in sviluppo (non ancora freeze)

- Nuovo modello pureC wifi_channel.h4owner statici, senzaheap/task, test MSVCf1a39b40-9702-42a3-808d-97834d3c6c3d EXIT0.
- Ownership routes_finish: wifi.c/wifi.h/testwifi; integra reserve/release/pinned e commonchannel NVS'channel' namespacewifi, rollbackSDK, idleSTAquando rimangonosoloradioRefs usando timer/sysworker esistenti. Non toccarequestifile fincheagentecomunica freeze.
- Root ha integrato pin in adapterStandalone e Solco (rilascio inognirollback/finish), Remote sceglieconnectedcurrent/pin/commonNVS e riservaownerRHUB anzichecanalefisso1. HoststartupHID b808ade4 PASS, adapter45e46a0e PASS (raccogliereactualEXIT seancorapending).
- Famiglia park legge ecos/channel, rispetta pin, annuncia/persistesoloalrientrostabile e non duranteaudio; search salta hoprifiutati. EC01 usacommonmeetingpoint e nonforzainvio suhopnegato.
- Stream OA01 ha mutexstatico80Bcirca (nessunheap/task) per lifecycle+pin; reserveprima pubblicazione e rilascio draincleanup/close soloowner. Ora richiedeworkerlistening. TXintercom apre streamDOPO RPClisten, cosicoldstartup/preflight trova peerprimadi bloccarecanale; RXpin sulcanalericevuto. Teststreamproduzione4e601a7b-c0c0-4443-96bd-d6515f37dd64 EXIT0. Serve intercomtest e firmwareaggiornati.
- Le matrici3488OS6 e33421Remote6 sonoancoraattive mafonticanalemodificatedurantegruppo: risultati validi perartefatto, NONultima verificafinale. Nonavviareduplicati; completarequeste e rieseguireincrementali dopofreeze. Remote1.8 rigenerazioneMSC richiede prima ricompilazionefull1930file, ormai>1200.
- Analyzercoordinatore e3331beb haunlintbrace; corretto. Nuovoanalyzer28609dc4-e2cc-4190-9cfa-e7db1a97fdeb EXIT0; scriptcompanion_ecos_build.ps1/session78386 staeseguendoWindowsdebugfinale. Dart funzionalmente49testPASS9906315a, nessun cambiologico successivo.
- Prossimo esatto: testparkannuncio e intercom, esitifirmware/Windows, freezeWiFiagente, correggereeventualiSDKerrors. Poi matriciOS/Remote/Solcofinali, mappe e docs. Nonclassificarecanalecompleto senzaquestiverifiche; nessunflash/release.

## Verifica gruppo canale e correzioni di integrazione

- Windowsdebug finale companion331f3113-f0a9-4fcb-b6d8-457e64ffc7d8 actualEXIT0 dopo analyzer28609dc4EXIT0; include orchestratore/proxy. Dart49test9906315a actualEXIT0.
- TestHIDstartup canalecomune b808ade4 actualEXIT0 e adapterStandalone45e46a0e actualEXIT0 confermati. Nuovo testpark/NVSannuncio1b7f74fe-cf2f-472f-b624-7aaa8989bc62 EXIT0. Prima58a9f48f testPASS ma runnerEXIT1 perCWDtemporaneo occupato dal compilatoreWindows: corretto cwdtestrepo, rieseguitoEXIT0.
- Teststream lifecycle/pin4e601a7b EXIT0. Intercom prima70ac3a25EXIT1 pervecchiaaspettativa test(openprimaRPC): aggiornato all'ordine corretto e aggiuntoconflittopin +stop dopoACK. Nuovo9842cff7-6a2c-4da5-88a6-5dbf1f30a3f9 EXIT0.
- MatriceOS3488 conclusa: prime3pass, ultime3fallite transitoriamentewifi.c include services/wifi_channel.h sbagliato; routes_finish ha corretto in wifi_channel.h. Remote33421 ancora in corso: primo1.8 stessa causafallimento dopocompilazioneSDKcache,1.75Cpass. Necessario rerun6finali dopo freezeWiFi, non conteggiareartefattiintermedi comeultimefonti.
- Root reviewrichieste a routes_finish: bloccare nuove scansioni/join SOLOdurantepinOA01 (HIDnonimpedisceInternet); evitarechegetterpinned/common e setterpark aspettinos_mutextenutoda join30s: snapshotnonblockingfalse/0, nessunaletturacachedsenzalock; releaseinvecedraina normalmente. Questi test/implementazione ancora in corso, attendere freeze.
- Budget aggiuntivo previsto statico streammutex ~80B +modello canale24B, nessuntask/timeraggiunto. NONmisuraheapruntime; mappefinali da rigenerare.
- Prossimo esatto: freezeWiFi +testproduzione, attendereRemote33421, avviarematriciOS6/Remote6/Solco6 sullefontifinali e mappe. Aggiornarechecklist e docstatoeffettivo. No flash/release.

## Freeze canale/WiFi del 8 novembre

- routes_finish ha congelato wifi.c/wifi.h/wifi_channel.h e test: wrapper c9cb1121-0331-47aa-b0df-d1570442c85b EXIT0, policy37912ba1-6b75-4fad-a73b-2534c5790115 EXIT0. IncludeSDKcorretto, pinrollback, default/commonNVS, rete/APautorita, snapshotnonblocking, radio-onlySTAdisconnect/retry, nuoviIPrefsprimaoffjob, protezioneOA01controjoin/scan.
- Root ha spostato il default Kit UI da6fisso a wifi/channel condiviso inNVS (fallback1), lettounavolta inkit_job; valoreUIatomico. CLI Solco mantiene canaleesplicito ma riservato dalmodello comune. Standalone non dipendedaserviziIP/NVSper lapolicy; ui_master giausavaNVS.
- Capacita statoLWW aumentata24->48 inPSRAM:28rotte(6peer+self)x4+canale lasciamospazioperconfigcasa. Nessun arrayinterno nuovo, solo store/snapshotPSRAM. Testregressione famiglia piena16b0716e-d495-44cc-964c-a4bdd9aaf8d0 EXIT0, replica14276eac-ed70-4258-b8f5-9e5561ca876a EXIT0. Evita checanaleannunciofalliscasempredopo24rotte.
- RootavviabuildOS1.8prima dellematrici successive; codicecanale/stream/Intercom ora congelato salvoerrori. AttendereEXIT0SDK, poi OSaltre5+Solco6 eRemote6finale; Remote33421precedenteancoraattiva, nonduplicarealcunbuilddir.
- Restano da verificare tuttefontifinali conmappe; svegliedicasa menzionatenella roadmap4.3 devonoessere censite/integrate, non dichiarare progetto totalmentechiuso. Ilringallarme localeesiste services/alarm/alarm.c ma non c'e ancoraoperazionehouse_alarmnelbus. Prossimo dopo freezeverifiche: completare ingressisveglieconNVS/rispostefailclosed e replica configurazione, preservandoallarmilocali.
- Nessunflash/release. HWqualita/heap/cacheBLE richiedeautorizzazioneflash e dispositiviavviati, non downloadmode.

## Pipeline di verifica finale canale: stato riprendibile

- OS1.8finalea0b596d6-c6c6-4f5a-97bd-50eaa236b010 actualEXIT0, codiceWiFi/channel/stream48keys. Map: totale4374887B, DIRAM204005B/free137755B, ExternalBSS53180B. Rispetto precedente203797/53116: +208B DIRAM,+64B externalBSS. Store48entries e snapshot sono allocatiPSRAM (non nelBSS), +24entries rispettoprima; nessunamisuraheapruntime.
- Pipeline sessione89698 `.release-work/finish_channel_matrix.py`: dopo actualexit1.8 esegue altre5OS conorbit_ble_matrix.ps1 (primo1.75C c056c5a0-5010-4747-a273-add4eef3f2b4 giafinito nel log), poiSolco6. Scrive orbit-channel-final-results.json al termineOS6. Solcohelper corretto perexit1se QUALSIASI targetfallisce, non solol'ultimo. Non duplicarequestebuild.
- RemoteWatcher vecchiamatrice21befbac fallito MSCdisabilitato nel sdkconfiggenerato, mentresdkconfig.defaults giaMSC=y; corretto solo configurazionegenerataWatcher aMSC=y, flashgia32MB. Verificatitutti6altreconfigMSC=y. HelperRemote oraasserisceTINYUSB_MSC_ENABLED=true oltrebuffer e sourceproof.
- VecchiamatriceRemote session33421 prosegueultimotarget4848. Pipeline session10843 `.release-work/remote_final_after_current.py` attendeJSON6risultatieffettivi poiavviaNUOVAmatriceRemote6 sullefontifinali; nonavviarlaamanualmente. PuorichiederefullrebuildWatcherdopocambioconfigglobale. Nessunflash.
- Nuovo sottotask routes_finish SOLO alarm/tests: harnessNVS/timerpermetterein evidenzafallimenti attualialarmservice. NESSUN edit codicealarm.cdurantefreeze. Analisi mirata conferma savevoidignoraopen/set/commit,errorloadnonvalidacount/blob, sys_posttimerfailnorearm. Rootdevedecidere/migrarestorageatomicoperprossimogrupposvegliedicasa. Non dichiararealarmscondiviseimplementate.
- Prossimo esatto: raccoglierematrici89698/10843 ehostalarmattesirossi, generaremappeOS/Remote/Solco consourceproof. Poi infrastrutturaalarmstoragefailclosed+identificatoricondivisi, mantenendoinvariatoalarm_t e allarmilocali. Blocchihardware separatidaquestolavorosoftware.

## 8 novembre: matrici finali concluse, persistenza allarmi

- Pipeline89698 e10843 entrambe actualEXIT0. JSON orbit-channel-final-results, remote-orbit-results, solco-orbit-results contengono ciascuno6/6 exit0. Totale18 build finali del gruppo canale; nessun flash/release.
- Mappe OS rigenerate con orbit_map_report.py, verifica sourceproof apps/services/core per tutti6; output orbit-os-memory.json. Mappe Remote/Solco ancora da aggregare.
- Agente routes_finish ha esaurito quota dopo aver scritto alarm/tests/stubs e test_alarm_persistence.c; root ha completato runner e correzione produzione. Non riattivare agenti a pagamento maggiore senza necessita.
- Modificati alarm.c: save restituisce errori NVS, aggiorna versione solo dopo commit; save/delete rollback RAM e ID chiamante su errore, overflow ID rifiutato; load verifica lunghezza multipla e limite; timer riarmato1s su sys_post fallito. Schema alarm_t/list storico preservato.
- Test produzione estratta MSVC930897e9-8351-462b-8f69-22dabab2d608 actualEXIT0. Primo runnera5c65581 fallito per costante STATE_ALARM_VERSION mancante nello stub, corretto e rieseguito.
- Build OS1.8 corrente session71567/job5b8c2621-b92b-4d99-a72d-c3862b92c2e6: raccogliere actualexit prima di considerare verifica firmware allarmi. Dopo fix ulteriori ripetere6OS.
- Prossimo esatto: completare validazione record/load e import atomico, test positivo/multi-record rollback e build6. Poi sveglie condivise/RPC/LWW e Companion: ancora lavoro software aperto, non un blocco hardware. Non marcare progetto completo.

### Aggiornamento verifica allarmi e mappe

- Mappe Remote/Solco aggregate da orbit_external_map_report.py:12/12 con sourceproof, output orbit-external-memory.json. OS6 mappe gia raccolte. Sono mappe statiche, non misure heap runtime.
- Build allarmi preliminare5b8c2621 actualEXIT0; include primo rollback ma precede ultimo import/lock. Non considerarla finale.
- Aggiunti import record+marker stesso commit (errore non pubblica record), validazione campi/ID duplicati del blob, init mutex statico serializzata e controllo creazione timer. Nessun nuovo task/timer, nessuna modifica ABI/allarmi locali.
- Host finale0c5c6924-2031-4aa6-98ce-cf65b1a07775 actualEXIT0 include successi e rollback cancellazione centrale e import fallito. Runner estrae funzioni reali di produzione, non le reimplementa; lock/SDK/import filesystem sono mock. Correzioni di init mutex/timer ancora da verificare SDK.
- Matrice OS6 allarmi session24047, script orbit_alarm_builds.ps1, primo17fa78de-809e-4c01-ba02-b825923f5d08. Codice congelato per questa matrice. Risultati orbit-alarm-builds.json al termine, aggregazione errori attiva. Non duplicare builddir.
- Checklist principale corretta eliminando stati obsoleti 'build in corso' del gruppo canale/companion/Remote. Sveglie condivise ancora mancanti; nessuna dichiarazione di completamento complessivo.

### Allarmi: primo target finale e regressioni caricamento

- OS1.8 finale17fa78de-809e-4c01-ba02-b825923f5d08 actualEXIT0. Matrice24047 ora1.75C job56237dc2-7430-4e42-a73a-5496c7d2f629, poi1.75/Knob/Watcher/4848. Attendere exit e orbit-alarm-builds.json prima di nuova build sullo stesso target.
- Test9d1b9b5e-f1f9-428b-836c-ae2493625039 actualEXIT0 con regressioni caricamento valido, campi invalidi, label non terminata e ID duplicati. Include tutti i test precedenti.
- Mappa1.8 aggiornata orbit-alarm-18-memory.json: rispetto gruppo canale FlashCode+624B, FlashData+48B; DIRAM/ExternalBSS invariati. Nessun aumento di task/timer/heap permanente introdotto da questo gruppo, nessuna misura heap runtime.
- Review chiamanti: alarms_app.c salva chiudendo editor solo ESP_OK; toggle linea20 e delete linea190 ignorano invece errori. Prossima correzione UI deve ripristinare toggle su fallimento e mantenere editor se delete fallisce; considerare messaggio utente. NON modificati durante questa matrice congelata.

## Chiusura versione 1.18.1 richiesta dall'utente

- Autorizzazione esplicita ricevuta: flash dei due dispositivi collegati e pubblicazione OTA per le altre board. Sostituisce il precedente divieto di flash/release. Non autorizza cancellazione NVS.
- Matrice allarmi orbit-alarm-builds.json finale:6/6 actualEXIT0. Snapshot implementato congelato per release. Sveglie condivise restano escluse dal perimetro pubblicato; nessuna dichiarazione che tutta la roadmap sia completata.
- Cataloghi GitHub consultati via CLI ufficiale: tutti6 stable1.18.0. Prossima patch1.18.1, immutabile. Preparazione su checkout isolati puliti, preservando market_app.c e impostazioni VSCode preesistenti.
- Sequenza prevista: commit/push sorgenti del gruppo, build produzione6 e preflight pacchetti; backup/flash appslot sui MAC noti1.8 e1.75C, verifica boot; solo dopo pubblicazione cataloghi6 e verifica hash remoti.

### Rilascio 1.18.1: riferimenti operativi

- Commit libreria fc1ed21 pubblicato su main. Preservati market_app.c preesistente e gli artefatti test locali non sorgente; nessun binario di test committato. Checkout pulito per build: `.release-work/orbit-common-1.18.1`.
- Driver `.release-work/release_orbit_1181.py`, sessione50360, job e973468d-c6e3-452c-a7f2-6fcf2815ab5d. Prepara i sei checkout gia isolati in `.release-work/<board>`, porta i commit board di fase1b e le modifiche Remote/Solco, aggiorna1.18.1, push main, build-release(max2), poi verify-only. NON pubblica automaticamente.
- Output `.release-work/stable-1.18.1/{boards.json,notes.md,*-build.log,*-verify.log,packages}`. Config prod copiata dagli sdkconfig della matrice finale, console dev disabilitata; preflight ricontrolla sourceclean/pin/versione/identita/QA.
- Backup privati fuori repo `C:/Users/matte/.orbit-backups/1.18.1`. Script orbit_backup_devices.py, sessione71013/job9d2adfdd-3805-4763-92de-2a46a0f1378e. MAC letti e confermati COM3=10:20:ba:46:46:3c, COM10=a0:f2:62:e3:07:94. Primo tentativo shell cmd session15397 uscito127 senza eseguire backup; sostituito dalla sessione PowerShell71013.
- Flash helper `.release-work/orbit_flash_final.py` NON ancora eseguito. Scrive solo lo slot OS rilevato/precedentemente salvato, verify-flash, watchdog-reset e boot30s; pretende1.18.1+Ready senza panic. Produce hardware-verified.json solo dopo entrambe le verifiche. Se OS non e lo slot attivo si ferma prima di scrivere e serve selezione slot esplicita.
- Pubblicazione autorizzata: dopo build/preflight e flash/boot avviare `release_orbit_1181.py --publish` sotto monitor. Il driver richiede hardware-verified.json; poi verificare cataloghi6, hash asset scaricati e tag sorgenti. Non dichiarare release/flash riusciti prima degli actualexit.

### Rilascio 1.18.1: pipeline definitiva con cache validate

- Primo driver isolato e973468d interrotto esplicitamente prima di qualsiasi pubblicazione: avrebbe ricompilato tutto SDK; non e un esito riuscito. I suoi sei commit di release restano quelli corretti e sono gia su main remoto.
- Preparazione sorgenti puliti sulle cartelle originali fa0daeaf-838c-49ac-8400-43a3abb809a7 actualEXIT0. Prima e24e3a54 fallita solo confronto byte CRLF/LF, corretto. Working changes salvati in stash e backup privati .orbit-backups/1.18.1/workspace/restore.json. Preferenze VSCode e market saranno ripristinati a fine packaging; gli edit Remote/Solco sono gia nei commit di release, confrontati ignorando solo CRLF/LF.
- Tutti6 sdkconfig/compile_commands della matrice automatica sono gia produzione (WATCH_DEV_CONSOLE false, nessuna WATCH_LOCAL_QA_CONSOLE). Build esistenti ora ricompilate per1.18.1 dai main puliti. Driver orbit_cached_release.py, coda orbit_wait_cached_prepare.py, session63941/job926d59ed-49bb-4c1f-a9bd-5050bc4a36b1. Output *-cached-build.log e *-verify.log. boards.json e cached-boards.json puntano ai progetti originali; isolated-boards.json conserva riferimenti precedenti.
- Backup completo1.8 OSslot0 finito. Stub esptool fallisce sempre leggendo0x31d000; ROM --no-stub supera il punto, quindi backup prosegue conROM e chunk256KiB. Nessuna scrittura ancora a questo checkpoint. BackupROM session53238/job924018f4-818d-4705-970b-2b4b3dc565f4. Backupprecedenti9d2adfdd e1eada673 EXIT1: non conteggiare come completi.
- Orchestratore finale orbit_finish_release.py, session23010/jobbb01b481-bd41-45f8-934a-cea10ca96fd9. Attende actualEXIT0 build/preflight e backup, poi monitor flash/boot2, pubblicazione6, download/audit6. Infine ripristina preferenze e market anche in caso errore. Stato persistente finalization-status.json. NON avviare un secondo flash/publish in parallelo.
- Successo finale richiede hardware-verified.json per2 e ota-verified.json per6 piu actualEXIT0 orchestratore. Quest'ultimo confronta binari pubblici con locali, SHA catalogo, manifest e tag commit sorgenti.
- La release include il lavoro implementato; sveglie condivise ancora escluse e dichiarate nelle note. Non dichiarare tutta ORBIT-ESPNOW.md completata. Nessun eraseflash/NVS.
- Review read-only routes_finish ha suggerito di conservare lo slot attivo. I metadata confermano OS1.18.0 in entrambi gli slot di entrambi i device: flash helper aggiornato per scrivere SOLO slot1 inattivo e mantenereOS0; verify-flash prima della selezione tramite orbit_select_os.py (sequenza/CRC ufficiali Espressif, stesso adapter keyword gia usato da Solco). Solo otadata cambia oltre allo slot1, mai NVS/partition table. Non dichiarare rollback automatico del bootloader non verificato.
- Esiti reali: build/preflight926d59ed actualEXIT0 (6/6); backupROM924018f4 actualEXIT0 (2/2); flash/boot75127298-77e1-47b7-a06e-7a2ab289e273 actualEXIT0 (2/2). hardware-verified.json identifica slot1 offset0x620000 su entrambi, OS0 conservato. Adapter OTA letto prima con --check exit0, poi selezione effettiva riuscita.
- Boot30s: versione1.18.1 e Ready su entrambi, nessun panic/assert/watchdog. Osservazione [ready] 1.8=120KB interni liberi/largest62KB/min56KB;1.75C=45KB/largest31KB/min38KB. Sono due osservazioni di avvio con impostazioni mantenute, NON la matrice completa dei sei stati radio.
- Mappe produzione6 e sourceproof rifatti con9ed485c5-1e45-40b6-b6ac-1b99af080a24 actualEXIT0, output orbit-os-memory.json; precedente salvato stable-1.18.1/before-release-memory.json.
- Pubblicazione OTA ora attiva job7fec25a6-3daa-4ee9-bcd0-f724113aae15, avviata solo dopo flash/boot. Attendere orchestratorebb01b481 e audit prima di dichiarare release conclusa.


## Release 1.18.1 conclusa e verificata

- Flash e boot2/2 actualEXIT0; OS precedente conservato nello slot0 su entrambi, nuovo attivo slot1. Nessuna scrittura NVS/partition table. Backup privati completi conservati fuori repository.
- Pubblicazione6/6 job7fec25a6-3daa-4ee9-bcd0-f724113aae15 actualEXIT0. Audit remoto3038471b-3e68-43df-95c9-d879367a7632 actualEXIT0: binari pubblici identici ai locali, SHA/size cataloghi, manifest e tag sorgenti corrispondenti.
- hardware-verified.json2 e ota-verified.json6 in .release-work/stable-1.18.1. Ripristinate preferenze VSCode e modifica QA market preesistenti; stashes/backup di sicurezza conservati.
- Perimetro della release chiuso. Restano software futuro: sveglie condivise e ingressi residui della roadmap; verifiche hardware estese radio/audio/memoria. Non marcarli come completati dalla sola release.
