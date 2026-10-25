# Solco 0.1.0-dev.4 — MP3, libreria e Orbit

24 ottobre 2026. Build sperimentale esterna per AMOLED 1.8 e Knob.
La conferma acustica dell'utente riguarda lo scratch AMOLED dev.3:
«Si sente e segue il gesto». I nuovi test automatici usano volume zero.

## Funzioni

- MP3 preparati sul dispositivo in WAV PCM16 stereo 44.100 Hz; conversione
  delle frequenze con filtro sinc polifase, decodifica minimp3 CC0.
- Importatore su core 0 a priorità inferiore allo storage, con cancellazione
  per generazione. Nessun decoder o accesso ai file nel task audio.
- Cache persistente `/music/.solco/p1-<SHA256>.wav`, pubblicazione tramite
  file temporaneo e rename, controllo spazio con riserva di 8 MiB.
  L'originale resta intatto. Circa 10,6 MB per minuto; nessuna pulizia automatica.
- FILE apre un browser scorrevole con selezione diretta, prime 32 tracce
  MP3/WAV più due demo; il volume resta accessibile. Preparazione con percentuale.
- Orbit: pagina Musica Solco per il solo dispositivo selezionato, caricamento
  in `/music`, filtro MP3/WAV, refresh dopo upload e verifica della risposta
  HTTP e del campo `ok`. Non mostra successo se il server rifiuta il file.

## Verifiche effettive

- Build ESP-IDF 6.1 completate per entrambi; installazione USB nello slot
  secondario, senza aggiornamento firmware Wi-Fi o riscrittura del sistema principale.
- Test host MSVC 64 bit /W4 /WX: MP3 originali da 8 secondi, 44.100,
  48.000 e 22.050 Hz. Output 352.800 frame in ciascun caso. Differenza RMS
  rispetto a FFmpeg: 0,139 / 0,688 / 0,682 unità di campione, rispettivamente.
  Input invalido, limite spazio e annullamento vengono rifiutati. Anche il
  convertitore host 32 bit ha completato il caso 48 kHz.
- Fixture originale MP3 stereo 48 kHz da 64.556 byte scritta sulle due SD;
  confronto byte per byte con l'asset incorporato superato su entrambe.
  Preparazione a freddo osservata al 25% e poi completata al 100% su entrambi.
- Test USB MP3 di riproduzione, gesto inverso, rilascio in pausa e CUE con
  riuso della cache: PASS su entrambe. Deck B demo in PLAY durante il
  caricamento dalla cache e le prove del deck A. Nessun blocco mancante,
  errore driver o clipping contato; volume zero limita la verifica del clipping.
  Questa prova non dimostra lettura simultanea di due lunghi file SD durante
  una conversione a freddo: resta un caso di stress da collaudare.
- Massimo del mixer osservato: AMOLED 1.516 µs, Knob 1.488 µs,
  rispetto al budget di circa 5.805 µs per blocco. Non è latenza analogica.
- Orbit: flutter analyze senza problemi; 12 test passati, inclusi upload
  mirato al dispositivo e risposta HTTP 200 con `ok=false`.
- APK debug compilato e identità verificata con aapt:
  `dev.watchos.orbit.companion.solco_test`, versione `0.5.2-solco-test`,
  etichetta `Orbit Solco Test`. Installabile accanto alla companion esistente.
  Non è stato ancora installato/collaudato su un telefono fisico.

Un primo MP3 di prova su SD aveva la dimensione corretta ma byte errati:
la scrittura diretta dall'asset flash è stata sostituita da copie in un buffer
RAM. Rigenerato esclusivamente il nostro file di collaudo e verificata
l'integrità prima di ripetere la decodifica. I tentativi falliti non sono PASS.
Un'asserzione preliminare richiedeva erroneamente 100% anche dopo CUE:
CUE azzera il progresso e riusa il file aperto. Il test finale distingue i casi.

## Utilizzo e limiti

Dal sistema principale: Orbit Solco Test → dispositivo → Musica Solco →
Carica file. Trasferimento via Wi-Fi. Poi App esterne → Solco → FILE.
La rete non è attiva nell'app esterna: non si carica dal telefono mentre suona.
Trasferimento solo BLE, controllo remoto dentro Solco, MIDI e hub restano futuri.

Il SoC S3 ha BLE; il Knob ha anche un ESP32 coprocessore con A2DP Classic.
Lo streaming A2DP raggiunge direttamente il DAC e non i deck scratchabili.
La precedente descrizione «entrambi senza Classic» era incompleta: consultare
`ESP32-S3-Knob-Touch-LCD-1.8/docs/COPROCESSOR.md`.

Non sono stati misurati latenza touch/analogica, THD, SNR o dropout analogici.
DMA preesistente invariato (AMOLED 6×480 frame, circa 65 ms), servo scratch
circa 25 ms, interpolazione lineare senza antialias dedicato alle alte velocità.
Nessuna promessa di risposta da giradischi professionale. Restano soak di ore,
brani di produzione lunghi, SD lente/errori I/O e più formati MP3 da verificare.
Pitch/key lock, EQ, loop, sampler e FX non sono implementati.

Nessuna release OTA generale né porting agli altri modelli in questa milestone.
AMOLED 1.75 e AMOLED 1.75C restano target distinti. Console di sviluppo attiva.
Log e prove in `progress-files/solco-*`; ricevute in `build.json` nei pacchetti.
Storico: TEST-REPORT-dev1.md, TEST-REPORT-dev2.md, TEST-REPORT-dev3.md.
