# Memo vocali: verifica in corso

## Stato — 12 ottobre 2026

Prototipo interno sull'AMOLED 1.8 collegato a COM3, firmware diagnostico
`1.3.7-dev.1`. Non pubblicato nel canale stabile: l'utente segnala memo
silenziose, mentre conferma che la sveglia emette suoni.

`CONFIG_WATCH_VOICE_MEMOS` è un'opzione sperimentale disabilitata per default.
È attiva solo nella configurazione diagnostica dell'AMOLED collegato.

La cattura hardware sugli altri tre dispositivi è esplicitamente non
supportata in questo primo step. Le relative app non vengono mostrate.

## Funzioni realizzate

- Pulsante rosso a schermo intero, microfono vettoriale e brevi animazioni
  di pressione/rilascio. Nessuna animazione continua quando inattivo.
- Tenere premuto per registrare; rilasciare per salvare. Swipe verso il
  basso durante la pressione per bloccare; secondo swipe e rilascio per
  terminare. Uscita, pausa e sveglia richiedono comunque l'arresto.
- Menu dal click BOOT sui dispositivi con pulsanti; pressione lunga mantiene
  il ritorno alla home. Fallback touch quando manca un controllo fisico;
  rotazione della ghiera apre la libreria, dove seleziona i file.
- WAV PCM mono, 16 bit, 16 kHz, massimo 60 secondi, locale sulla microSD.
  Riserva di 4 MB, scrittura a blocchi, finalizzazione e recupero `.part`.
- Nomi dei nuovi file con data e ora; nomi precedenti interpretati e ordinati
  cronologicamente. Il menu si apre sulla memo più recente e mostra la
  posizione nella lista delle 20 più recenti. File precedenti conservati.
- Grafico di 24 picchi reali in registrazione e riproduzione, scala digitale
  da -60 a 0 dBFS; colori per picchi elevati e saturazione. Non è un FFT,
  uno spettrogramma di frequenza né una misura calibrata in dB SPL.
- Volume memo esplicito e regolabile durante l'ascolto, senza cambiare
  l'impostazione globale. Nessuna normalizzazione automatica del rumore.
- Nessuna registrazione automatica, upload o riconoscimento della voce.

## Evidenza raccolta

Build finale riuscita sui quattro firmware di produzione, con prototipo e
console diagnostica disabilitati. Riuscita anche la build AMOLED con prototipo
attivo e console diagnostica. Nessuna nuova release stabile di questo step.

Flash dell'AMOLED limitato all'immagine OS in ota0; verificati identità/MAC,
nome immagine e hash. NVS e app esterna in ota1 conservate.

WAV scaricati con verifica di dimensione, continuità dei blocchi e formato.
Le prime registrazioni contenevano un transitorio di avvio con saturazione;
dopo il primo secondo il livello RMS era circa 4–7 campioni (-78…-73 dBFS).
File più recenti avevano RMS complessivo circa 20 e picchi 358–534, ancora
molto bassi. La presenza di campioni non dimostra che la voce sia registrata.

ALDO1 e ALDO2 risultano già accese a 3,3 V sul dispositivo collegato;
l'alimentazione non è quindi una causa dimostrata del silenzio. I registri
ADC letti rispecchiano il percorso analogico abilitato, PGA 30 dB, scala
digitale 24 dB, volume ADC 0 dB, ALC disabilitato e uscita ADC sulle slot.
Confronto ricezione mono/stereo: nessuna prova di miglioramento della voce.

Il grafico e la libreria sono stati acquisiti dal dispositivo. I log BLE
possono intercalarsi ai blocchi della console; le acquisizioni incomplete
vengono respinte e ripetute. Il controllo temporaneo dei log e il WAV di tono
sono disponibili soltanto con console di sviluppo, assente dalle produzioni.
Il tono sintetico è marcato «Prova audio» e non sovrascrive memo esistenti.
Il WAV di prova recuperato via USB è lungo 1,5 secondi: RMS 8294, picco 12000,
nessun campione saturo. Il suo ascolto tramite il lettore delle memo è ancora
da confermare sul dispositivo; il file sintetico non contiene dati personali.

Prova sul dispositivo: primo swipe blocca, la registrazione continua dopo il
rilascio (schermata acquisita a 12 secondi), secondo swipe e rilascio salvano.
Click BOOT apre la libreria sulla nuova memo, con data/ora e indice `1 / 20`;
nessun pulsante Memo sulla superficie di registrazione. Gesto trattenuto
dal pulsante, evitando la propagazione automatica alla schermata LVGL.
La coda finale di riproduzione viene drenata per massimo 200 ms, con controllo
di stop/sveglia ogni 20 ms; 80 ms non erano sufficienti a 16 kHz.

## Verifiche ancora obbligatorie

1. Riproduzione del tono noto attraverso lo stesso lettore WAV delle memo;
   confronto con una nuova registrazione parlata identificata per data/ora.
2. Livello utile della voce e ascolto percepibile; non correggere amplificando
   alla cieca un ingresso potenzialmente privo di segnale.
3. Conferma dell'ergonomia con il dito reale e su schermi differenti;
   blocco/sblocco e selezione iniziale sono verificati tramite input diagnostico.
4. Volume percepito, stop, uscita e cancellazione confermata.
5. Precedenza reale della sveglia durante registrazione/riproduzione.
   La prova automatica precedente non ha attivato il timer: non è superata.
   La conferma dell'utente che la sveglia suona non sostituisce questa prova.
6. Recupero dopo interruzione, SD piena/assente, registrazione massima,
   misura di heap e ripetizione dopo riconnessioni Wi-Fi/BLE.
7. Attivazione nelle produzioni e release soltanto dopo la verifica audio.

WAV personali e catture diagnostiche restano nella directory ignorata
`build/microphone-stage`; non devono entrare nei repository o nelle release.

Fonti hardware: [schema AMOLED 1.8](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8/ESP32-S3-Touch-AMOLED-1.8.pdf),
[guida ES8311](https://files.waveshare.com/wiki/common/ES8311.user.Guide.pdf).
