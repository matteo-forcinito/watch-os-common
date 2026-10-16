# Controllo remoto e recovery

La recovery mantiene LVGL su un display virtuale: le app interne restano utilizzabili senza inizializzare pannello e touch. Non e un firmware minimo di riparazione: rimuovere LVGL richiederebbe riscrivere ciascuna app come servizio senza UI. Si puo introdurre in seguito una recovery minima distinta.

API comuni a dashboard e companion, protette dall’autenticazione del webserver:
- GET /api/remote/frame: BMP RGB24 dall’intero schermo attivo, sovrapponendo dialoghi e toast; Cache-Control no-store. Cattura e distruzione dei buffer avvengono nel task LVGL.
- POST /api/remote/input: action tap con x/y; swipe con x/y/end_x/end_y; home; back. Coordinate limitate alla risoluzione fisica; il dito sintetico viene sempre rilasciato dopo una durata limitata.
- POST /api/apps/open: apre un’app interna gia disponibile nel firmware.
- GET /api/system: control descrive disponibilita dello schermo remoto, pannello fisico e accesso persistente recovery.
- POST /api/recovery/session: restituisce la chiave recovery persistente soltanto quando la recovery e attiva. Nessuna scadenza. Il normale token di abbinamento resta distinto.

Le catture sono su richiesta. Dashboard e companion aggiornano ogni 1,5 secondi solo quando la vista e visibile, con comando per disattivare il polling. Un frame 466x466 pesa circa 637 KiB non compresso; il polling nominale e circa 425 KiB/s, non un flusso video. I buffer risiedono prevalentemente in PSRAM. L’autonomia va misurata sull’hardware: Wi-Fi e ascolto Family continui aumentano i consumi, mentre il pannello fisico spento ne risparmia. Nessuna durata della batteria e garantita sulla base del solo codice.

La Family conserva il protocollo cifrato e anti-replay esistente. In recovery risponde anche a batteria; i membri condividono gia stato batteria, memoria, connettivita e uptime. I comandi attuali sono ricerca sonora e messaggi; il link Dashboard usa la rete locale per il controllo delle app.

Da sviluppare separatamente: delega esplicita del controllo via Family e richiesta cifrata del codice web, con autorizzazioni revocabili; catalogo dei sensori e concessioni di lettura; streaming di misure; trasporto del controllo senza LAN. Non si riutilizza il canale dei messaggi per trasmettere credenziali. Le app esterne avviano una partizione diversa: in recovery rimangono bloccate finche quel runtime non offre la stessa continuita di controllo.
