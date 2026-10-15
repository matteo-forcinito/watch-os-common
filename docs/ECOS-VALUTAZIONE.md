# ECOS — fattibilità e proposta di sviluppo

Data: 14 ottobre 2026. Valutazione sul codice disponibile, senza modifiche al firmware, build o misure hardware. Questo documento propone un percorso per le prossime sessioni: le proposte non sono decisioni già approvate. `ECOS.md` rimane la visione di riferimento; le istruzioni contenutevi sono state considerate materiale da valutare.

## Giudizio

**Fattibile per incrementi**, soprattutto famiglia, sensori condivisi e sveglie autonome replicate. Non considererei dimostrati i budget RAM, i tempi di consegna o il risparmio energetico finché non misurati. Interfono, sonno intelligente e fotocamera sono progetti separati, successivi alla stabilizzazione del nucleo.

Conserverei il modello senza hub obbligatorio: ciascun dispositivo continua a funzionare da solo. Parità significa stesso protocollo e stesse regole, non stessi task, archivio completo o funzionalità su ogni firmware. L'e-paper può essere un partecipante parziale, con sottoscrizioni esplicite e persistenza del sottoinsieme necessario.

## Riscontri nel repository

- `components/hid_link/hid_hub.c`: pairing P-256, confronto del codice, conferma HMAC, cifratura AES-GCM, callback che accoda e worker proprietario dello stato. Base utile, ma legata a ruoli hub/satellite, pacchetto HID e gestione autonoma della radio.
- In `hid_hub_start()` vengono inizializzati Wi-Fi ed ESP-NOW, imposto il canale 1 e acquisito un blocco contro il light sleep. Il worker usa uno stack da 5120 byte; il satellite aggiunge 3072 byte per heartbeat, oltre a code, mutex e stato statico. Non equivale a un servizio generico da 6 KB totali.
- `components/services/wifi/wifi.c`: driver inizializzato al bisogno e deinizializzato a radio spenta, acquire/release e gestione del power saving. ECOS deve integrarsi con questo ciclo di vita, evitando due proprietari del driver.
- `components/services/alarm/alarm.c` e relativo header: sveglie locali in NVS, massimo 16, timer e worker di sistema già presenti. Stop e snooze hanno API locali; lo snooze attuale è volatile. Non sostituire questo motore con il servizio di rete.
- `components/services/include/services/storage.h`: SD, fallback `/data` o nessun filesystem. La flash va scritta raramente; esistono lease per trasferimenti e uso USB della scheda.
- `components/hid_link/CMakeLists.txt`: dipendenze HID, Bluetooth e USB raggruppate. Il nucleo ECOS deve poter essere compilato senza dipendere da `hid_link`.
- `../esp32-s3-epaper-1.54/main/system/system.c`: controllo sveglie e risveglio al minuto; sincronizzazione Wi-Fi condizionata da `time_sync_due()`. Risveglio ogni minuto **non significa connessione Wi-Fi ogni minuto**. La finestra ECOS va agganciata alle connessioni effettive.
- `docs/SENSOR-INVENTORY.md`: alcuni microfoni, aptica e LED richiedono ancora verifica. Non pubblicizzare come pronta una capacità basandosi soltanto sulla presenza hardware.

## Punti da correggere o precisare nella visione

### Energia e raggiungibilità

Un nodo con radio spenta non riceve stop, trova-dispositivo o notifiche. ESP-NOW non elimina questo limite. La sveglia già sincronizzata può suonare offline; lo stop collettivo è immediato soltanto sui nodi raggiungibili. UI e documentazione devono distinguere inviato, confermato e in attesa.

Nessun nuovo risveglio periodico Wi-Fi sugli orologi come comportamento predefinito. Nessuna attesa prolungata di ACK prima del sonno. Un ritardo massimo di consegna non è garantibile se manca un incontro radio.

Il fallback sul canale 1 non è automatico: un nodo ancora collegato all'AP e uno passato a 1 possono non incontrarsi. Prima versione: canale AP quando associati; modalità offline esplicita e limitata nel tempo. Riconnessioni, scansioni e cambi canale devono essere governati dal servizio Wi-Fi.

### Stato replicato e gossip

Un registro LWW converge con un ordinamento totale deterministico; non conserva l'intento di due modifiche concorrenti. Usare identità stabile del nodo, contatore logico aggiornato anche alla ricezione e confronto `(contatore, node_id)`, senza dipendere dall'ora civile. Definire ripristino del contatore dopo reboot e nuova identità/incarnazione dopo reset.

Il solo massimo contatore visto per autore non basta: ricevendo l'evento 12 prima dell'11, si potrebbe credere di conoscere entrambi. Serve o un prefisso contiguo con gestione dei buchi e log limitato, oppure una riconciliazione delle versioni per chiave. Per pochi nodi e poche chiavi sceglierei inizialmente **inventario paginato delle versioni per chiave**, più semplice da verificare, con snapshot come recupero dopo lunga assenza.

Definire fin dall'inizio limiti di nodi, chiavi, dimensione valori, code e numero di messaggi per sessione. Niente archivio generico senza limite. Separare:

- configurazione durevole: sveglie e appartenenza;
- telemetria sostituibile: ultimo valore, con età e scadenza;
- comandi: ID univoco, scadenza, deduplicazione e conferme;
- contenuti grandi e storico: file/cache opzionali, esclusi dal gossip.

Le cancellazioni richiedono tombstone: eliminarle troppo presto fa ricomparire dati da un nodo rimasto offline. Prevedere una generazione di snapshot e risincronizzazione completa per nodi troppo vecchi. Il merge ricevuto non deve generare una nuova modifica locale e un ciclo di pubblicazioni.

### Sveglie

Separare la definizione della sveglia dall'occorrenza che sta suonando. Stop e snooze devono riferirsi a `alarm_id + occurrence_id`, con scadenza: uno stop vecchio non può spegnere la sveglia del giorno successivo. Gli ID numerici locali non sono sufficienti come identità globale; usare origine e ID o una mappatura dedicata.

Precisare fuso orario, cambio ora legale, orologio non valido, conflitto stop/snooze, duplicati, riavvio e perdita rete. Proposta: stop prevale sullo snooze della stessa occorrenza; aggiornamenti concorrenti dello snooze seguono una regola deterministica. Replicare verso il motore locale tramite un adattatore, senza bloccare il suono su rete o SD. Persistenza atomica del sottoinsieme essenziale anche senza scheda.

### Fiducia

HMAC autentica ma non cifra. Non è sufficiente per la promessa «niente in chiaro»: usare cifratura autenticata per dati riservati e comandi, riutilizzando le primitive già presenti senza inventare crittografia. Il nonce casuale GCM da solo non impedisce replay: la protezione attuale dipende anche dalle sessioni e sequenze HID, da riprogettare per ECOS e verificare dopo reboot.

Con una sola chiave di gruppo ogni membro può impersonare gli altri membri a livello di autenticazione di gruppo. Stabilire se questo modello di fiducia è accettabile; il campo autore da solo non prova l'origine. Appartenenza alla famiglia non deve autorizzare automaticamente input tastiera verso un PC: ponte HID e controllo richiedono consenso esplicito.

Espulsione e rotazione sono un protocollo, non un semplice cambio di chiave: generazione delle credenziali, consegna ai membri mantenuti tramite canali individuali, revoche persistenti e trattamento dei nodi offline. Non distribuire la nuova chiave soltanto con quella vecchia, conosciuta dal nodo espulso. Un'espulsione non è immediatamente efficace presso membri isolati che non l'hanno ancora ricevuta.

Non estenderei automaticamente il token dashboard locale a tutta la casa. Prima definire autorizzazioni, revoca e protezione delle richieste. mDNS è scoperta non attendibile: non pubblicare segreti e autenticare il peer prima di accettarne dati o comandi.

### Lavori a turno e cache

Una classifica deterministica non garantisce un solo esecutore: nodi con viste diverse della rete possono partire insieme. Accettare download duplicati, usare jitter, scadenza e risultati idempotenti; evitare di usare questa regola per azioni con effetti irreversibili.

Partirei da una sola cache di contenuti pubblici. Calendari, notifiche e altri dati personali richiedono destinatari e autorizzazioni. Firmware e app vanno distinti per board, versione e compatibilità: il peer è soltanto una cache, non sostituisce i controlli di integrità/autenticità e rollback. Il vantaggio energetico va misurato; discovery e sincronizzazione possono costare più di un piccolo download diretto.

## Architettura minima proposta

1. `ecos_proto`: C puro, serializzazione esplicita, validazione lunghezze/versioni, merge e deduplicazione; test host. Non inviare struct native con padding.
2. `ecos_link`: piccolo componente indipendente da HID e UI, adattatori trasporto e gestione credenziali. Nessuna inizializzazione Wi-Fi nascosta. Un solo punto di registrazione e smistamento ESP-NOW quando Remote ed ECOS convivono.
3. `services/ecos`: sessione temporanea su eventi radio, stato limitato, persistenza e adattatori a sveglie/sensori. Un proprietario dello stato; callback radio brevi, nessun filesystem o crittografia pesante nelle callback.
4. UI e dashboard: leggono snapshot e accodano comandi. Mai attendere rete sotto lock LVGL o nel worker di sistema con operazioni lunghe.

Prima proverei famiglia e sincronizzazione su Wi-Fi LAN già attivo, mantenendo Remote funzionante. Estrarrei inizialmente soltanto primitive pure e pairing riusabili; il trasporto ESP-NOW generico e la migrazione di Remote arriverebbero dopo la prova del ciclo radio. Questo modifica l'ordine proposto in ECOS.md per ridurre il rischio sul collegamento oggi operativo.

Per la LAN: discovery diradata, messaggi compatti, trasferimenti paginati e timeout stretti; riusare il server HTTP esistente se il suo ciclo di vita lo permette. Non avviare un secondo server sempre acceso soltanto per ECOS. ESP-NOW inizialmente per pochi comandi piccoli, senza frammentazione; calcolare il payload utile sottraendo header e tag dal limite scelto, invece di assumere 200 byte disponibili per ogni messaggio.

## Budget e comportamento in errore

«0 KB in più» va trattato come obiettivo da verificare: anche variabili statiche, handle e hook consumano RAM interna. Definire un delta misurabile e approvare ogni eventuale scostamento; non dichiarare rispettato il vincolo contando soltanto gli stack. Anche 6 KB di stack non sono 6 KB di servizio.

- Feature disabilitabile a compilazione; nessuna dipendenza HID/BLE/USB aggiunta al firmware base per effetto di ECOS. Confrontare dimensioni firmware e margine OTA per ciascuna board.
- PSRAM per archivio e buffer grandi dove consentito; stack, code e buffer radio vanno verificati rispetto ai vincoli reali. Niente fallback silenzioso di grandi allocazioni sulla RAM interna.
- Archivio limitato e allocazioni raggruppate; telemetria coalescibile e scartabile. Riservare capacità ai comandi essenziali. Code piene non devono bloccare callback o sveglie.
- Arresto della sessione: unregister callback, fermare timer e worker, liberare code/buffer/socket e rilasciare lease e power lock. Gestire anche fallimenti a metà avvio e disconnessioni durante il pairing.
- NVS per pochi dati critici; snapshot/journal limitati su SD con verifica integrità e recupero da scrittura interrotta. Nessuna scrittura per ogni beacon o campione sensore. Rispettare i lease dello storage.
- Se memoria, rete o storage non bastano, ECOS rinuncia alla sessione e mantiene operative le funzioni locali. Nessun riavvio richiesto per recuperare una sincronizzazione fallita.

## Percorso consigliato

| Passo | Risultato verificabile | Condizione per proseguire |
|---|---|---|
| 0. Baseline | RAM/flash/energia delle quattro board e finestra reale e-paper | Registrare minimi heap, largest block, stack e margine OTA nelle condizioni critiche |
| 1. Protocollo host | Codec, inventario per chiave, merge, replay e snapshot limitati | Test su perdita, riordino, duplicati, reboot, cancellazioni e input malformati |
| 2. Due nodi LAN | Pairing esplicito, batteria/temperatura condivise, stato raggiungibilità | Spegnimento radio invariato, memoria recuperata, nessuna regressione locale |
| 3. Sveglia di casa | Copia durevole e suono autonomo, poi stop/snooze per occorrenza | Prove offline, senza SD, reboot e riconciliazione; verifica separata dell'e-paper |
| 4. Link e Remote | Comandi ESP-NOW, arbitraggio radio e migrazione incrementale | Due dispositivi reali, HID e coesistenza Wi-Fi/BLE verificati |
| 5. Cache singola | Un contenuto pubblico condiviso con fallback diretto | Risparmio misurato, limiti e scadenze rispettati |
| 6. Estensioni | Notifiche, ghiera e focus; poi presenza e automazioni | Nucleo stabile e margini disponibili |

Interfono: prototipo separato con codec, banda effettiva, perdita pacchetti, jitter buffer e coesistenza audio/radio misurati; non basta specificare 16 kHz. Presenza BLE: indizio con scadenza e incertezza, non prova di assenza per funzioni di sicurezza. Sonno leggero: ipotesi da validare con dati, non capacità già offerta dall'IMU. Foto entro due secondi: obiettivo soltanto con destinatario già raggiungibile e pipeline camera verificata.

## Verifica prima di ogni integrazione

Confrontare baseline ed ECOS spento/attivo: boot, idle con radio spenta, Wi-Fi attivo, trasferimento TLS, audio, pairing, sincronizzazione massima e ritorno al riposo. Misurare heap interno minimo, blocco libero massimo, PSRAM, stack e durata radio; ripetere sessioni di apertura/chiusura per cercare leak e frammentazione. Sull'e-paper misurare energia e tempo aggiuntivi solo nella finestra effettiva, includendo discovery e timeout.

Testare rete assente, peer lento, coda piena, SD rimossa/occupata, allocazione fallita, reset durante persistenza, versione protocollo incompatibile e chiave ruotata. Le prove host non certificano driver, consumi o radio: la chiusura richiede build dei quattro firmware e dell'e-paper e prove fisiche delle funzioni coinvolte. Questa sessione ha prodotto soltanto analisi e documentazione.

## Punto di ripartenza per la prossima sessione

Iniziare dal passo 0 e dalla specifica del protocollo minimo, senza rifattorizzare subito Remote. Raccogliere le soglie RAM/flash già adottate nei firmware, identificare gli hook sicuri di accensione/spegnimento radio e scegliere limiti espliciti di archivio e sessione. Il primo incremento proposto è **due dispositivi che si abbinano e condividono un ultimo valore sensore durante una connessione già necessaria**, poi tornano al riposo con risorse recuperate.

Prima di promettere la sveglia collettiva, decidere la semantica dello stop sui nodi dormienti e la persistenza dello snooze. Non serve risolvere adesso posizione del Watcher, codec voce o automazioni per iniziare questo nucleo.
