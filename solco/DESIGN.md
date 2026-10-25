# Solco: obiettivi e criteri di avanzamento

## Hardware di collaudo

- AMOLED 1.8, COM3: primo ascolto attraverso l'altoparlante, microSD, touch,
  display rettangolare 368×448. Identità principale verificata: `amoled_watch`.
- Knob 1.8, COM9: rotella, microSD, display rotondo 360×360, DAC stereo.
  Identità principale verificata: `knob_os`. L'utente non dispone attualmente
  di un cavo AUX: nessun ascolto analogico certificato su questo target.
- AMOLED 1.75C: target distinto, senza microSD; non più collegato durante
  questo collaudo. AMOLED 1.75 con schermo rotto resta un ulteriore target
  distinto, da gestire anche attraverso comandi diretti senza mirroring.

## Prima milestone: motore e misure

Il prototipo esterno riduce le interferenze con i servizi del sistema principale.
Un solo codice condiviso gestisce i due display attraverso `board_info()`.
La scelta interna/esterna definitiva dipenderà dai margini misurati.

Prima di ampliare il DSP:

1. Avvio su entrambe le schede, identità e RAM registrate nei log.
2. Due sorgenti contemporanee, crossfader e master durante l'aggiornamento UI.
3. Nessun errore restituito dal driver audio; nessun buffer insufficiente dopo
   il riempimento iniziale. Il budget per blocco è 5.805 microsecondi.
4. Prove di play/pausa, ritorno all'inizio, cambio sorgente e fine file.
5. Prova con veri WAV sulla microSD e successivamente scheda lenta/errori I/O.
6. Sessioni prolungate e misura audio esterna prima di parlare di affidabilità
   da palco: i contatori software non dimostrano assenza di interruzioni analogiche.

Non dichiarare una prova hardware superata sulla sola base della compilazione.
I risultati effettivi vanno registrati in un verbale separato.

## Sviluppi successivi

- MP3 dev.4: preparazione asincrona in cache PCM, un importatore seriale e
  due deck indipendenti. Da misurare i tempi su brani lunghi e SD lente.
- Browser base dev.4 completato; restano metadati e cue persistenti.
- Pitch tramite ricampionamento, poi loop, EQ e filtro con budget misurato.
- Waveform preparata fuori dal task audio; nessuna analisi pesante nella UI.
- Trasferimento occasionale del "vinile" da Orbit: file temporaneo, spazio
  verificato, verifica integrità e attivazione solo dopo completamento.
- Modalità controller MIDI USB, con verifica del percorso USB per ogni scheda.
- Controller aggiuntivi che inviano comandi/stato all'hub; audio centralizzato.
- Variante senza microSD: limiti espliciti di spazio e funzioni, senza assumere
  che tutta la flash fisica sia disponibile ai brani.
- Integrazione interna solo dopo misure insieme ai servizi del sistema.

Key lock di qualità e scratch avanzato richiedono benchmark separati.
Master stereo e preascolto stereo indipendente richiedono quattro canali:
il DAC stereo del Knob da solo non li offre.

## Distribuzione

Flash e test locali via USB. Gli slot del firmware principale e la sua identità
restano invariati. I pacchetti Solco esterni sono specifici per scheda.
OTA generale e aggiornamenti Orbit seguiranno il collaudo delle rispettive
funzioni; il prototipo PCM non va presentato come prodotto completo.

## Decisione architetturale: gruppo e uscita audio indipendenti (24 ottobre 2026)

Il coordinatore del gruppo non deve essere necessariamente l'unico dispositivo
che suona. Tenere separati due parametri: ruolo di sincronizzazione e destinazione
dell'audio. ESP-NOW e il candidato per clock/eventi fra ESP32-S3; BLE resta il
trasporto Orbit per avvio/configurazione. Nessuna associazione a router o SoftAP
richiesta per queste funzioni. La coesistenza BLE/ESP-NOW deve essere misurata,
non assunta priva di costo: condividono radio e antenna dell'S3.

- Locale indipendente: ogni strumento suona senza rete.
- Gruppo, audio distribuito: ogni dispositivo riproduce i propri suoni;
  esempio due AMOLED1.8 come bacchetta sinistra/destra con due speaker.
  Il coordinatore distribuisce tempo, fase e trasporto; un guasto radio non
  deve impedire di suonare localmente. Stato di sincronizzazione visibile.
- Gruppo, uscita unica: i controller inoltrano i colpi all'uscita scelta e
  disabilitano soltanto la loro copia locale per evitare raddoppi.

Il sincronismo richiede un protocollo a timestamp e uno scheduler audio.
La ricezione MAC di ESP-NOW non equivale a consegna applicativa: usare sequenze,
conferme, deduplicazione, code limitate e scadenze. Non riprodurre un colpo vecchio
solo perche e arrivato dopo numerose ritrasmissioni. Separare colpi live dai
comandi affidabili di configurazione/trasporto. Non ritrasmettere audio PCM.

Per audio distribuito, stimare offset e deriva dei clock e correggere
progressivamente il sequencer; non saltare la posizione audio a ogni beacon.
Il fatto di avere un clock musicale condiviso non dimostra fase analogica
identica fra due DAC: questa richiede misure specifiche. Non promettere latenze
o sincronismi professionali prima di misurare errore, jitter, perdite,
riconnessioni, carico UI/SD e BLE contemporaneo sui dispositivi reali.

La dev.7 consegna il motore locale. Il modulo ESP-NOW preliminare salvato in
progress-files e escluso dalla build; va adattato a queste due modalita prima
qualunque prova di gruppo. Non sostituirlo automaticamente con un hub BLE.
