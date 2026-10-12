# Memo vocali: verifica AMOLED 1.8

## Stato — 12 ottobre 2026

Prototipo interno sull'AMOLED 1.8 collegato a COM3, firmware diagnostico
`1.3.7-dev.1`. Il 12 ottobre l'utente ha confermato sia il tono di prova
sia una nuova memo parlata udibile. Release di produzione prevista: `1.3.7`.

`CONFIG_WATCH_VOICE_MEMOS` resta disabilitata per default nel codice comune.
È abilitata nei default di produzione AMOLED 1.8; console diagnostica spenta.

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

Build di compatibilità riuscite sui quattro firmware e build diagnostica
AMOLED con memo e Sensori. La produzione AMOLED abilita le memo; gli altri
modelli mantengono gli adattatori microfono non supportati e app nascosta.

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
nessun campione saturo. Il suo ascolto tramite il lettore delle memo è stato
confermato dall'utente: «Il tono si sente». Il file non contiene dati personali.

Nuova registrazione parlata identificata per data/ora e scaricata solo via USB:
5,04 secondi, PCM 16 kHz mono 16 bit, RMS 170,74, picco 1536, DC 2,81,
zero campioni saturi. RMS dei primi cinque secondi: 119,45 / 230,60 /
184,02 / 155,12 / 146,82. Conferma dell'utente: «sento la memo».
Il livello resta contenuto: non è una misura calibrata di sensibilità e non
dimostra ascolto a distanza. Nessuna amplificazione automatica introdotta.

Prova sul dispositivo: primo swipe blocca, la registrazione continua dopo il
rilascio (schermata acquisita a 12 secondi), secondo swipe e rilascio salvano.
Click BOOT apre la libreria sulla nuova memo, con data/ora e indice `1 / 20`;
nessun pulsante Memo sulla superficie di registrazione. Gesto trattenuto
dal pulsante, evitando la propagazione automatica alla schermata LVGL.
La coda finale di riproduzione viene drenata per massimo 200 ms, con controllo
di stop/sveglia ogni 20 ms; 80 ms non erano sufficienti a 16 kHz.

## Priorità audio e verifiche residue

Aggiornamento del 12 ottobre, prova controllata successiva: timer impostato
esplicitamente a un minuto, registrazione iniziata durante il countdown.
Alla scadenza il recorder ha chiuso 1.241.600 byte PCM (38,8 secondi) con
`ESP_OK`; il codec ha aperto l'uscita allarme a 22.050 Hz 14 ms dopo.
Questa verifica supera il controllo di precedenza e rilascio del codec
descritto sotto; resta da confermare il suono percepito. Touch diagnostico
rilasciato e timer cancellato nel blocco di pulizia della prova.

Verificati: tono noto, registrazione parlata/ascolto, nomi datati e selezione
iniziale, blocco/sblocco diagnostico e precedenza del timer in registrazione.
Prima di estendere la validazione ad altri hardware, restano:

1. Ergonomia con dito reale e su schermi differenti.
2. Volume percepito in ambienti diversi; ascolto della sveglia durante memo
   e prova di precedenza durante riproduzione.
3. Recupero dopo interruzione, SD piena/assente e durata massima su hardware.
4. Misure di autonomia e prove prolungate con riconnessioni Wi-Fi/BLE.

Queste prove residue non sono presentate come già superate. Durata, spazio,
validazione WAV e recupero `.part` sono protetti nel codice; la registrazione
richiede sempre microSD montata e azione manuale dell'utente.

WAV personali e catture diagnostiche restano nella directory ignorata
`build/microphone-stage`; non devono entrare nei repository o nelle release.

Fonti hardware: [schema AMOLED 1.8](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8/ESP32-S3-Touch-AMOLED-1.8.pdf),
[guida ES8311](https://files.waveshare.com/wiki/common/ES8311.user.Guide.pdf).
