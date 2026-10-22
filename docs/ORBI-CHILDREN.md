# Children: i pet dei dispositivi

Disponibili dal firmware 1.9.5 e da Orbit Companion 0.5.1.

## Sul dispositivo

Al primo avvio nasce un child con un codice di 16 cifre esadecimali: famiglia e
aspetto dipendono da identità e capacità del dispositivo. Nome e aspetto si
ricostruiscono dallo stesso codice su ogni schermo. Il codice viene salvato una
volta in NVS: riavvio e aggiornamento OTA non fanno nascere un altro pet.

Aprire **Menu → scheda con l'immagine del child** (sugli schermi rotondi:
**Il tuo child** nel menu circolare). Anche la homepage Lista app ha la scheda.
Nella pagina il pet è al centro, su una scheda scura con accenti ciano come
Copertura. Toccarlo per una coccola; sono disponibili cibo, gioco e nanna.
"Altri child sul device" passa agli ospiti ricevuti da altri dispositivi.

Sazietà, energia, gioia, esperienza, età e riposo sono conservati sul device.
I bisogni seguono il tempo reale quando l'orologio è sincronizzato; nessun pet
muore. Non serve lasciare Orbit aperta per prendersene cura.

"Piccola compagnia" mostra il pet nell'angolo delle altre schermate; si può
disattivare. L'anteprima nel menu è statica. Le animazioni lavorano a 15 fps,
si fermano durante il tocco e non aggiornano viste nascoste o fuori schermo.
La compagnia scompare nella pagina dedicata, con schermo spento/AOD, durante
una scena Orbit e nelle schermate di sveglia/chiamata.

## Viaggi con Orbit

1. Aggiornare i device coinvolti alla 1.9.5 o successiva e Orbit alla 0.5.1.
2. Collegare i device alla companion; aprire **Ecosistema → Children**.
3. Scegliere **Viaggia** sotto il pet e il device di destinazione.

L'aspetto, il nome generato, l'origine e i progressi seguono il child. Ogni
device può ospitare fino a otto record, compresi i viaggi da completare.
Il pet originario non viene rigenerato quando lascia la sua casa.

Il viaggio è salvato per passaggi. Se si interrompono la rete o Orbit,
ricollegare gli stessi device e aprire Children: riprende automaticamente,
oppure premere "Riprendi viaggio". Prima della partenza si può annullare;
dopo la partenza va completato. Non ripristinare backup NVS vecchi né cancellare
i dati di Orbit durante un viaggio. I backup completi possono, per loro natura,
ripristinare anche vecchie copie dei pet.

## Protocollo e persistenza

Rotte autenticate tramite `web_route`, disponibili anche sul bridge BLE:

- `GET /api/orbi`: archivio e selezione; feature `pets` in `GET /api/link`.
- `POST /api/orbi`: `action`, `seed` e, per i viaggi, `transfer` (16–40 caratteri
  esadecimali/trattini). Azioni locali: `pet`, `feed`, `play`, `sleep`, `select`,
  `company` (con `on`). Le interazioni richiedono un record attivo.
- `reserve`: blocca e salva il pet sulla sorgente, restituisce lo snapshot.
- `receive`: salva lo snapshot `pet` sulla destinazione come inattivo.
- `depart`: salva la partenza sulla sorgente, conservando una ricevuta.
- `arrive`: attiva il pet sulla destinazione e lo seleziona.
- `forget`: conferma il completamento alla sorgente e libera la ricevuta.
- `cancel`: prima di `depart`, elimina la ricezione inattiva e sblocca la
  sorgente. Orbit annulla prima la destinazione, poi la sorgente.

I passaggi sono idempotenti per seed e identificativo del viaggio. Nessuna
destinazione diventa attiva prima della partenza durabile dalla sorgente.
La ricevuta non viene riutilizzata prima della conferma finale. Orbit salva il
registro `orbi-child-transfer-v1` prima dei passaggi successivi e ne controlla
la rilettura. Se il salvataggio locale fallisce non avanza nel protocollo.

Il firmware salva un blob versionato nella namespace NVS `orbi_children`,
senza task aggiuntivi né scritture periodiche in flash. Il salvataggio fallito
di un comando ripristina lo stato in RAM precedente e restituisce errore.

## Verifiche

`python tools/test_pet_store.py` esegue il vero modello C su Windows con NVS e
hardware simulati. Verifica nascita distinta, persistenza dopo riavvio,
interazioni, fallimento NVS, import non valido, viaggio e annullamento.

`flutter test test/child_transfer_test.dart test/orbi_genome_test.dart` verifica
genomi e coordinamento, inclusi timeout dopo ogni passaggio già ricevuto,
riavvio della companion, destinazione offline e registro non scrivibile.
