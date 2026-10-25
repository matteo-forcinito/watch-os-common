# Solco 0.1.0-dev.5 — buffering e concorrenza microSD

24 ottobre 2026. Collaudo su AMOLED 1.8 (COM3) e Knob (COM9).
Versione sperimentale esterna. Nessuna pubblicazione OTA generale.

## Modifiche

- Profilo DMA riservato alla build Solco: 6 descrittori da 128 frame,
  invece di 6 da 480. A 44.100 Hz la capacità passa da 65,31 a 17,41 ms,
  riduzione del 73,3%. Non è una misura della latenza touch/analogica.
- I driver hardware accettano una definizione di compilazione specifica;
  il valore predefinito del sistema principale resta 480 frame. Il percorso
  microfono AMOLED resta a 256 frame. Nessuna modifica al guadagno audio.
- Diagnostica `@TIMING`: massimo intervallo fra completamenti di audio_write,
  inclusi rendering, attesa I2S, logging e scheduling fra le scritture.
  `late` conta intervalli superiori a 17.415 µs dopo i primi 100 blocchi.
  È un indicatore di margine temporale, non un rilevatore di dropout analogici.
- Comando di sviluppo per il test a freddo: chiude i due file in pausa,
  elimina solo la cache SHA-256 del nostro MP3 originale di collaudo,
  carica MP3 sul deck A e WAV reale su B. Non cancella musica dell'utente.
- Corretto il verificatore della fixture: un file troncato non può più
  risultare identico al riferimento soltanto perché il prefisso coincide.

## Risultati hardware

Prima fase: MP3 originale stereo 48 kHz da 8 secondi preparato da zero
mentre B riproduce il WAV stereo da 12 secondi sulla stessa microSD.
La preparazione è stata osservata attiva contemporaneamente alla lettura
SD di B; completata con A pronto su entrambe le schede.

Seconda fase: sei cicli di CUE, caricamento e riproduzione simultanea
MP3 preparato + WAV. Posizioni di entrambi verificate in avanzamento
per oltre un secondo ciascuno, attraversando la capacità delle cache.
Al termine entrambi i deck in pausa, CUE a zero e nessun dito agganciato.

| Target | Mixer massimo | Intervallo scritture massimo | late / empty / errors / clips |
| --- | ---: | ---: | --- |
| AMOLED 1.8 | 1.546 µs | 8.305 µs | 0 / 0 / 0 / 0 |
| Knob | 1.503 µs | 9.559 µs | 0 / 0 / 0 / 0 |

La prova conclusiva è stata eseguita rispettivamente con volume utente
100% e 3%, dopo autorizzazione all'ascolto durante i test. Gli script non
hanno cambiato il volume. Il materiale originale di test non arriva a
fondo scala; l'assenza di clipping non garantisce ogni possibile brano/mix.
Attesa osservata dal PLAY di B alla disponibilità di A: circa 9,08 / 6,45 s;
comprende il polling USB e non è un benchmark preciso del decoder.

Log conclusivi: `progress-files/solco-{amoled,knob}-dev5-stress-accepted.*`.
I tentativi precedenti interrotti da variazioni manuali del volume o da
risposte USB mancanti non vengono classificati PASS. Lo script finale
permette l'ascolto soltanto con `--allow-user-volume`, mentre il default
mantiene la protezione per i test muti. Dopo un ACK USB perso, non ripete
alla cieca un comando PLAY che potrebbe essere già stato eseguito.

Anche il test MP3 PLAY, scratch inverso, rilascio in pausa e CUE con riuso della cache è PASS su entrambi con il nuovo profilo DMA. Nessun incremento di empty/errors/clips/late; massimi temporali invariati. Log: progress-files/solco-{amoled,knob}-dev5-scratch.*.

## Build e distribuzione

Build ESP-IDF 6.1 di entrambi completate. Immagini con identità specifica
`solco_amoled` / `solco_knob`; installazione USB nel solo slot secondario,
con verifica hash esptool e copia SD `/apps/Solco` aggiornata all'avvio.
Firmware principale e tabella delle partizioni non riscritti.
La ricevuta build.json comprende anche driver audio, wrapper CMake e
profilo DMA, oltre alle impronte del motore e del decoder CC0.

UI e companion restano quelle della dev.4: APK Orbit Solco Test 0.5.2
compatibile. I suoi 12 test e l'analisi statica sono documentati nel
TEST-REPORT-dev4.md; nessun nuovo APK compilato in questa milestone.

## Limiti e fonte tecnica

La riduzione della coda DMA non dimostra una latenza finale di 17 ms:
restano blocchi software, touch, servo di posizione circa 25 ms e codec/DAC.
Nessuna acquisizione analogica o confronto strumentale prima/dopo disponibile.
Il profilo richiede ancora prove di ore, SD lente e brani lunghi reali.

Scratch resta con interpolazione lineare e limite ±4×; key lock, pitch,
EQ, loop, FX, MIDI, hub e importazione durante il mix restano futuri.
Trasferimento brani da Orbit nel sistema principale via Wi-Fi, non solo BLE.
AMOLED 1.75 e 1.75C restano modelli distinti, non collaudati qui.

I parametri DMA e la relazione frame/frequenza seguono la documentazione
[Espressif I2S](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html).
Il confronto 65,31 → 17,41 ms è un calcolo sulla capacità configurata.
Storico completo nei TEST-REPORT-dev1.md fino a TEST-REPORT-dev4.md.
