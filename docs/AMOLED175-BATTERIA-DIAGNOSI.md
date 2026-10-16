# AMOLED 1.75: indicazione fissa a 0%

Diagnosi del 15 ottobre 2026 sul dispositivo collegato a COM10, firmware `amoled175_watch` 1.7.0. Sintomo riferito: percentuale sempre a zero.

## Risultati osservati

Il log di avvio riporta `AXP2101: battery 0% 2909 mV, USB`, senza indicazione di carica. Il driver PMU della scheda non è cambiato fra i rilasci 1.6.1 e 1.7.0.

Per separare il sistema operativo dal PMU è stata compilata una piccola applicazione ESP-IDF 6.1 eseguita interamente in RAM con `esptool --no-stub load-ram`. Nessuna scrittura flash, NVS o registri di configurazione PMU. Sono stati letti quattro campioni a distanza di cinque secondi, poi ripristinato l'avvio normale tramite reset USB.

Tutte le letture I²C sono riuscite, con valori stabili:

| Registro | Valore | Interpretazione |
|---|---|---|
| 0x03 | 0x4a | AXP2101 risponde |
| 0x00 | 0x28 | VBUS valido e batteria rilevata |
| 0x01 | 0x15 | Sistema acceso, corrente in standby, stato caricatore 5: non in carica |
| 0x18 | 0x0a | Fuel gauge e ricarica della cella abilitati |
| 0x30 | 0x05 | ADC batteria e VBUS abilitati |
| 0x34/35 | 0x0b5c–0x0b5e | Tensione batteria letta dal PMU: 2908–2910 mV |
| 0x38/39 | 0x13b4 | Tensione USB letta dal PMU: 5044 mV |
| 0x61 | 0x05 | Limite di precarica configurato: 125 mA |
| 0x62 | 0x08 | Limite di carica configurato: 200 mA |
| 0x64 | 0x03 | Tensione di fine carica configurata: 4,2 V |
| 0x50 | 0x00 | Ingresso TS configurato per influire sul caricatore, sorgente di corrente disattivata |
| 0x54/55 | 0x00/0x00 | Soglie TS di carica da verificare rispetto alla variante hardware |
| 0x67 | 0xe6 | Timer di sicurezza attivi; non modificati |
| 0xa4 | 0x00 | Il PMU stesso restituisce 0%, indipendentemente dalla UI |

Le tensioni sono letture del PMU, non misure indipendenti con multimetro. I limiti configurati non sono misure della corrente effettiva.

## Conclusione e limiti

Non emerge un semplice errore grafico o una conversione I²C fallita in questo esemplare durante la prova: lo zero proviene dal PMU e coincide con una tensione bassa. Il caricatore è abilitato ma non sta caricando. Questo non basta a distinguere una configurazione PMU incompleta/inadeguata da un problema del percorso di carica o della cella.

L'inizializzazione attuale abilita rilevamento batteria, gauge e ADC, ma non configura esplicitamente le condizioni TS o la carica: dipende dallo stato già presente nel PMU. Prima di modificare questi parametri occorre confrontare la variante fisica e il BSP ufficiale, leggere gli eventuali motivi di blocco e verificare il percorso di alimentazione. Non disabilitare protezioni termiche o timer per far salire la percentuale.

Nel driver esiste inoltre una debolezza distinta: `read_reg()` ignora l'errore I²C e restituisce zero; in particolare una lettura fallita del registro percentuale può apparire come 0%. Non è la causa dimostrata qui, perché la diagnostica ha ottenuto letture valide e ripetibili. Non è stata applicata una correzione speculativa né pubblicato altro firmware.

Le prove successive utili sono: rilevare i motivi di blocco prima che l'avvio normale cancelli gli IRQ, confrontare inizializzazione PMU e schema della revisione reale, osservare la carica con alimentazione USB nota e verificare tensione/cella/percorso di carica con misure indipendenti se il blocco persiste.

Riferimenti: [datasheet AXP2101 fornito da Waveshare](https://files.waveshare.com/wiki/common/X-power-AXP2101_SWcharge_V1.0.pdf), sezioni 6.7 e registri 0x00, 0x01, 0x18, 0x50–0x55; [schema della scheda](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75/ESP32-S3-Touch-AMOLED-1.75.pdf).

Artefatti locali ignorati da Git: `build/ecos-checks/amoled175-battery-boot.log`, `amoled175-pmu-raw.log`, sorgenti in `build/battery-diag/` e runner `build/ecos-checks/battery_ram_read.py`.

## Correzione candidata 1.7.1

Il confronto con l'[esempio ufficiale Waveshare](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/examples/esp-idf/01_AXP2101/main/port_axp2101.cpp), righe 104–106, mostra una configurazione mancante nel nostro driver: `disableTSPinMeasure()`. Il produttore la richiede per la scheda senza rilevamento della temperatura batteria, altrimenti la ricarica può essere anomala.

Preparata una modifica nel solo repository AMOLED 1.75:

- Registro 0x50: `(valore & 0xf0) | 0x10`, come l'implementazione XPowers usata dal riferimento Waveshare; il pin TS diventa ingresso fisso e non condiziona la carica.
- Registro 0x30: bit 1 disabilitato, ADC batteria e USB abilitati; gli altri bit restano invariati.
- Limiti di corrente, tensione finale, protezione termica del chip e timer di sicurezza conservati.
- Operazioni read-modify-write controllate: nessuna scrittura dopo una lettura fallita; errori di inizializzazione restituiti al chiamante.
- Percentuale non leggibile o fuori intervallo restituita come sconosciuta (-1), distinta dallo 0% valido.

Superati i test sul vero driver in emulatore RISC-V con I²C simulato: configurazione TS, conservazione dei parametri di carica, percentuali valide/non valide, errori di lettura/scrittura e rilascio del dispositivo in caso di inizializzazione fallita. Test riproducibili nel repository della scheda, `tests/pmu/`.

Generata anche l'immagine locale 1.7.1 (`amoled175_watch.bin`, 0x2dfc30 byte): controllo partizione superato, 52% libero. Nessun task o buffer dinamico aggiunto dalla correzione.

La prova fisica di questa configurazione è stata bloccata dalla revisione automatica perché la scrittura TS può influire sulla protezione termica della ricarica e manca un'approvazione esplicita per questo preciso cambio. Nessuna scrittura TS è stata eseguita e il firmware correttivo non è stato pubblicato. Occorre autorizzare la prova prima di verificarne l'effetto sul dispositivo e completare il rilascio 1.7.1.

## Prova fisica autorizzata: ricarica ripartita

Dopo l'autorizzazione esplicita dell'utente è stata eseguita la prova in RAM. Prima della modifica: registro 0x50 = 0x00, stato 0x01 = 0x15 (non in carica), tensione 0x0b5f = 2911 mV. Dopo l'applicazione della sola configurazione TS prevista dal riferimento Waveshare:

- 0x50 = 0x10, stato 0x01 = 0x31: precarica attiva.
- Nei campioni successivi, stato 0x01 = 0x32: carica a corrente costante; tensione 3032 e poi 3059 mV.
- IRQ 0x4a = 0x08: avvio della ricarica. Le letture I²C sono riuscite.
- I registri 0x61, 0x62, 0x63, 0x64, 0x65 e 0x67 sono rimasti invariati.

La prova conferma sul dispositivo il blocco della carica dovuto alla configurazione TS mancante. Non certifica capacità residua o salute della cella dopo la scarica. Il gauge può restare inizialmente a 0% mentre la batteria recupera. Log: `build/ecos-checks/amoled175-pmu-ts-fix.log`.

Installata la build locale 1.7.1 nello slot OTA1, conservando OTA0 e senza scrivere NVS o storage. Dopo il completamento del boot e un successivo riavvio: `App version: 1.7.1`, `AXP2101: battery 0% 3458 mV, charging, USB`, arrivo a `Ready` e ritorno da `app_main`. La carica prosegue anche con il firmware completo; il valore percentuale iniziale rimane basso mentre la tensione recupera. Log: `build/ecos-checks/amoled175-171-verified-boot.log`.
