# Solco 0.1.0-dev.1 — collaudo del 24 ottobre 2026

Prima base sperimentale standalone, installata e avviata via USB su hardware
reale. Non è ancora una release DJ professionale né una qualifica da palco.

## Dispositivi e installazione

| Target | Porta | Uscita | Binario |
| --- | --- | --- | --- |
| Waveshare AMOLED 1.8 | COM3 | Mono, media L+R | 1.346.928 byte |
| Waveshare Knob 1.8 | COM9 | Stereo | 1.332.208 byte |

Sono state verificate le identità `amoled_watch` e `knob_os` prima della
scrittura. Solco occupa lo slot secondario a `0x620000`; il firmware principale,
il bootloader, NVS e la tabella delle partizioni non sono stati riscritti.
La selezione di avvio OTA è stata modificata attraverso USB, senza trasferire
firmware tramite Wi-Fi. Sono conservati backup da 3 MiB dell'inizio degli slot
secondari originali, sufficienti a coprire la regione sostituita. I file storici
denominati `*-full.bin` sono letture parziali, non backup completi della flash.

SHA-256 dei binari installati:

- AMOLED: `fcdc951426f02ac5318784e7e821dc4c9829cb2c3252429e0f7d38b4d9e03833`
- Knob: `bc8ea9570892e95c07c08dc5d771a9d97eeb68015936f3e2dea2a9dd792ce2e5`

## Risultati effettivi

- Entrambi i progetti compilano e producono un'immagine valida per ESP-IDF 6.1.
- Test numerici host superati con MSVC `/W4 /WX`: parser WAV e ingressi
  malformati, crossfader, segnali correlati a fondo scala, mute, cambi ripetuti
  del crossfader e conversione mono in-place.
- PLAY e pausa aggiornano lo stato e il tempo. CUE riporta il deck a 00:00;
  FILE su un deck fermo cambia la sorgente. FILE durante PLAY viene respinto
  con il messaggio visibile «Pausa prima di FILE».
- La rotella Knob controlla il crossfader; il touch controlla il master.
- Acquisite e ispezionate schermate complete da entrambi i display durante
  la riproduzione. Le forme d'onda sono inviluppi dei dati audio nei buffer,
  normalizzati per leggibilità: non rappresentano l'intera traccia. Il meter
  digitale separato usa una scala logaritmica da -60 a 0 dBFS.
- Primo intervallo senza screenshot: massimo passaggio UI 13 ms su AMOLED,
  26 ms su Knob. Screenshot e interazioni hanno prodotto picchi maggiori;
  questi dati non costituiscono una misura degli FPS o della latenza touch.
  Ultimo intervallo di 12 secondi senza screenshot: 23 ms AMOLED, 19 ms Knob;
  nessun interrupt di completamento flush perso nei campioni di salute.
- Memoria interna libera dopo avvio e controlli: 182 KiB AMOLED, 206 KiB Knob;
  PSRAM libera fra 8.026 e 8.030 KiB nei campioni. La memoria interna libera
  resta invariata dopo i controlli. Non è un test di durata di ore.

## Problema trovato e correzione

La prima implementazione generava le demo con funzioni trigonometriche per
campione nel task produttore. Il collaudo reale con due deck ha rilevato
migliaia di blocchi con buffer insufficienti, pur con mixer e driver operativi.
Quella versione non ha superato il test di continuità.

Le demo sono state sostituite con loop PCM originali preparati offline e
incorporati nel binario. Le immagini corrette sono state ricompilate,
installate nuovamente e collaudate. Nei successivi log con due deck attivi,
entrambi i buffer rimangono pieni (16.384 frame ciascuno), con contatori
`empty=0`, `errors=0`, `clips=0`. Il budget è 5.805 microsecondi per blocco;
i tempi massimi del solo mix osservati sono inferiori a 500 microsecondi.

Ultimo campione della lettura continua, circa 261 secondi dall'avvio:

| Target | Blocchi | Mix massimo | Buffer A/B | Empty / errori / clip |
| --- | ---: | ---: | ---: | ---: |
| AMOLED | 44.804 | 444 µs | 16.384 / 16.384 | 0 / 0 / 0 |
| Knob | 44.800 | 433 µs | 16.384 / 16.384 | 0 / 0 / 0 |

Il deck B AMOLED è stato avviato dopo quello A; sul Knob il deck A è stato
fermato, riportato all'inizio e cambiato durante le prove dei controlli.
Non si tratta quindi di 261 secondi ininterrotti di entrambi i deck su ogni
target. Durante gli intervalli di lettura riportati entrambi erano attivi.

## Limiti della verifica

Le prove di continuità utilizzano le demo PCM incorporate. Non sono ancora
collaudati su hardware brani WAV reali da microSD, schede lente, errori I/O,
fine file, decodifica MP3, pitch, EQ, loop, cue persistenti o browser libreria.
MIDI, hub con altri dispositivi e controllo/trasferimento Orbit non sono ancora
implementati. Nessuna release OTA per gli altri modelli è stata pubblicata.

I contatori software non dimostrano l'assenza di dropout analogici. Non è
stata eseguita acquisizione strumentale dell'uscita, misura THD/SNR o latenza
end-to-end. Sul Knob manca il collegamento AUX per l'ascolto dell'utente.
Il driver AMOLED mantiene il buffering DMA preesistente: non viene dichiarata
una latenza adatta a scratch o performance live. L'ascolto sull'altoparlante
AMOLED resta da confermare dall'utente.

La console di sviluppo è attiva in questi pacchetti di test. CPU e DAC restano
attivi anche in pausa. Un riavvio ordinario torna al launcher; per riaprire
Solco già installata è possibile selezionare lo slot 1 via USB come da README.

Log, ricevute di installazione e screenshot della sessione sono conservati
in `progress-files/solco-*`; ogni pacchetto include identità e impronte dei
sorgenti in `build.json`.
