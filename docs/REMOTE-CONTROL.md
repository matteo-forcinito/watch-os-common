# Controllo remoto e recovery

La recovery mantiene LVGL su un display virtuale: le app interne restano utilizzabili senza inizializzare pannello e touch. Non e un firmware minimo di riparazione: rimuovere LVGL richiederebbe riscrivere ciascuna app come servizio senza UI. Si puo introdurre in seguito una recovery minima distinta.

API comuni a dashboard e companion, protette dall’autenticazione del webserver:
- GET /api/remote/frame: BMP RGB24 dall’intero schermo attivo, sovrapponendo dialoghi e toast; Cache-Control no-store. Cattura e distruzione dei buffer avvengono nel task LVGL.
- GET /api/remote/frame?format=orf2&seq=N&wait=ms: sessione mirror. Il pannello fisico si spegne e LVGL disegna solo in un framebuffer ombra in PSRAM (nessun flush QSPI). La risposta contiene solo il rettangolo cambiato dal frame `seq` (header ORF2 di 28 byte: magic, larghezza/altezza schermo, x/y/w/h dell'area, seq, flag bit0 run RLE bit1 schermo intero, lunghezza; poi RGB565 grezzo o coppie conteggio/colore). Senza `seq` o con un `seq` diverso dall'ultimo servito arriva lo schermo intero. Se nulla cambia il server attende fino a `wait` (massimo 250 ms: il webserver ha un solo task) e risponde con area vuota. Il Wi-Fi resta senza modem sleep per tutta la sessione.
- Fine sessione: dopo 4 s senza richieste frame/input, con POST /api/remote/session `{"active": false}`, oppure con un uso locale (tocco, rotella, tasto). Dopo un uso locale per 10 s una nuova sessione risponde 409.
- POST /api/remote/input: action tap con x/y; swipe con x/y/end_x/end_y; home; back. Coordinate limitate alla risoluzione fisica; il dito sintetico viene sempre rilasciato dopo una durata limitata.
- POST /api/apps/open: apre un’app interna gia disponibile nel firmware.
- GET /api/system: control descrive disponibilita dello schermo remoto, pannello fisico e accesso persistente recovery.
- POST /api/recovery/session: restituisce la chiave recovery persistente soltanto quando la recovery e attiva. Nessuna scadenza. Il normale token di abbinamento resta distinto.

Formati ORF1/BMP (cattura completa per richiesta, pannello acceso) restano per i client precedenti. Le catture ORF1 sono su richiesta. Dashboard e companion aggiornano ogni 1,5 secondi solo quando la vista e visibile, con comando per disattivare il polling. Un frame 466x466 pesa circa 637 KiB non compresso; il polling nominale e circa 425 KiB/s, non un flusso video. I buffer risiedono prevalentemente in PSRAM. L’autonomia va misurata sull’hardware: Wi-Fi e ascolto Family continui aumentano i consumi, mentre il pannello fisico spento ne risparmia. Nessuna durata della batteria e garantita sulla base del solo codice.

La Family conserva AES-GCM, credenziali distinte per coppia e protezione anti-replay. In recovery risponde anche a batteria. Con il webserver attivo mantiene l’ascolto Family anche in modalita normale, cosi la richiesta del codice puo arrivare mentre la pagina web e aperta. Dashboard e companion includono una pagina Family con permessi per membro, disattivati per impostazione iniziale:

- `permissions: 1`: consente al membro di richiedere il codice web corrente; il webserver deve essere gia attivo. In recovery restituisce lo stato senza alcun codice. Il normale abbinamento web rimane persistente.
- `permissions: 2`: consente letture su richiesta di batteria, tensione, RAM e campioni IMU gia disponibili. Non avvia il sensore. Campioni piu vecchi di un secondo non sono esposti come correnti; nessuno streaming automatico.
- I bit sono combinabili (`3`). La revoca blocca le richieste successive; gli accessi web gia abbinati restano validi. Non equivale alla revoca di una chiave web gia ottenuta.

API autenticate: GET `/api/ecos/family`; POST allo stesso endpoint con `{action: "permissions", id, permissions: 0..3}`, `{action: "web_code", id}` o `{action: "resources", id}`. Il permesso e concesso dal dispositivo che riceve la richiesta, non da quello che la invia.

I permessi in NVS sono legati all'impronta della credenziale della coppia: dimenticare e riabbinare non eredita le concessioni. Richieste e risposte usano un'estensione cifrata EL02, identificatore casuale a 64 bit, correlazione per membro/operazione e finestra di risposta di dieci secondi. Limitazione delle richieste: due secondi. Le risposte sono conservate in RAM per sessanta secondi; questo limite non cambia la durata illimitata degli accessi web. Nessun token web persistente transita nel protocollo Family. I firmware precedenti ignorano l'estensione e non rispondono.

Il link Dashboard usa la rete locale per il controllo delle app. Restano da sviluppare: inoltro dei comandi e controllo senza LAN, catalogo esteso dei sensori, attivazione autorizzata e streaming delle misure, revoca individuale dei client web gia abbinati. Le app esterne avviano una partizione diversa: in recovery rimangono bloccate finche quel runtime non offre la stessa continuita di controllo.
