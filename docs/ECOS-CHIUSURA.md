# ECOS: consegna dello sviluppo

Aggiornato il 15 ottobre 2026. Questo documento descrive il codice corrente e integra `ECOS-PAIRING.md`; `ECOS.md` rimane la visione generale, non una lista di funzionalità già disponibili.

## Perimetro realizzato

- Abbinamento diretto ESP-NOW con confronto del codice e doppia conferma; alternativa LAN esplicita. Massimo sei dispositivi, credenziali persistenti a coppie.
- Famiglia: elenco dispositivi, informazioni/stato, ricerca e suoneria con arresto, messaggi rapidi e rimozione locale dell'abbinamento.
- Le operazioni sono disponibili nell'app Ecosistema e nella dashboard principale, con le protezioni di accesso già usate dalle altre API. API famiglia: GET/POST `/api/ecos/family`.

Non sono implementati gossip, chiave condivisa di famiglia, revoca distribuita, client e-paper o autorizzazioni HID. Dimenticare un dispositivo elimina il rapporto sul nodo locale: per rimuoverlo da entrambi occorre agire su entrambi.

## Correzioni di chiusura

- Trasporto Famiglia **EL02**, separato dall'abbinamento EC01. Nonce AES-GCM di 12 byte: epoca persistente a 64 bit e sequenza a 32 bit; la sequenza sopravvive ai riavvii del worker radio. Esaurimento o errore di persistenza impediscono l'invio.
- Protezione replay mantenuta nei riavvii del worker; checkpoint NVS dei comandi ricevuti prima di eseguirli, legato alla credenziale. Errori di lettura/scrittura non vengono ignorati. Pulizia anche delle chiavi inizializzate solo parzialmente.
- La ricerca considera trovato un nodo soltanto dopo una risposta autenticata. L'ACK radio indica invio, non esecuzione remota: la UI riporta «richiesta inviata».
- Rimozione serializzata rispetto all'abbinamento e identificativi validati prima dell'accodamento.
- La suoneria Trova viene fermata prima del passaggio a un'altra app e lascia priorità alla sveglia. Non parte se non è disponibile il timer che ne limita la durata.
- Nessun avvio periodico del worker Famiglia in assenza di dispositivi abbinati.

**Compatibilità:** aggiornare entrambi i dispositivi alla versione EL02. I vecchi frame EL01 di Famiglia vengono rifiutati; le credenziali di abbinamento EC01 restano valide. Non cancellare NVS.

## RAM, radio e flash

Il budget del solo abbinamento non descrive più l'intera funzione: Famiglia aggiunge un timer di controllo ogni 15 secondi e stato persistente in PSRAM. Il worker radio è attivo mentre serve all'interfaccia, per il lease della dashboard (90 secondi), oppure nella politica di ascolto su alimentazione esterna con schermo spento/AOD. Questo ascolto mantiene la radio e ha un costo energetico e di RAM reale.

Sessioni, code e stato voluminoso sono in PSRAM; il worker usa uno stack da 6144 byte, oltre a TCB, Wi-Fi, ESP-NOW e crittografia. In uscita vengono liberati code e chiavi e rilasciati i riferimenti radio. Non è stata misurata sul dispositivo la RAM minima della versione finale: il margine flash non dimostra il margine heap.

NVS: una prenotazione dell'epoca al primo uso del link con abbinamenti in ciascun boot, più un checkpoint per comando applicativo ricevuto. Nessuna scrittura periodica per la telemetria. Un reset tra checkpoint ed esecuzione può perdere il comando; non viene promessa esecuzione esattamente una volta.

## Verifiche

Build ESP-IDF 6.1 riuscite con le modifiche su AMOLED 1.8, SenseCAP Watcher, AMOLED 1.75 e Knob 1.8. Tutte le immagini rientrano nella partizione applicativa, con almeno il 51% libero nelle configurazioni di prova. Non sono misure dell'incremento rispetto a una baseline.

Superati i test C dei codec in emulatore RISC-V: limiti e tipi dei frame, transcript di abbinamento, codifica stato, epoche, duplicati, riordinamento e limiti della sequenza. Superato `node --check` sul JavaScript della dashboard e `git diff --check`. Questi test non simulano PSA, driver radio, NVS né la concorrenza completa del firmware.

La revisione finale non ha installato firmware sui dispositivi. Le precedenti prove dell'abbinamento non certificano il nuovo trasporto Famiglia EL02. Prima di dichiarare stabile questa versione, aggiornare AMOLED 1.8 e Watcher insieme e verificare:

1. Abbinamento, doppia conferma, annullamento e persistenza dopo reboot.
2. Stato, Trova/arresto e messaggi nei due versi, da orologio e dashboard; nodi su canali diversi e nodo assente.
3. Sveglia durante Trova, timeout, uscita dalla pagina e passaggi rapidi tra app.
4. Cicli ripetuti di apertura/chiusura, abbinamento e rimozione, rilascio radio e recupero heap; alimentazione USB e batteria.
5. Reset e guasti NVS durante i comandi, assenza di esecuzione di duplicati e rifiuto dei frame della versione precedente.

Il codice e la documentazione chiudono questo incremento software; la qualificazione hardware della versione finale rimane esplicitamente aperta. Su richiesta dell'utente viene preparato il rilascio 1.7.0 per tutte e quattro le schede, con note esplicite sulla compatibilità EL02 e sulle verifiche ancora aperte. La pubblicazione non implica l'installazione sui dispositivi collegati.
