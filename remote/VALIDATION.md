# Remote 1.0.0 — verifiche e limiti

Data: 12 ottobre 2026. ESP-IDF 6.1, LVGL 9.6.

## Compilazione

I quattro firmware principali e le quattro varianti di Remote sono stati
compilati con i rispettivi componenti hardware. Il controllo delle partizioni
è eseguito da ESP-IDF; nessuna immagine supera lo slot OTA. Remote resta
esclusa dal firmware principale. I pacchetti sono distinti per modello.

Le build stabili disabilitano `CONFIG_WATCH_DEV_CONSOLE`; lo strumento di
confezionamento rifiuta un binario configurato con console di sviluppo attiva.
La CI ricompila da `sdkconfig.defaults` e allega il pacchetto Remote al rilascio.

## Watcher W1-A, COM6

- Avvio dell'app nello slot secondario; firmware principale e NVS preservati.
- Collegamento al PC: `Host connected`, `Encryption: on` nei log BLE.
- Screenshot reali: home, swipe, PC, Fn, tastiera locale, mouse, media,
  presentazioni, gamepad e categorie delle scorciatoie.
- Mouse: frecce di scroll sui lati del pad, stato di connessione dentro il pad,
  nessuna sovrapposizione con Sinistro/Trascina/Destro.
- Gamepad: croce con STOP centrale, tasti colorati a rombo, modalità delle
  direzioni e L/R/Select/Start entro il cerchio.
- Dopo apertura delle schermate: circa 91 KB di RAM interna libera, blocco
  massimo 44 KB, minimo dall'avvio 38 KB, circa 7.3 MB di PSRAM liberi;
  nessun interrupt di flush perso. La misura include la console di sviluppo
  e l'acquisizione di screenshot: non è una misura di autonomia.

## AMOLED 1.8, COM3

- Partizioni lette prima della prova: sistema in ota_0, precedente Remote in
  ota_1. Il binario di prova è stato scritto solo in ota_1, senza erase della NVS.
- Avvio Remote 1.0.0 e registro delle 16 app verificati dalla console.
- Collegamento HID BLE al PC osservato nel log.
- Screenshot completi di tastiera PC, gamepad e categorie delle scorciatoie.
  L'acquisizione seriale iniziale perdeva righe: lettura a blocchi e controllo
  di completezza hanno permesso di ripeterla senza usare immagini corrotte.
- Console iniziale: circa 104 KB di RAM interna libera, blocco massimo 56 KB,
  nessun interrupt di flush perso.
- Le operazioni di controllo seriale possono riavviare il dispositivo; Remote
  imposta il sistema come destinazione del riavvio. Non scambiare un ritorno
  al sistema provocato dalla procedura di prova per un crash dell'app.

## Revisione dell'affidabilità

- Pressione/rilascio di tastiera e media nella stessa transazione; batch testo
  e correzioni accettati interamente o rifiutati prima dell'invio.
- Coda non bloccante nella UI, spazio riservato ai report neutri, generazioni
  della connessione per scartare input vecchio, tentativi di rilascio anche a
  coda vuota in caso di errore del trasporto.
- Stato gamepad e trascinamento azzerati uscendo o perdendo la connessione.
- Preferenze NVS precedenti lette mantenendo i default dei campi aggiunti.
- Sveglie del sistema inizializzate in Remote; UI di suoneria ritentata in caso
  di errore di allocazione/apertura. Non è stato eseguito un test sonoro completo.
- Launcher anche su memoria interna, upload su file temporaneo, conservazione
  e recupero del file precedente in caso di pubblicazione interrotta.

## Verifiche ancora necessarie

- Effettivo arrivo e significato di ogni comando nelle applicazioni del PC.
  I log BLE confermano la connessione, non il risultato nell'applicazione.
  L'automazione del browser/desktop non era disponibile nella sessione;
  non sono state inviate scorciatoie alla cieca ad applicazioni dell'utente.
- USB HID: enumerazione e interazione completa con tastiera/mouse/media/gamepad.
- Hardware AMOLED 1.75 e Knob: touch/layout, ghiera, ritorno al sistema,
  installazione da `/data` o SD e aggiornamento con spazio limitato.
- Sveglia durante una sessione Remote, batteria, riconnessione dopo sospensione
  del PC e compatibilità del gamepad con i giochi scelti.
- Interruzione reale di alimentazione durante upload/installazione.

Il gamepad è HID generico, non XInput. I comandi Teams/Zoom/PowerPoint/browser
dipendono da sistema, applicazione, impostazioni e finestra con focus. Remote
non riceve stato microfono, camera, playback o slide dal computer.
