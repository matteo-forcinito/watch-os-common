# Sensori e ingressi — 12 ottobre 2026

Inventario dei quattro firmware attivi; e-paper escluso. La presenza fisica
di una periferica non implica che il driver sia già verificato.

| Dispositivo | Periferiche già utilizzate | Lavoro attuale | Da integrare/verificare |
|---|---|---|---|
| AMOLED 1.8, V1 collegata | Touch, RTC, AXP2101, batteria, speaker, SD | QMI8658: accelerazione, velocità angolare, temperatura chip; memo ES8311, voce e ascolto confermati | Sensibilità a distanza; sensore sotto movimento; consumo reale |
| AMOLED 1.75 | Touch, RTC, AXP2101, batteria, ES8311, memoria `/data` | Adattatore QMI8658 e app Sensori | Prova fisica IMU; ingresso ES7210 e due microfoni; registrazioni su flash con limiti di usura/spazio |
| SenseCAP Watcher W1-A | Touch, RTC, batteria/ricarica, espansore, ghiera/click, SD, fotocamera | Nessuna IMU integrata dichiarata | ES7243E, LED RGB e relativo consumo; eventuali sensori Grove scelti esplicitamente |
| Knob 1.8 | Touch, ghiera PCNT/fallback UART, SD, batteria ADC, audio/coprocessore | Nessuna IMU integrata dichiarata | DRV2605/LRA, microfono PDM, prove audio e batteria |

## App Sensori

App interna per i due AMOLED. Un unico worker temporaneo possiede il driver;
lettura a 10 Hz, aggiornamento UI a 5 Hz. Nessuna transazione I²C nel task LVGL.
Identità verificata prima della configurazione; impostazioni originali salvate
e ripristinate alla chiusura, quando una pagina copre l'app e a schermo spento.
Ogni transazione ha un timeout. Nessun reset del chip, modifica di GPIO IRQ,
calibrazione permanente, FIFO o rilevamento automatico di accessori esterni.

Valori negli assi del sensore: g e gradi/secondo. La temperatura è quella del
chip e non va usata come termometro ambiente. Nessun conteggio passi o bussola:
questo step non implementa algoritmi di fusione e non aggiunge un magnetometro.

## Microfoni

La registrazione resta avviata esplicitamente dall'utente, locale e limitata.
ES8311 su AMOLED 1.8: tono noto e nuova memo parlata ascoltati dall'utente;
file PCM integro, 5,04 secondi, RMS 170,74 e nessun campione saturo.
L'app è abilitata nella produzione 1.3.7 di questo modello, con console spenta.
Il livello resta contenuto; sensibilità a distanza e autonomia richiedono prove.
Nessun audio personale è pubblicato nei repository o nelle release.

ES7210 duale, PDM e ES7243E richiedono adattatori differenti: non basta copiare
la configurazione ES8311. Due ingressi non dimostrano cancellazione del rumore
o beamforming; serviranno acquisizione dei canali separati e misure reali.

## Fonti

- [QST QMI8658C, registri e modalità sincronizzata](https://files.waveshare.com/wiki/common/QMI8658C.pdf)
- [Waveshare AMOLED 1.8, schema V1](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8/ESP32-S3-Touch-AMOLED-1.8.pdf)
- [Waveshare AMOLED 1.75, riferimento hardware](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/HARDWARE_REFERENCE.md)
- [Waveshare Knob, periferiche ed esempi](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8)
- [Seeed Watcher, hardware](https://wiki.seeedstudio.com/watcher_hardware_overview/)

Le verifiche dell'AMOLED 1.8 valgono per la V1 collegata. Non estenderle alla
V2 con controller display/touch diversi senza identificazione e prove.
