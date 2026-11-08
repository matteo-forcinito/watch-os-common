# Orbit su ESP-NOW: un solo bus tra i dispositivi, Bluetooth per la companion

Documento tecnico, aggiornato il 8 novembre 2026. Stato: fase 1b verificata su 1.8 e 1.75C;
fase 2 verificata host e su queste due board (Watcher aperto); fase 3 parzialmente implementata,
verificata automaticamente e compilata sulle sei board; fase 4 migrata al bus,
verificata in build sulle sei board per Remote e Solco; fasi 5 e 6 parziali. Checkpoint: `ORBIT-ESPNOW-PROGRESS.md`.
Riferimenti: `ECOS.md` (visione), `ECOS-PAIRING.md` (abbinamento EC01), `ECOS-CHIUSURA.md`
(trasporto Famiglia EL02), `ORBIT-LINK.md` (rotte HTTP attuali), `REMOTE-CONTROL.md`.

## 1. Obiettivo

Oggi l'ecosistema usa insieme Wi-Fi, Bluetooth ed ESP-NOW. Accesi insieme consumano quasi
tutta la RAM interna, e il Wi-Fi o il Bluetooth spesso non riescono ad accendersi.
L'obiettivo è **più affidabilità e stabilità senza togliere funzioni**:

- **ESP-NOW** come unico canale tra dispositivi (dorsale dell'ecosistema);
- **Bluetooth LE** solo verso la companion (telefono, PC);
- **Wi-Fi** solo su richiesta, per Internet (meteo, Market, NTP, radio, aggiornamenti);
- le funzioni di **Orbit** che oggi vivono nella companion passano sui dispositivi: l'ecosistema
  funziona anche a companion spenta;
- la **companion** diventa un nodo della famiglia e, quando c'è, l'**orchestratore intelligente**.

Regola già in vigore dalla 1.18.0: il dispositivo ha **un solo nome** ovunque
(`settings_device_name()`), e companion e Remote condividono un solo abbinamento Bluetooth.

## 2. Situazione attuale

| Funzione | Trasporto | Codice |
|---|---|---|
| Famiglia ECOS (trova, suona, messaggi) | ESP-NOW cifrato EL02, in alternativa LAN UDP | `services/ecos/ecos_link.c` |
| Abbinamento ECOS | ESP-NOW EC01 (ECDH P-256, codice a 6 cifre) | `services/ecos/ecos.c` |
| Orbit Link (scene, eventi, timer, memo, altoparlante, microfono) | HTTP sul Wi-Fi di casa, token | `apps/link/` |
| Companion (notifiche, musica, ora, controllo) | Bluetooth LE (NimBLE) | `companion/ble_companion.c` |
| Remote e Deck | Bluetooth HID + GATT Deck; hub multi-orologio su ESP-NOW | `hid_link/` |
| Kit di Solco (audio condiviso) | ESP-NOW | `solco/main/link.c` |

Tre implementazioni ESP-NOW indipendenti (ECOS, hub di Remote, Solco), ognuna con il proprio
formato, la propria cifratura e la propria gestione del canale. Orbit Link dipende dal router:
DHCP, server web sempre acceso, mDNS.

**Misure del 5/11 sulla AMOLED 1.75C** (log di avvio, `sys_heap_log` e splash):

- RAM interna libera con il Wi-Fi spento: 107 KB; durante la connessione Wi-Fi per l'NTP: 37-47 KB;
- dalla fine della UI (`[ui]` 129 KB) a fine avvio (`[ready]` 38 KB) quando il Bluetooth riserva la sua memoria;
- con le due radio accese restano poche decine di KB, frammentati: è la causa delle accensioni
  fallite di Wi-Fi o Bluetooth.

**Fase 1, Watcher (5/11, comando QA `radios`, valori in KB di RAM interna):**

| Stato | Libera | Blocco più grande | Bluetooth attivo | Radio Wi-Fi |
|---|---|---|---|---|
| Radio spente | 102 | 56 | no | no |
| Bluetooth | 68 | 31 | sì | no |
| Bluetooth spento | 102 | 34 | no | no |
| ESP-NOW | 48 | 31 | no | sì |
| ESP-NOW + Bluetooth | 48 | 31 | **non parte** | sì |
| Wi-Fi collegato | 48 | 31 | no | sì |
| Wi-Fi + Bluetooth | 48 | 31 | **non parte** | sì |
| Radio spente (fine) | 101 | 31 | no | no |

- Il Bluetooth costa circa 34 KB; il driver Wi-Fi circa 54 KB, **uguale per ESP-NOW e Wi-Fi collegato**:
  il costo sono i buffer del driver, non il collegamento al router.
- Con la radio Wi-Fi accesa il controller Bluetooth non trova memoria e non si avvia.
- Il blocco più grande non torna a 56 KB dopo lo spegnimento: la memoria resta frammentata.

**Fase 1, AMOLED 1.75C (5/11, stesso comando, firmware 1.18.0 + banco QA):**

| Stato | Libera | Blocco più grande | Minimo dall'avvio | Bluetooth | Radio Wi-Fi |
|---|---|---|---|---|---|
| Inizio (Bluetooth già acceso) | 72 | 42 | 28 | sì | no |
| ESP-NOW | 47 | 28 | **8** | sì | **non parte** |
| Wi-Fi collegato | 47 | 28 | 8 | sì | **non parte** |
| Fine | 47 | 28 | 8 | sì | no |

- Il Bluetooth resta acceso anche quando il banco lo rilascia (la companion lo tiene attivo),
  quindi qui si vede il caso opposto al Watcher: **con il Bluetooth acceso il Wi-Fi non parte**.
- L'accensione fallita del Wi-Fi **non restituisce circa 25 KB** (72 -> 47 KB, blocco 42 -> 28 KB) e il
  minimo scende a 8 KB: dopo un tentativo fallito il dispositivo resta con meno memoria fino al riavvio.
- Il messaggio `Disconnected ... errore di connessione (8)` all'avvio non è un errore: è il motivo 8
  (uscita volontaria) che compare ogni volta che il sistema spegne il Wi-Fi dopo l'NTP.

**Fase 1, AMOLED 1.8 (5/11, stesso comando):**

| Stato | Libera | Blocco più grande | Bluetooth attivo | Radio Wi-Fi |
|---|---|---|---|---|
| Radio spente | 115 | 62 | no | no |
| Bluetooth | 81 | 42 | sì | no |
| Bluetooth spento | 115 | 62 | no | no |
| ESP-NOW | 48 | 31 | no | sì |
| ESP-NOW + Bluetooth | 48 | 31 | **non parte** | sì |
| Wi-Fi collegato | 48 | 31 | no | sì |
| Wi-Fi + Bluetooth | 48 | 31 | **non parte** | sì |
| Radio spente (fine) | 114 | 52 | no | no |

Stesso quadro del Watcher: Bluetooth 34 KB, driver Wi-Fi 66 KB (ESP-NOW e Wi-Fi collegato uguali),
Bluetooth che non parte con la radio Wi-Fi accesa, blocco più grande che non torna al valore iniziale.

**Configurazione del driver Wi-Fi oggi** (log di avvio, uguale sulle tre board): 16 buffer statici di
ricezione da 1600 byte (25 KB, sempre allocati), 32 dinamici di ricezione, 16 statici di trasmissione,
32 di cache in trasmissione, 32 buffer corti di gestione. Per ESP-NOW e per il traffico di un orologio
sono dimensionati come per un access point: è il primo punto della fase 1b.

Conseguenza: ESP-NOW da solo non libera memoria. Prima del bus serve la **fase 1b** (sezione 6),
compreso il rilascio completo della memoria quando l'avvio del Wi-Fi fallisce (1.75C).

**Fase 1b, fatta il 5/11** (AMOLED 1.8, 1.75, 1.75C, Knob, Watcher; Guition 4848 aveva già i buffer ridotti):

- driver Wi-Fi come su Guition e LCD 1.3: 4 buffer statici di ricezione, 4 di trasmissione, 8 di cache,
  16 dinamici di ricezione, finestra di riordino 6 (`sdkconfig.defaults` di ogni board);
- all'avvio il Bluetooth parte **prima** della sincronizzazione dell'ora (`main.c` delle sei board):
  il controller vuole un blocco contiguo di circa 38 KB (il controllo in `ble_companion.c` chiede 40 KB)
  che il driver Wi-Fi, partito prima, divideva.

| Board | Stato | Libera | Blocco più grande | Bluetooth | Radio Wi-Fi |
|---|---|---|---|---|---|
| AMOLED 1.8 | ESP-NOW | 88 (prima 48) | 46 | no | sì |
| AMOLED 1.8 | ESP-NOW + Bluetooth | 54 | 31 | **sì** | sì |
| AMOLED 1.8 | Wi-Fi + Bluetooth | 53 | 31 | **sì** | sì |
| 1.75C | Bluetooth all'avvio | 72 | 25 | sì | no |
| 1.75C | ESP-NOW + Bluetooth | 46 | 25 | **sì** | **sì** |
| 1.75C | Wi-Fi + Bluetooth | 46 | 25 | **sì** | **sì** |

Il driver Wi-Fi passa da 66 a 27 KB di RAM interna. Accettazione della fase 1b raggiunta su 1.8 e 1.75C;
il Watcher va rimisurato quando torna collegato. Con tutte le radio accese restano 46-54 KB liberi:
il bus ECOS (fase 2) deve stare dentro pochi KB interni e tenere i buffer in PSRAM.

**Cleanup completato nel codice il 6/11:** i fallimenti di init, storage, modalità
e start rilasciano il driver e le interfacce di rete possedute. Lo spegnimento
pulisce anche un avvio parziale; errori di stop/deinit e coda sys piena vengono
riprovati dal timer esistente. Una radio già usata da altri client viene conservata.
Test host sulle funzioni C di produzione con fault injection superati
(`python components/services/wifi/tests/run_driver_tests.py`). Il recupero effettivo
dell'heap su errore/OOM richiede ancora una misura sul dispositivo: le tabelle
precedenti restano misure storiche, non risultati di questa correzione.

### Fase 2: implementazione compatibile EC01/EL02 (5/11)

Il bus `services/ecos/ecos_bus.c` è ora integrato nella build. Abbinamento EC01 e
Famiglia EL02 usano un solo proprietario di ESP-NOW: primo servizio aperto = lease radio,
ultimo chiuso = deinizializzazione e rilascio. Il bus smista per intestazione a quattro byte;
le chiavi, la cifratura, gli ACK applicativi e la protezione replay restano nei servizi.
Il formato EB01 descritto sotto resta il progetto delle fasi successive, non è ancora trasmesso.

- Nessun task o coda del bus; tabella di quattro servizi e mutex statici (poche centinaia
  di byte interni: 190 byte di simboli statici nella build AMOLED 1.8, esclusi
  eventuali padding del linker; nessuna allocazione heap del bus neppure dopo il primo utilizzo).
- Code RX dell'abbinamento e RX/TX della Famiglia in PSRAM. La chiusura disabilita il
  servizio e aspetta le callback già avviate prima che il chiamante liberi le code.
- Potenza: minimo dei limiti richiesti, senza superare il valore precedente della radio;
  ripristino quando il limite viene tolto. Errori di registrazione delle callback annullano
  l'avvio e restituiscono il lease.
- L'abbinamento continua a sospendere Famiglia durante la ricerca: i due servizi condividono
  la radio, ma non possono cercare contemporaneamente su canali diversi.
- Gli esiti TX sono attribuiti al solo servizio che ha inviato, con generazione
  dello slot e un singolo invio radio pendente. La chiusura non attribuisce un
  esito tardivo a un servizio riaperto, anche se usa lo stesso MAC.
- Hub Remote e Solco restano firmware separati ma usano lo stesso bus; le
  maschere RX preservano i formati legacy RH/RP e SL02. Build in verifica.

Test host del codice C di produzione: `python components/services/ecos/tests/run_bus_tests.py`
(MSVC su Windows, `--vcvars` per un'installazione diversa). Copertura: lease multipli,
smistamento, limiti e ripristino potenza, errori d'avvio, tabella piena e callback in corso.

Console locale QA (`WATCH_LOCAL_QA_CONSOLE=1`, esclusa dalle build normali):
`family on|off`, `family refresh`, `family ring <id>`, `family stop <id>`,
`family message <id>`, `family`. Il messaggio di prova fisso è `Orbit bus QA`;
lo snapshot espone solo la presenza di quel messaggio, senza stampare altre notifiche.
Le prove della fase 2 si possono fare a coppie, sostituendo un dispositivo alla volta:
non occorrono tre collegamenti USB simultanei.

**Prova hardware 1.8 e 1.75C superata (5/11, build locale QA 1.18.0):**
ricerca, suona, ferma e messaggio ricevuto in entrambe le direzioni, sul canale 1,
con entrambe le station scollegate dal router. Integrità heap verificata prima,
durante e dopo. Abbinamenti e NVS conservati; aggiornato solo lo slot Watch OS,
lasciando Remote e l'altra copia OS dove presenti.

| Board | RAM interna prima | Durante il lease | Dopo la prova | Blocco massimo dopo |
|---|---|---|---|---|
| AMOLED 1.8 | 115,8 KiB | 82,3 KiB | 114,4 KiB | 36 KiB |
| AMOLED 1.75C | 72,8 KiB | 40,2 KiB | 72,5 KiB | 25 KiB |

Questa prova comprende notifiche e schermata dello squillo: il confronto complessivo
non isola da solo il costo del bus. Il bus ha 190 byte statici e nessuna allocazione
heap propria; i task e le code dei servizi vengono rilasciati alla chiusura.

**Accettazione hardware ancora aperta sul Watcher**: ripetere trova/suona/messaggi
e confronto health a Wi-Fi scollegato, collegandolo in coppia con una AMOLED.
Una build o un test host non certificano la prova sul dispositivo assente.

### Fase 3: primo ingresso Orbit sui dispositivi (6/11)

Il trasporto transitorio usa EL02 anche per Orbit (tipi 9 richiesta e 10 risposta),
senza introdurre un secondo proprietario della radio o un worker permanente.
Ogni corpo contiene un ID di richiesta non nullo a 64 bit e 1–144 byte opachi;
la risposta deve corrispondere sia all'ID sia al MAC abbinato. Le chiavi e il nonce
restano quelli EL02, e il replay viene persistito prima di eseguire un comando.

Il mittente attende una risposta applicativa e può ritrasmettere lo stesso frame
cifrato tre volte in totale. La cache di risposta per peer vive nella sessione in
PSRAM: una ritrasmissione con la stessa epoca/sequenza riceve la risposta già calcolata,
senza ripetere l'azione. Dopo un riavvio la protezione replay persistente impedisce
di ripetere l'azione, ma la cache non è conservata: un timeout resta un esito ignoto,
non autorizza a ripetere automaticamente il comando con un nuovo ID.

Il dispatcher applicativo riusa gli ingressi HTTP per scene, timer e memo. Sono
disponibili richieste JSON compatte `scene`, `events`, `timer`, `memo`; eventi e nomi
dei memo sono letti uno per volta. Non vengono troncati messaggi o risposte JSON:
superare il limite produce errore esplicito. Nella schermata di un membro sono
disponibili **Mostra aura** e **Cancella scena**, con esito nelle notifiche.

Test host codec e cache replay superati; test del dispatcher e build sono riportati
nel checkpoint con il loro esito effettivo. Non sono state eseguite nuove prove radio
né flash: 1.8 e 1.75C sono in download mode.

Sono ora collegati anche la frammentazione OF01 (richieste fino a 1024 byte,
risposte fino a 144), il coordinatore deterministico e lo stato LWW in PSRAM.
Il contatore Lamport riserva blocchi durevoli in NVS; il gossip confronta riassunti
e trasferisce solo differenze, con un worker transitorio e senza accendere una
radio spenta. I tombstone impediscono la ricomparsa di chiavi cancellate.
Le chiavi e i valori restano in PSRAM: dopo un reboot si recuperano dai peer svegli.

In Ecosistema sono disponibili **Aura di famiglia**, **Timer famiglia 60 s** e
**Ferma timer famiglia**. Il primo nodo secondo alimentazione, batteria, nome e MAC
coordina il fanout. Un ACK "in coda" non certifica l'esecuzione su tutta la famiglia;
il coordinatore pubblica il conteggio di esiti accettati, rifiutati e ignoti.
La precedenza della companion come nodo ECOS e il proxy del gateway sono implementati e verificati automaticamente; resta la prova radio su hardware.

La prima build integrata 1.8 è superata; la nuova matrice delle sei board è nel
checkpoint. Test host verificano frammenti/duplicati, modello e gossip, dispatcher,
coordinatore, attesa delle RPC e fallimenti del timer. Non costituiscono prove radio.
L'instradamento autonomo degli eventi e l'interfono sono integrati e verificati
con test host sulle funzioni di produzione; resta il collaudo radio.
Le letture sensori cached sono esposte come `sensor` sul bus e
`/api/link/sensors?part=accel|gyro|temperature` su BLE/HTTP: valori interi in
mg, mdps e mC, età in millisecondi e freschezza esplicita entro un secondo.
Queste letture non avviano l'IMU e non trasformano un campione vecchio in uno nuovo.
Il codec ADPCM comune e i pacchetti indipendenti da 15 ms sono verificati su host;
trasporto e lifecycle dell'interfono sono integrati: sessione autenticata,
half duplex massimo 30 secondi, jitter buffer limitato in PSRAM, stop/timeout
e mutua esclusione con registrazione memo e audio HTTP. L'interfaccia del membro
offre una conversazione di 10 secondi e lo stop. Latenza e qualita richiedono hardware.

Le rotte input sono chiavi LWW `ecos/route/<MAC sorgente>/<tipo>`, per tap, dial,
dial_done e orbi. Si configurano dalla schermata del membro oppure tramite
`/api/link/route`. Il destinatario applica l'input alla scena e registra il MAC
sorgente senza reinoltrarlo. Coda PSRAM limitata a otto eventi, nessun retry
con nuovo ID su esito ignoto; lo stream audio sospende nuovi inoltri.

Le sei build OS del 7 novembre sono superate e puntano al checkout condiviso.
Sono verifiche software: non sostituiscono la scena coordinata su tre dispositivi
e l'interfono tra due richiesti dai criteri di accettazione.

### Remote e Solco (fase 4 parziale)

I due firmware aprono un servizio sul bus comune e non possiedono piu
direttamente ESP-NOW. Il bus gestisce lease, callback, smistamento e invii.
Remote conserva il pairing legacy e offre **Usa membro ECOS**: il consenso HID
va dato su entrambi i dispositivi. La chiave Remote deriva dalla credenziale ECOS
con dominio separato e MAC ordinati; la rimozione o rotazione della credenziale
invalida il consenso al riavvio. Nessun accesso automatico al PC per la famiglia.
Test host su grant/NVS/rollback superati, build firmware in corso. Le code Remote
sono in PSRAM; la gestione comune del canale e le prenotazioni sono implementate e verificate host e nelle sei build Remote/Solco. Solco conserva le chiavi derivate dalle credenziali ECOS
e usa un adapter radio standalone, senza server IP o componente servizi completo.
I test di rollback startup e teardown passano; tutte le sei build Solco sono
superate. La matrice Remote dopo tuning RAM e ancora in verifica; risultati
effettivi e correzioni delle configurazioni generate sono nel checkpoint.

### Ponte di famiglia e companion (fase 5 parziale)

Il ponte BLE esistente, riservato a un collegamento bonded e cifrato, raggiunge
`GET /api/link/family` e `POST /api/link/relay` tramite un handler diretto.
Queste richieste, scene, timer, eventi e letture sensori non richiedono il server
HTTP neppure sul gateway. Il fallback delle altre API conserva il loopback
esistente, avviato solo al bisogno e chiuso dopo otto secondi senza richieste HTTP.
I peer ricevono RPC EL02 cifrate e non avviano server IP o connessioni al router.
Il relay accetta soltanto i percorsi Orbit previsti, con limiti espliciti;
un timeout viene restituito come esito ignoto senza ripetere il comando.
Scene, timer, eventi e metadati dei peer sono accessibili dalla companion tramite
un solo collegamento Bluetooth. Le richieste di audio PCM e API non migrate
restano escluse dal relay. Il cursore degli eventi avanza sul singolo evento
restituito, senza saltare il backlog.

La companion crea collegamenti di famiglia distinti, disabilita il bootstrap
Wi-Fi per questi peer e rimuove i collegamenti quando il gateway sparisce;
può riscoprirli attraverso un altro ponte connesso. Test Flutter dedicati superati.
La companion dispone anche di identita ECOS persistente e pairing EC01 via NUS,
con lo stesso ECDH/commitment/SAS e consenso su entrambi i nodi. Salvataggio
fallito, disconnessione o sessione obsoleta non producono una conferma SAVED.
Test host C, test Dart e prova automatica dell'interfaccia di consenso superati.
Il nuovo ponte trasporta frame EL02 autenticati con le credenziali proprie del
nodo: epoch persistito prima dell'invio, replay persistito prima della consegna.
Codec verificato contro fixture Python AES-GCM indipendente; integrazione C e
build aggiornate sono in verifica. Si mantiene EL02 compatibile invece di
introdurre un secondo formato EB01 prima di chiudere questa transizione.
La priorita del companion usa uno status EL02 autenticato, legato al peer BLE
attivo e fresco entro30 secondi. Il gateway annuncia la presenza come proxy ai
suoi peer: questi possono eleggerlo senza una seconda chiave con il telefono.
Il gateway inoltra i lavori al companion, che invia solo operazioni locali di
scena/timer; non avvia una nuova elezione. Un solo lavoro e ammesso alla volta;
frammenti e comandi ripetuti non duplicano gli effetti. Al distacco il gateway
invalida subito la presenza e torna al coordinamento locale.
I49 test companion e i test C di presenza/delega sono superati; le build finali
aggiornate sono concluse: OS, Remote e Solco hanno sei target ciascuno con exit 0. Il canale comune e verificato automaticamente; le prove fisiche restano aperte.
Nessuna nuova prova BLE o radio e stata eseguita sui dispositivi.

## 3. Vincoli tecnici

1. **ESP-NOW vive nel driver Wi-Fi.** Richiede `esp_wifi_init/start` (radio in modalità station,
   non collegata), ma non l'associazione al router, lo stack IP, il server HTTP o mDNS. Costa
   molto meno del Wi-Fi collegato, non zero. Lo stesso conteggio di riferimento di
   `wifi_radio_acquire()` usato oggi da ECOS.
2. **Canale.** Tutti i nodi devono stare sullo stesso canale. Un nodo collegato al router è
   vincolato al canale del router. Politica già usata dall'abbinamento: chi è collegato resta sul
   suo canale, gli altri lo cercano visitando 1, 6, 11, poi gli altri.
3. **Dimensioni.** ESP-NOW v1: 250 byte per frame; v2 (ESP-IDF 5.4 e successivi, quindi anche 6.1):
   fino a 1470 byte. Frame più grandi vanno frammentati.
4. **Banda.** Qualche centinaio di kbit/s utili, con perdite: sufficiente per comandi, stato, scene
   ed eventi; per l'audio serve compressione (ADPCM 4 bit a 16 kHz = 64 kbit/s). Il kit di Solco
   fa già passare audio su ESP-NOW.
5. **Cifratura.** La cifratura nativa di ESP-NOW ha un numero limitato di peer cifrati:
   si usa la cifratura applicativa AEAD già scritta per EL02 (AES-GCM, nonce epoca + sequenza,
   protezione replay persistente), con chiavi derivate dalle credenziali di abbinamento.
6. **Telefono e PC non parlano ESP-NOW.** La companion Flutter (Android, Windows) usa il Bluetooth.
   Per il PC esiste in alternativa un ponte USB con un ESP32 (per esempio la Deck Bar).
7. **Internet resta sul Wi-Fi**, acceso per l'operazione e spento subito dopo, come la
   sincronizzazione dell'ora dalla 1.18.0 (`wifi_release_now`).

## 4. Architettura

### 4.1 Il bus ECOS

Un solo componente (`services/ecos/ecos_bus.c`) possiede ESP-NOW. ECOS, l'hub di Remote, il kit di
Solco e le funzioni di Orbit diventano **servizi** sul bus, non più proprietari della radio.

Frame (versione esplicita, nessuna struct C trasmessa):

| Campo | Byte | Note |
|---|---|---|
| Magic + versione | 4 | `EB01` |
| Tipo | 1 | dati, ACK, stato, frammento |
| Servizio | 1 | famiglia, orbit, remote, solco, sistema |
| Mittente, destinatario | 6 + 6 | MAC; destinatario tutto `FF` per i broadcast |
| Epoca + sequenza | 8 + 4 | nonce AEAD e protezione replay (schema EL02) |
| Frammento | 2 | indice/totale per messaggi oltre un frame |
| Payload cifrato + tag | n + 16 | AES-GCM con chiave per coppia o di famiglia |

Due classi di consegna:

- **affidabile** (comandi, sveglie, scene): ACK applicativo, ritrasmissione con backoff,
  deduplica per sequenza; l'ACK radio di ESP-NOW dice solo "trasmesso";
- **a perdere** (stato periodico, eventi di presenza, audio): nessun ACK, il pacchetto
  successivo sostituisce il precedente.

La radio è accesa solo mentre un servizio la tiene (lease), con lo stesso conteggio di riferimento
del Wi-Fi. A riposo: nessun task, nessuna radio (budget ECOS: 0 KB interni in più).

### 4.2 Servizi sul bus

| Servizio | Contenuto | Classe |
|---|---|---|
| Famiglia | trova, suona/ferma, messaggi rapidi, informazioni (oggi EL02) | affidabile |
| Orbit, scene | `pulse`, `aura`, `beam`, `text`, `light`, `orbi`, `dial`, `clear` | affidabile |
| Orbit, eventi | tocchi sulle scene, `dial`, `tap`, `orbi` | affidabile, in coda |
| Orbit, timer e memo | come `/api/timer` e `/api/voice` | affidabile |
| Orbit, audio | altoparlante e microfono tra dispositivi (interfono), ADPCM | a perdere |
| Remote hub | inoltro tra orologi verso il PC | affidabile |
| Solco kit | clock e audio del kit | a perdere |
| Stato di casa | chiavi `ecos/...` (vedi `ECOS.md` 2.3), ultima scrittura vince | gossip |

Le rotte HTTP di Orbit Link restano durante la transizione, implementate sopra gli stessi servizi:
stessa logica, due ingressi.

### 4.3 Orchestrazione sui dispositivi

Oggi stato, tempi e instradamento vivono nella companion Orbit. Si spostano sui dispositivi, nel
rispetto della decisione di `ECOS.md` 2.1 (nessun nodo speciale per ruolo):

- **Stato condiviso**: archivio chiave-valore replicato in PSRAM, con orologio logico
  (contatore + autore), ultima scrittura vince; i nodi si scambiano riassunti e differenze.
- **Coordinatore per regola, non per elezione**: per ogni lavoro "di tutti" (scene su più
  dispositivi, interfono, timer condivisi) ogni nodo calcola la stessa regola sui nodi svegli:
  prima la companion, se presente; poi chi è alimentato (Watcher, Knob); poi chi ha più batteria;
  poi il nome. Il primo coordina; se sparisce, al giro dopo tocca al successivo.
- **Funzioni Orbit portate sui dispositivi**: instradamento degli eventi (chi riceve il tocco o la
  ghiera di un altro dispositivo), scene coordinate (la stessa aura su più orologi), interfono,
  timer e sveglie di casa.

### 4.4 La companion: nodo e orchestratore

- **Nodo**: si collega via Bluetooth a un dispositivo della famiglia, che fa da ponte verso il bus
  (servizio GATT che trasporta gli stessi frame del bus, con le stesse chiavi). Abbinamento con il
  codice a 6 cifre, come gli altri nodi.
- **Orchestratore intelligente quando c'è**: automazioni "quando… allora…", logiche che richiedono
  Internet o più calcolo (AI, calendario, notifiche), interfaccia di configurazione. Ha la
  precedenza nella regola di coordinamento.
- **Quando manca**: i dispositivi continuano con il coordinamento di base; nessuna funzione locale
  si ferma.

### 4.5 Wi-Fi solo su richiesta

Il Wi-Fi si accende per un'operazione Internet e si spegne subito dopo. Il canale ESP-NOW segue il
router mentre un nodo è collegato. A Wi-Fi spento si torna al canale comune della famiglia,
annunciato nello stato di casa.

## 5. Memoria ed energia

- Fine principale: **non avere mai Wi-Fi collegato e Bluetooth attivi insieme per lavoro tra
  dispositivi**. Restano insieme solo il bus ESP-NOW (radio Wi-Fi senza IP) e il Bluetooth verso
  la companion.
- Fase 2: nessun task del bus; i servizi mantengono i propri task e code in PSRAM.
  Per le fasi successive, un eventuale worker condiviso ha un budget previsto di 6 KB interni
  solo durante un lease; dimensione da confermare con misure.
- Ascolto continuo solo sui nodi alimentati; i nodi a batteria partecipano a finestre (a schermo
  acceso o su evento), come oggi la Famiglia.
- Budget da fissare dopo la fase 1, con misure su 1.75C, AMOLED 1.8 e Watcher.

## 6. Fasi e criteri di accettazione

Checklist operativa (gli esiti automatici non sostituiscono il collaudo radio):

| Attivita | Stato effettivo | Prossima verifica |
|---|---|---|
| Bus unico, smistamento EC01/EL02/Remote/Solco, rollback Wi-Fi | Completato e verificato automaticamente | Concorrenza radio su hardware |
| Stato replicato, scene, eventi, timer, memo, sensori, rotte e interfono | Completato e verificato automaticamente; OS6 finali compilati | Collaudo scena3 nodi/interfono2 e memoria runtime |
| Migrazione Solco | Completato e verificato automaticamente; build6 superate | Clock/audio sui dispositivi |
| Migrazione Remote e consenso HID da credenziale ECOS | Completato e verificato automaticamente; build6 finali superate | Collaudo HID |
| Famiglia e relay Orbit attraverso un solo BLE senza HTTP/router | Completato e verificato automaticamente | Collaudo BLE e cache Windows |
| Nodo companion EC01, identita e consenso persistenti | Completato e verificato automaticamente; OS6 finali superati | Pairing fisico |
| Ponte frame EL02 con chiavi proprie della companion | Completato e verificato automaticamente; OS6 e Windows debug superati | Integrazione fisica BLE/ESP-NOW |
| Priorita companion, proxy gateway e handler coordinatore | Completato e verificato automaticamente; OS6 e Windows debug superati | Collaudo failover e fanout tra nodi |
| Canale comune dinamico e annuncio nello stato replicato | Completato e verificato automaticamente: pin condivisi, annuncio LWW, Remote/Solco e 18 build exit 0 | Verifica radio fisica ancora aperta |
| Sveglie di casa | Mancante: replica e ingressi RPC; servizio locale in correzione e test | Persistenza atomica, identita condivisa e gestione one-shot senza riattivazioni |
| API residue e server web solo su richiesta | Parzialmente implementato | Completare ingressi necessari; fallback locale8s per API residue |
| Heap reale, coesistenza radio e qualita audio | Bloccato da collaudo hardware | Nessun flash autorizzato; Watcher non disponibile |

1. **Misure** (nessuna modifica funzionale). RAM interna libera e blocco più grande in sei stati:
   radio spente, solo Bluetooth, solo ESP-NOW, ESP-NOW + Bluetooth, Wi-Fi collegato,
   Wi-Fi + Bluetooth. Su 1.75C, AMOLED 1.8, Watcher. Esito: tabella in questo documento.
1b. **Memoria del driver Wi-Fi**: meno buffer statici di ricezione, buffer dinamici e di
   trasmissione in PSRAM dove il driver lo consente, allocazione delle radio in ordine fisso
   (Bluetooth prima del Wi-Fi) per limitare la frammentazione. Accettazione: `radios` mostra
   Bluetooth attivo in ESP-NOW + Bluetooth e Wi-Fi + Bluetooth sulle tre board.
2. **Bus ECOS** con il servizio Famiglia portato sopra (compatibilità EL02 per una versione).
   Accettazione: trova/suona/messaggi tra 1.75C, 1.8 e Watcher a Wi-Fi spento; 0 KB interni a riposo.
3. **Orbit sui dispositivi**: scene, eventi, timer, memo sul bus; coordinatore per regola;
   interfono ADPCM. Accettazione: scena coordinata su tre dispositivi e interfono tra due, senza
   router né companion.
4. **Remote hub e kit di Solco** sul bus (stesso canale, stesse chiavi).
5. **Companion nodo** via Bluetooth (ponte GATT) e orchestratore. Accettazione: la companion vede
   e comanda tutta la famiglia attraverso un solo dispositivo.
6. **Server web non più necessario** tra dispositivi: resta per la dashboard, acceso su richiesta.

Ogni fase: build delle sei board, prove sul dispositivo, misura RAM prima/dopo nel report health
(che dalla 1.18.0 elenca anche i lock di potenza).

## 7. Rischi aperti

- **Canale**: due nodi collegati a router diversi non si sentono; serve una regola di priorità
  (rete di casa nota) e un messaggio chiaro all'utente.
- **Perdite ESP-NOW a corta distanza**: a 20 dBm il ricevitore satura (misurato durante
  l'abbinamento); potenza adattiva sul bus.
- **Bluetooth su Windows**: cache dei servizi; dalla 1.18.0 l'indicazione Service Changed risolve il
  passaggio companion/Remote, da verificare anche con il ponte GATT.
- **Interfono**: latenza e perdite su ESP-NOW a pacchetti piccoli; prova di qualità prima di
  sostituire il percorso HTTP.
- **Compatibilità**: frame `EB01` nuovi; i dispositivi non aggiornati restano raggiungibili via
  EL02 e HTTP finché la transizione non è chiusa.
