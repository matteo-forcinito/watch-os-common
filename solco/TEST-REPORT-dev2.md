# Solco 0.1.0-dev.2 — collaudo del 24 ottobre 2026

## Modifiche

Volume digitale unico, avvio a 0% e deck in pausa. Il codec è ora a guadagno
unitario, eliminando i -22,5 dB nascosti della build precedente. L’utente può
regolare lo slider o usare -/+ (un punto), MUTE e ripristino. La rotella Knob
ora regola il volume. Il livello non viene conservato fra i riavvii.

Interfaccia più larga, trasporti ingranditi e pulsanti A > / B > per il singolo
deck. Il disco ruota secondo la posizione audio e si ferma in pausa; questa
versione non implementa ancora scratch, jog o seek trascinando il disco.

All’avvio viene pubblicata una copia del binario corrente in /apps/Solco sulla
microSD, se disponibile, e registrato il nome dello slot per i launcher
compatibili. Il pacchetto rimane sulla scheda anche quando lo slot secondario
viene occupato da un’altra app. Il launcher può quindi reinstallarlo.

## Verifiche eseguite

- Compilazione ESP-IDF 6.1 completata per AMOLED 1.8 e Knob, senza warning nei
  sorgenti Solco nella build finale.
- Flash USB, verifica hash e avvio reale completati su COM3 e COM9.
- Stato iniziale Knob osservato: volume=0, playing=0/0, peak=0, launcher=1.
- Su AMOLED il primo campione è stato acquisito dopo l’interazione dell’utente:
  volume=54 e deck in pausa. Non è stato alterato dal test automatico.
- Pubblicazione della copia SD confermata dal task su entrambi (launcher=1).
  Riapertura dalla UI del firmware principale ancora da confermare dall’utente.
- Sul Knob, test touch +: 0 -> 1; rotella +2: 1 -> 3; MUTE: 3 -> 0;
  riattivazione: 0 -> 3; secondo MUTE: 3 -> 0. Verificati dai valori del motore.
- Vista singolo deck aperta tramite A > e screenshot completo ispezionato.
  Ispezionati anche screenshot completi del mixer sui due display.
- Durante l’uso dell’utente su AMOLED, due deck attivi, volume=57, cross=861:
  empty=0, errors=0, clips=0, picco digitale non nullo. Questi valori non
  dimostrano che l’uscita analogica sia udibile o priva di disturbi.
- Memoria interna libera: 182 KiB AMOLED / 206 KiB Knob. PSRAM circa 8.024 /
  8.023 KiB. Nessun flush IRQ perso nei campioni di salute. Non è una prova
  prolungata di stabilità né una misura di latenza audio.

Prima del flash COM3 conteneva Motore 1.0.0 nello slot secondario, al posto di
Solco. È stato salvato il prefisso di 1,5 MiB che copre tutti i settori
sovrascritti dall’immagine Solco: progress-files/solco-com3-before-dev2.bin,
SHA-256 c1da3dd0fd310c0c52a483e4d216fc1f312afe40e8e7437db6747dea605c4b2c.
Il resto dello slot non è stato scritto. Firmware principale, bootloader, NVS
preesistente e tabella delle partizioni non sono stati riflashati. L’app scrive
solo la propria nota NVS e i file sotto /apps/Solco; la selezione di boot viene
aggiornata via USB e dall’SDK. Nessun firmware trasferito via Wi-Fi.

## Limiti

Ascolto dall’altoparlante ancora in attesa di risposta dell’utente. Non è stata
eseguita acquisizione analogica o misurazione di latenza, THD o rumore.
Le prove usano demo PCM incorporate; WAV reali da SD, fine file e schede lente
restano da collaudare. MP3, scratch, pitch, EQ, loop, MIDI, hub e Orbit non sono
implementati. Nessuna release OTA generale è stata pubblicata.

Il verbale dev.1 resta in TEST-REPORT-dev1.md come storico: i relativi tempi di
mix e prove numeriche non sono una nuova misura di durata della dev.2.
I binari di questo pacchetto includono la console di sviluppo.
