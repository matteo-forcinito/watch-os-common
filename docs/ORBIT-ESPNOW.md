# Orbit su ESP-NOW: un solo bus tra i dispositivi, Bluetooth per la companion

Documento tecnico, 5 novembre 2026. Stato: proposta approvata, da sviluppare per fasi.
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
- Task del bus: uno, con stack interno da 6 KB, solo durante un lease; code e stato in PSRAM.
- Ascolto continuo solo sui nodi alimentati; i nodi a batteria partecipano a finestre (a schermo
  acceso o su evento), come oggi la Famiglia.
- Budget da fissare dopo la fase 1, con misure su 1.75C, AMOLED 1.8 e Watcher.

## 6. Fasi e criteri di accettazione

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
