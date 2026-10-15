# ECOS — primo incremento: abbinamento diretto (ESP-NOW) o LAN

> Per lo stato corrente, Famiglia, compatibilità EL02, budget aggiornato e verifiche finali vedere [ECOS-CHIUSURA.md](ECOS-CHIUSURA.md). Le misure sotto riguardano il solo abbinamento.

Sviluppo del 14 ottobre 2026; trasporto ESP-NOW aggiunto il 15 ottobre 2026. Stato sperimentale: questo incremento stabilisce credenziali **a coppie**, non implementa ancora gossip, chiave di famiglia, revoca distribuita o autorizzazioni HID. Riferimenti: `ECOS.md` e `ECOS-VALUTAZIONE.md`.

## Uso

Su due dispositivi vicini (nessun router né internet necessario), aprire **Ecosistema** dal launcher o dalle Impostazioni, quindi **Avvia abbinamento**. Confrontare nome, identificativo e codice di sei cifre sui due dispositivi. Premere **I codici coincidono** su entrambi soltanto se il codice corrisponde. Un codice diverso richiede annullamento su entrambi e un nuovo tentativo.

La dashboard principale contiene la stessa procedura nella scheda **Ecosistema**; usa l'autenticazione locale già prevista dalla dashboard, senza nuovi token condivisi. Non è necessario lasciare aperta l'app Ecosistema per usare la dashboard. Uscire dall'app sull'orologio annulla la finestra attiva; chiudere il browser lascia scadere la finestra, al massimo tre minuti. Il pulsante Annulla la termina esplicitamente.

Per impostazione predefinita i due nodi si parlano direttamente via ESP-NOW (vedi Protocollo). In alternativa, nella dashboard si può indicare l'IPv4 privato dell'altro nodo per usare la rete di casa (10/8, 172.16/12 o 192.168/16), su almeno uno dei due. Non aggiunge un hub: la rete deve già poter instradare UDP tra i dispositivi. Il servizio fissa indirizzo e porta sorgente del candidato per tutta la finestra, consentendo anche una traduzione NAT stabile. L'IP non viene conservato dopo la sessione.

Il primo candidato viene mantenuto per tutta la finestra: se non è quello desiderato, annullare e riavviare. Avviare soltanto sui due dispositivi da associare. Gli abbinamenti memorizzati sono al massimo sei e non indicano presenza online. La pagina Famiglia permette ora di dimenticare localmente un dispositivo; la revoca distribuita rimane futura. Non cancellare manualmente l'intera NVS per liberare un posto.

## Ciclo di vita e memoria

- `CONFIG_WATCH_ECOS_PAIRING`: abilitato per impostazione predefinita; disabilitandolo si escludono servizio, app e controlli operativi API. La dashboard segnala che ECOS è disabilitato.
- A riposo nessun task, socket, timer o lease Wi-Fi ECOS. Rimane uno snapshot senza chiavi da 396 byte, posto in PSRAM con `EXT_RAM_BSS_ATTR` sulle configurazioni delle board, più tre flag e lo spinlock (11 byte di simboli interni, oltre all'allineamento). **Il costo statico interno non è letteralmente zero**. Verificare nella mappa che la configurazione mantenga attivo il posizionamento della BSS in PSRAM.
- L'avvio esplicito crea un worker con stack interno da 6144 byte; sessione e archivio temporaneo sono allocati soltanto in PSRAM, senza fallback sulla RAM interna. Aggiungere al budget task/TCB, socket, Wi-Fi e memoria temporanea PSA, non soltanto lo stack.
- ESP-NOW (predefinito): il worker accende solo la radio con `wifi_radio_acquire()` (stazione non collegata, stesso conteggio di riferimento di `wifi_acquire()`), disattiva il modem sleep, inizializza ESP-NOW e riduce la potenza TX a 8 dBm per la durata della finestra. Ripristina tutto in uscita. La ricerca non avvia un access point.
- LAN (IPv4 indicato): il worker acquisisce il Wi-Fi di casa tramite `wifi_acquire()`; non cambia canale e non tocca ESP-NOW.
- Finestra massima di 180 secondi; ricezione con timeout 200 ms, invio ogni 300 ms (ESP-NOW) o 750 ms (LAN). Annullamento non bloccante, osservato dal worker; durante la connessione può attendere la fine di `wifi_acquire()`. Il tempo effettivo può includere attese dei servizi condivisi e pulizia finale.
- In uscita: chiusura socket o deinit ESP-NOW, ripristino potenza TX, distruzione chiave effimera, rilascio del lease Wi-Fi, azzeramento delle chiavi temporanee e liberazione della PSRAM. Il servizio Wi-Fi applica poi il suo normale ritardo di spegnimento se non esistono altri utilizzatori.

Il piccolo archivio in NVS viene letto all'avvio, senza conservare le chiavi nello snapshot pubblico; l'abbinamento lo riscrive soltanto dopo la doppia conferma. Anche la rimozione locale aggiorna l'archivio. Il trasporto Famiglia usa inoltre epoche e checkpoint descritti in `ECOS-CHIUSURA.md`. Un errore di lettura non viene mascherato creando un archivio vuoto. SD e filesystem non sono necessari.

## Protocollo v1

Stessi frame su due trasporti.

**ESP-NOW (predefinito)**: broadcast su un canale comune. I nodi non collegati a una rete partono dal canale 1 (attesa 4 s), poi visitano i canali 1, 6, 11, 2…13 ogni 700 ms finché non fissano un commitment; un nodo collegato a una rete resta sul canale di quella rete e l'altro lo trova visitando i canali. Il MAC radio del mittente deve coincidere con quello scritto nell'header. Potenza TX ridotta a 8 dBm durante l'abbinamento: provato sul banco (AMOLED 1.8 e Watcher affiancati), a 20 dBm il ricevitore satura (RSSI −7/−9 dBm) e un verso perde quasi tutti i frame; a 8 dBm RSSI circa −20 dBm e scambio completo nei due versi. Bonus: l'abbinamento resta locale (stessa stanza).

**LAN (IPv4 indicato)**: UDP IPv4 sulla porta 42178. Discovery con broadcast COMMIT limitato alla LAN o verso un IPv4 privato indicato esplicitamente; dopo la selezione del commitment, invio unicast e controllo dell'indirizzo e porta sorgente. Reti guest e isolamento client possono impedire anche il collegamento diretto. Non esiste ancora discovery mDNS.

Header esplicito di 17 byte: `EC01`, tipo (1 byte), MAC mittente (6), MAC destinatario (6; zero nella ricerca). Nessuna struct C viene trasmessa. Dimensioni accettate esattamente:

| Tipo | Payload | Frame |
|---|---|---|
| HELLO (1) | nonce casuale 16, chiave pubblica P-256 SEC1 65, nome ASCII con terminatore/padding 24 | 122 byte |
| CONFIRM (2) | HMAC-SHA256 32 | 49 byte |
| SAVED (3) | HMAC-SHA256 32 | 49 byte |
| COMMIT (4) | SHA256 dell'HELLO e dell'identità, prima di rivelare chiave pubblica e nonce | 49 byte |

Ogni finestra genera chiave P-256 e nonce nuovi. Prima dello scambio HELLO ciascun nodo pubblica `SHA256("EC-COMM1" || MAC || HELLO)`. Rivela il proprio HELLO soltanto dopo avere fissato il commitment dell'altro nodo; un HELLO che non corrisponde viene rifiutato. Il commitment resta immutabile nella finestra. Questo passaggio serve a impedire scelte adattive dei valori per cercare un codice SAS coincidente: il ruolo dei commitment nei protocolli con codice breve è descritto in [RFC 6189, sezione 4.4.1.1](https://www.rfc-editor.org/rfc/rfc6189.html#section-4.4.1.1). ECOS non implementa ZRTP e questa prima versione richiede ancora revisione e prove del protocollo completo.

Il transcript è `EC-PAIR1`, le due identità e i due HELLO completi, ordinati lessicograficamente per MAC. L'hash SHA256 del transcript viene autenticato mediante HMAC con il segreto ECDH per derivare la chiave della sessione. Il codice SAS deriva da `HMAC(key, "SAS" || transcript_hash)`, primi quattro byte big endian modulo 1000000. I tag di conferma includono tipo, MAC del mittente e hash del transcript: non si possono riflettere nella direzione opposta o riutilizzare nella finestra successiva.

Il nome annunciato non è attendibile prima del confronto. Il codice di sei cifre richiede verifica umana ed è una protezione probabilistica; una sola selezione per finestra evita sostituzioni automatiche del candidato. Errori crittografici richiedono una nuova finestra. Nessun comando applicativo viene accettato da questa porta.

La credenziale persistita deriva separatamente da `HMAC(key, "LINK" || transcript_hash)`. Non vengono trasmesse chiavi segrete. Lo schema NVS v1 è un blob fisso con versione, numero di record e sei slot (MAC, nome 24, credenziale 32). Non abilita da solo alcun accesso HID o dashboard. Il futuro trasporto dovrà derivare chiavi dedicate da questa credenziale e definire cifratura autenticata, sessioni e protezione replay.

Il salvataggio richiede conferma locale e tag valido di conferma remota. SAVED viene inviato soltanto dopo il commit locale riuscito; l'esito «completato su entrambi» richiede SAVED remoto e viene ritrasmesso per cinque secondi. Non è un commit atomico distribuito: disconnessione, reset o memoria piena possono lasciare la coppia salvata su un solo nodo. La UI distingue salvataggio locale ed esito remoto non confermato; un nuovo abbinamento esplicito permette di riprovare.

## Codice e verifiche

- Servizio: `components/services/ecos/`; API pubblica: `services/ecos.h`.
- App: `components/apps/ecos/ecos_app.c`; UI indipendente dal worker, con snapshot limitato e polling soltanto mentre esiste la pagina.
- Dashboard: `web/index.html`, GET/POST `/api/ecos` protetti dallo stesso guard delle altre API. Conferma legata al candidato/codice visualizzati, non al candidato corrente letto al momento del clic.
- Codec e costruzione del transcript: `ecos_wire.h`, test C in emulatore RISC-V con il runner `tests/run_tests.py` (GCC ESP RISC-V, Unicorn 2.1.4, pyelftools).

Test host superati: tipi/lunghezze, versione, ordinamento simmetrico del transcript, alterazione di ogni byte di identità/nonce/chiave pubblica/nome, confronto dei byte. Questi test non verificano la crittografia PSA, la radio o NVS: richiedono prove fisiche e di errore separate.

### Verifiche da chiudere prima di dichiarare stabile

Le build delle quattro board sono superate; risultati e prove hardware ancora aperte per il codice finale sono in [ECOS-CHIUSURA.md](ECOS-CHIUSURA.md). Restano il confronto firmware/mappa con baseline comparabile e la verifica di layout tondo/ghiera e dashboard con browser già abbinato e non abbinato. Il client e-paper non è incluso in questo incremento.
