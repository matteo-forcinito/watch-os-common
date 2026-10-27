# Risonanza — piastra di Chladni virtuale

App esterna, build di sviluppo `0.1.0-dev.1` per AMOLED 1.8 (wrapper in
`ESP32-S3-Amoled-Touch-1.8/external_apps/risonanza`). Si apre in orizzontale
a 448 x 368; il launcher resta nel suo orientamento.

La figura e una **simulazione guidata dal sensore**, non la ricostruzione della
superficie misurata: le frequenze rilevate scelgono il modo (m,n) di una piastra
quadrata simulata, l'intensita della vibrazione decide quanto si agitano i granelli,
che si raccolgono lungo le linee nodali.

## Schermata

- A sinistra: frequenza dominante, segnale (256 campioni, DC rimosso), spettro
  a 64 bande, RMS/picco, fiducia, avvisi di saturazione o tempi irregolari.
- A destra: la piastra con 680 granelli.
- In basso: sorgente (IMU / MIC / ESPLORA), CAL, congela, A, B, A|B.

## Sorgenti

- **IMU** (QMI8658 a ~112 Hz, +/-4 g): vibrazioni meccaniche 2-40 Hz, asse con
  piu varianza. CAL misura il rumore di fondo a dispositivo fermo.
- **MIC** (ES8311 a 8 kHz): suoni 60-2400 Hz. Abilitato solo in questa app
  (`CONFIG_WATCH_VOICE_MEMOS` definito per il solo componente hardware).
- **ESPLORA**: nessun sensore; trascinando sulla piastra si sceglie il modo
  (orizzontale) e l'energia/smorzamento (verticale).

## Congela e confronta

Il tasto pausa congela misura e figura. A e B salvano la misura mostrata in NVS
e, se c'e la microSD, in `/sdcard/risonanza/A.csv` e `B.csv` (scrittura nel
worker di sistema, mai nel task LVGL). A|B mostra le due misure, il rapporto delle
ampiezze (solo se stessa sorgente e stesso asse) e permette di rivedere la figura
di ciascuna.

## Test

`tests/test_dsp.c` verifica il DSP sull'host (toni, DC, fuori banda, saturazione,
temporizzazione, confronto, simmetrie dei modi). Console di sviluppo: comando
`risonanza` stampa la misura corrente.

Da fare sul dispositivo: flash e prova di IMU, microfono, salvataggio e Esplora.
