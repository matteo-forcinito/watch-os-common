# Solco — prototipo DJ standalone

AMOLED 1.8: build di sviluppo `0.1.0-dev.10`, installata e verificata in
orizzontale. Gli altri target restano alla precedente `0.1.0-dev.9`; questa
sessione non li ha installati o ripubblicati. Verbale: [TEST-REPORT-dev10.md](TEST-REPORT-dev10.md).
Non costituisce ancora una release professionale: EQ, overview dell'intera traccia, MIDI e controllo Orbit non sono implementati.

## Orizzontale su AMOLED 1.8 (dev.10)

Solco si apre a 448 x 368: ruotare fisicamente il dispositivo per usarlo.
La scelta vale solo per Solco e non cambia l'orientamento del launcher.
I deck A/B sono affiancati, con PLAY/CUE/FILE separati e forme d'onda piu alte.
Il tasto STUDIO > apre le altre funzioni; Drumpad e Strumenti hanno otto pad
su due righe da quattro. Il vinile resta circolare e intero, con < MIX, MENU,
PLAY/PAUSA e CUE sempre accessibili negli angoli.
Volume iniziale zero: alzarlo gradualmente con + o con lo slider.
Per riaprire: launcher -> App esterne -> Solco. Il binario e registrato in
/apps/Solco sulla microSD e come app dello slot. Nessun nuovo firmware del
launcher viene installato da questa procedura.

## Uso

- I due deck partono in pausa. PLAY avvia/ferma; CUE ferma e torna all'inizio.
- FILE apre una libreria scorrevole con selezione diretta: due demo integrate
  e i primi 32 file `.wav`/`.mp3` nella cartella `/music` della microSD.
  Il cambio file richiede il deck in pausa, per proteggere la riproduzione in corso.
- WAV supportati: PCM lineare a 16 bit, 44.100 Hz, mono o stereo.
  Nessuna conversione silenziosa di WAV incompatibili.
- MP3 mono/stereo: preparazione sul dispositivo, con avanzamento percentuale.
  Il brano viene decodificato una volta in PCM stereo 44.100 Hz e memorizzato
  in `/music/.solco`; una firma SHA-256 permette di riusarlo ai carichi successivi.
  La conversione delle frequenze usa un filtro sinc polifase. Servono circa
  10,6 MB per minuto di brano, oltre a una riserva di 8 MiB liberi sulla SD.
  Il file originale resta intatto; la cache non viene eliminata automaticamente.
  PLAY si abilita a preparazione completata. Anche il brano MP3 supporta scratch.
- Motore stereo; uscita stereo sul Knob, somma L+R mono sull'altoparlante
  AMOLED 1.8 e SenseCAP Watcher, così nessuno dei due canali della traccia viene scartato.
- Crossfader a potenza costante con margine di 3 dB per deck. VOLUME parte da 0%
  a ogni avvio: premere PLAY e alzare gradualmente con + o con lo slider.
  I tasti -/+ cambiano di un punto; MUTE ricorda il livello precedente.
  RIATTIVA dopo un avvio a zero imposta il 10%, solo su pressione esplicita.
- Misuratore del picco digitale con rilascio graduale (non misura il livello analogico).
- Inviluppi A/B ricavati dall'audio reale nei buffer (circa 743 ms di anteprima),
  con aggiornamento a 20 Hz; misuratore su scala da -60 a 0 dBFS.
- Nel mixer la ghiera Knob regola il volume, un punto per scatto. Il crossfader è touch.
- A > / B > aprono il singolo deck con disco rotante legato al tempo audio.
  < MIX torna al mixer. Trascinare il disco in senso orario/antiorario controlla
  davvero posizione e velocità dell’audio, anche all’indietro. Tenere fermo il
  dito arresta il vinile; rilasciarlo riprende PLAY soltanto se era già attivo.
  In pausa il gesto permette di ascoltare lo scratch, poi il deck resta fermo.
  Servono VOLUME > 0 e crossfader aperto verso quel deck per sentirlo.
  Prima versione sperimentale: interpolazione lineare, velocità limitata a ±4×,
  nessuna promessa di risposta da giradischi professionale o latenza misurata.
- Indietro/tasto di sistema riavvia nel launcher; anche un riavvio ordinario
  torna al launcher. Non cambia la tabella delle partizioni del dispositivo.

Il comando VOLUME regola il master digitale. Il codec resta al valore API 100;
sull'AMOLED la compensazione del profilo hardware comporta circa +3,6 dB
nel DAC: non equivale a guadagno analogico unitario. La percentuale non
rappresenta una misura del livello analogico. Il gracchiamento segnalato
anche sulle demo resta da diagnosticare; il KIT B non lo risolve in modo evidente.

Dopo il flash USB, Solco pubblica una copia del proprio binario nella microSD
se montata, sotto `/apps/Solco`, per ritrovarla in App esterne. Gli altri
applicativi non vengono modificati. Senza SD registra il nome dello slot
per i launcher che supportano le app installate direttamente in flash.

Per installare manualmente nel launcher, copiare la cartella `Solco` del pacchetto nella cartella
`apps` della microSD. Il firmware di test può anche essere avviato direttamente
dallo slot secondario via USB, senza microSD; un riavvio normale torna al sistema.
Per riaprire uno slot già installato, con l'ambiente ESP-IDF attivo:

```powershell
python watch-os-common/solco/tools/select_slot_usb.py COM3 --slot 1
# Sul Knob usare COM9.
```

## Vista scratch immersiva e ghiera

Sul Knob il disco occupa la larghezza del display circolare. Nella nuova
vista AMOLED orizzontale il diametro segue il lato corto, con comandi rapidi
negli angoli; la precedente dev.9 usava la vista verticale. Il centro apre il menu rapido: MIX, VINILE, PLAY,
CUE, FILE, volume e loop. Toccare VINILE per tornare subito al disco.
I solchi vengono rasterizzati una volta nella PSRAM; durante la rotazione
si invalidano solo i rettangoli del vecchio e del nuovo indicatore.

La corona esterna controlla lo scratch. Passare per il centro azzera il
riferimento angolare, per evitare salti. Cambiare direzione ribasa il
bersaglio sulla posizione attuale, senza inseguire il vecchio movimento.
Sul Knob ogni scatto della ghiera vale 50 ms di sorgente, limite +/-4x,
rilascio dopo 120 ms; il touch ha priorita'. Il menu offre GHIERA: VINILE/VOL
per scegliere il ruolo; in Studio, mixer e libreria la ghiera regola il volume.

Per un loop manuale: IN segna l'inizio; OUT segna la fine e attiva il loop.
EXIT esce, LOOP lo riattiva; 1/2 e x2 cambiano la durata dal punto IN.
IN mostra la conferma IN OK / premi OUT. Minimo 256 frame; OUT deve seguire
IN nella stessa traccia e x2 deve restare
entro la fine. CUE/cambio brano cancellano i punti. Non e' beat-sync o auto-loop.
Quattro pagine dedicate per deck conservano i bordi, oltre alla cache normale;
un inviluppo di 64 campioni ai bordi limita il salto dei tagli arbitrari.
Durante lo scratch il vincolo del loop e' sospeso, poi riprende al rilascio.
Se si esce dai punti, il rilascio o il ridimensionamento riparte da IN.

MP3 supportati come nella dev.4: conversione una sola volta in cache PCM.
Orbit Solco Test 0.5.4 apre le app via BLE; upload BLE musicale non incluso.
Il servizio file non gira in Solco: per ora copiare i brani sulla microSD.

## Architettura e misure

Il task storage su core 0 alimenta due cache bidirezionali da 131.072 frame
stereo (512 KiB ciascuna) in PSRAM. Ogni cache contiene 128 pagine da 1.024 frame,
identificate dalla posizione assoluta nel WAV, con prelettura prima e dopo la
testina. La cache copre circa 2,97 secondi; il brano intero rimane sulla microSD.
Il task audio su core 1 legge e interpola blocchi da 256 frame (5,805 ms), senza
filesystem, rete o allocazioni nel ciclo. Durante lo scratch la testina segue
il dito con un controllo di posizione; un dato mancante non fa saltare avanti
la posizione. Il contatore empty registra i blocchi che hanno incontrato dati
non ancora disponibili.
I comandi UI usano una coda limitata; LVGL viene aggiornato a 20 Hz e i testi
vengono modificati solo quando cambia il loro valore.
Questo primo prototipo mantiene DAC e CPU attivi anche in pausa.
Le due demo originali sono loop PCM preparati offline, incorporati nel binario:
non eseguono funzioni trigonometriche nel task che alimenta i buffer.

Ogni dieci secondi il log USB riporta blocchi prodotti, massimo tempo di mix,
blocchi con buffer insufficienti, errori restituiti dal driver, clipping e
riempimento dei buffer. Questi contatori non misurano da soli i dropout del DAC
o la latenza analogica: occorre acquisizione audio esterna per verificarli.

## Profilo audio e diagnostica

Fino alla dev.10 la coda di uscita reale era quella predefinita della scheda,
6 buffer DMA da 480 frame (65,3 ms a 44.100 Hz; 4 x 128 sul Guition): la
definizione `WATCH_AUDIO_TX_DMA_FRAMES=128` passata dagli script non era letta
da nessun driver e la console stampava un valore fisso. Ora Solco sceglie la
coda all'avvio con `audio_start_ex` (HAL di ogni scheda) secondo un profilo di
latenza; il firmware principale continua a usare `audio_start` con la coda
predefinita.

| Profilo | Blocco motore | Coda DMA | Coda in ms | Predefinito su |
|---|---|---|---|---|
| `safe` (0) | 256 frame | 4 x 256 | 23,2 | schede con altoparlante |
| `live` (1) | 128 frame | 4 x 128 | 11,6 | Knob (uscita di linea, hub) |

`solco-profile N` cambia profilo a caldo (riapertura dell'uscita con
dissolvenza); `@TIMING` riporta profilo, blocco e coda effettiva letta dal
driver. La coda non e la latenza complessiva del gesto (touch, UI, filtri
del codec, amplificatore): quella va misurata con acquisizione esterna.

I log periodici sono eseguiti dal worker, fuori dal task audio: stdout USB
puo attendere fino a 50 ms se il computer non legge.

La console `solco` include @TIMING con intervallo massimo tra scritture e
numero di intervalli oltre la capacita nominale della coda. Il test
`tools/test_stress_usb.py` verifica preparazione MP3 a freddo mentre B
legge dalla SD, poi sei cicli con i due file in riproduzione. L'opzione
`--allow-user-volume` abilita espressamente il collaudo con ascolto;
senza opzione lo script si ferma se il volume non e zero.

## Build locale

Eseguire `python solco/tools/prepare_projects.py` dalla cartella comune.
Lo script crea i wrapper `external_apps/solco` nei due progetti e junction locali
verso le dipendenze già installate. Non modifica i sorgenti delle dipendenze.

Con ambiente ESP-IDF 6.1 attivo, impostare:

```powershell
$env:WATCH_OS_COMMON_DIR = 'C:/Users/matte/Documents/ESP-IDF/watch-os-common'
$env:SOLCO_LOCAL_COMPONENTS = '1'
$env:IDF_COMPONENT_MANAGER = '0'
$env:PYTHONUTF8 = '1'
idf.py reconfigure
ninja -C build -j 4
```

La console di sviluppo delle build di collaudo è abilitata nel solo `sdkconfig`
locale (`CONFIG_WATCH_DEV_CONSOLE=y`), non nei default delle release.
I binari esterni hanno identità `solco_amoled` e `solco_knob`; non vanno inviati
all'aggiornatore del firmware principale come `amoled_watch` o `knob_os`.

## Test host

Compilare `tests/test_audio.c`, `main/mix.c` e `main/wav.c` con include `main` e
libreria matematica dove richiesta. I test controllano validazione WAV,
estremi del crossfader, somma di segnali correlati a fondo scala e volume zero.

## Caricare i brani da Orbit

Questa build esterna non avvia ancora BLE o Wi-Fi. Per caricare dal telefono:

1. Tornare al firmware principale e selezionare in Orbit il dispositivo corretto.
2. Nell'APK di prova aprire **Musica Solco**, quindi Carica file. La cartella
   `/music` viene aperta direttamente. Nella companion precedente usare File.
3. Il contenuto viaggia via Wi-Fi; BLE resta disponibile per la connessione
   iniziale e i comandi del sistema. Il trasferimento dei brani solo via BLE
   non e implementato.
4. Caricare MP3 oppure WAV PCM16 44.100 Hz, mono/stereo.
5. Aprire App esterne > Solco sul dispositivo e premere FILE sul deck in pausa.
   Scegliere il brano; per gli MP3 attendere il completamento della preparazione.

Il caricamento avviene dal sistema principale: Solco non mantiene la rete
attiva durante la riproduzione. Nessuna importazione dal telefono mentre suona.
Orbit tiene sessioni separate: il brano caricato su AMOLED non viene copiato
sul Knob. I due Solco sono indipendenti, senza hub o sincronizzazione.
Le varianti senza microSD richiedono ancora uno storage dedicato.

Il SoC ESP32-S3 supporta BLE, non Bluetooth Classic. Il Knob include pero
anche un secondo ESP32 con A2DP Classic: quel percorso porta lo streaming
del telefono direttamente al DAC e non fornisce file/campioni ai deck Solco.
AMOLED 1.8 non usa quel coprocessore. Riferimenti: documentazione
[Espressif S3 BLE](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/ble/overview.html)
e `ESP32-S3-Knob-Touch-LCD-1.8/docs/COPROCESSOR.md`.

L'APK **Orbit Solco Test 0.5.2** usa un identificatore distinto e firma debug,
puo convivere con Orbit Companion e richiede di collegare nuovamente i dispositivi.
Non e una release per lo store. Build e test automatici sono completati;
il trasferimento reale da telefono resta da verificare con l'utente.

La decodifica usa minimp3 (CC0), con sorgenti e revisione in
`third_party/minimp3/SOURCE.txt`. Nessuna decodifica compressa nel task audio.

Il file `Solco-original-scratch-test.wav`, se presente in `/music`, è materiale
originale di collaudo da 12 secondi, scritto via console USB senza sovrascrivere
altri file. È un vero WAV su SD, non un collegamento alla demo incorporata.

## Studio: drumpad, strumenti e movimento (dev.8)

Toccare SOLCO + nel mixer per aprire Studio. DRUMPAD offre otto pad:
kick, snare, clap, hi-hat chiuso/aperto, tom, shaker e rim. Tutti i suoni
sono originali, generati una volta all'avvio e riprodotti dalla PSRAM.
REC avvia una registrazione di una battuta in 16 passi, in overdub:
i colpi vengono assegnati al sedicesimo corrente e ripetuti dalla battuta
seguente. REC termina la registrazione; PLAY/STOP controlla il loop.
PLAY e REC restano in basso. Il pulsante ... apre kit, CLR BEAT e BPM -/+
(60-200 BPM); < SUONA torna ai pad. Otto pad piu grandi occupano il centro,
con lo stesso spazio anche per le note degli strumenti.
Il pattern viene salvato sulla microSD al termine della registrazione,
allo stop o al cambio tempo. Senza SD si può suonare, senza salvataggio.

STRUMENTI offre otto note di Do maggiore per piano elettrico, basso e
campana. Toccare il nome dello strumento per cambiarlo. Sono note
percussive a decadimento, senza sustain, pitch bend o campioni importati.
Le 16 voci sono condivise da pad, strumenti e sequencer. VOL apre il
master; sul Knob la ghiera continua a regolare il volume in Studio.
Il bus Studio ha un limitatore a 24.000 e il mixer riserva gradualmente
margine ai deck quando si suona, prima del master comune.

In MOVIMENTO si sceglie MARACAS oppure BATTERIA. IMPARA COLPO attiva
BATTERIA e ricomincia l'apprendimento: tenere fermo il device, poi fare
un colpo in avanti (silenzioso). Il testo passa da FAI IL COLPO DI PROVA a
BATTERIA: suono. I colpi successivi richiedono accelerazione nella direzione
appresa e frenata entro 20-240 ms; un movimento opposto da fermo non arma.
Tornare quieti riabilita il colpo; intervallo minimo 180 ms. Non ogni traiettoria
di ritorno e' distinguibile: la prova a mano resta necessaria.

L'inclinazione relativa alla posizione iniziale sceglie cinque componenti:
centro snare, sui due assi laterali hi-hat/open hat oppure kick/tom.
La stima di gravita' usa giroscopio e accelerometro; non misura la posizione
assoluta o il punto esatto nello spazio. Il display indica il suono scelto
all'inizio del colpo. AMOLED ha IMU; Knob no. MARACAS conserva la soglia
bidirezionale e cooldown di 90 ms. La qualita' del gesto richiede feedback reale.

La dev.8 suona localmente e non avvia BLE, Wi-Fi o ESP-NOW. Orbit puo
inviare l'avvio al launcher tramite il bridge Bluetooth del sistema;
una volta entrati nell'app standalone, questa build non espone ancora
controlli remoti. Il gruppo ESP-NOW resta da implementare con due modalita audio
selezionabili: audio locale su ogni dispositivo (due speaker, ad esempio
bacchetta sinistra e destra) oppure inoltro verso un'unica uscita.
La rete non e inclusa nella dev.8 locale: verra misurata separatamente,
con ESP-NOW tra dispositivi e BLE opzionale verso Orbit, senza router.

Orbit Solco Test 0.5.4 preferisce Bluetooth per l'apertura delle app,
anche se conserva un indirizzo HTTP. Upload binari e risposte BLE troppo
grandi non attivano piu il Wi-Fi automaticamente. Il trasferimento di
brani via Bluetooth richiede ancora un protocollo a blocchi; non e
implementato in questa build. Per ora copiare i file sulla microSD.

### Confronto kick e tom

KIT B è il nuovo timbro predefinito: kick più corto, fondamentale a 80 Hz
con attacco e armonici; tom a 155 Hz con modi aggiuntivi. Contiene meno
energia bassa rispetto ai primi suoni, per rendere più utile il piccolo
speaker. KIT A ripristina il confronto con kick 48 Hz e tom 105 Hz.
Il pulsante cambia solo questi due suoni, anche nel sequencer e nei gesti;
non modifica il master. Gli strumenti e gli altri pad restano uguali.
Il kit torna a B al riavvio. Sono disponibili 32 suoni più due riferimenti
A/B; 16 voci simultanee complessive. Un segnale digitale senza clipping
non garantisce l'assenza di distorsione analogica dello speaker.

Il layout AMOLED della dev.8 resta verticale; è stata accolta la possibilità
di un layout orizzontale. La rotazione richiede un percorso rendering/touch
verificato e non è ancora inclusa. Il Knob mantiene il layout circolare.

## Trasporto lento/veloce e reverse

Dal disco: centro -> VELOCITA. Otto scelte per deck: avanti/indietro a
0,5x, 1x, 1,5x e 2x. PLAY/pausa resta separato: scegliere una velocita' non avvia
un deck fermo. La testina parte dalla posizione attuale; un file in reverse
si ferma all'inizio, in avanti alla fine. Le demo restano cicliche.
TONO FISSO e' attivo di default e mantiene l'intonazione cambiando velocita'.
Nel menu VELOCITA il pulsante TONO FISSO lo disattiva per il comportamento vinile. CUE e cambio traccia conservano la velocita'
scelta, ma cancellano i punti del loop. Tornare ad AVANTI 1x per il normale ascolto.
Lo scratch prevale temporaneamente sul trasporto e al rilascio ritorna alla
velocita' scelta; il loop funziona in entrambi i versi.

Un'esperienza di controllo ispirata a Stem Player senza separazione automatica
e' compatibile con questa direzione. Questa build non separa voce/strumenti
ne' simula stem che non esistono: controlla la traccia completa.

I cambi di loop su SD vengono preparati in quattro pagine aggiuntive di staging
(16 KiB per deck oltre ai 16 KiB dei bordi attivi). Il loop precedente continua
mentre il worker legge i nuovi bordi; il nuovo intervallo diventa attivo solo
quando sono tutti pronti. Anche dimezzamento/raddoppio in reverse seguono questo
percorso, senza pubblicare un bordo ancora da leggere.

Transizione tra rate: rampa esponenziale di circa 120 ms; pausa e scratch
conservano il controllo rapido. Il passaggio per velocita quasi zero attenua
il segnale per non mantenere un campione DC. Key Lock incluso dalla dev.9; lo scratch conserva il comportamento vinile.


## Tono fisso (dev.9)

Disco -> centro MENU -> VELOCITA -> TONO FISSO: ON/OFF, indipendente per deck.
Avanti/indietro a 0,5x, 1x, 1,5x e 2x. Il reverse mantiene le parole al contrario.
Scratch touch e ghiera scavalcano il tono fisso; al rilascio torna alla modalita
scelta. CUE e cambio brano conservano la scelta ma azzerano lo stato DSP.

Time-stretch WSOLA stereo in tempo reale, con un solo allineamento per i due
canali; nessuna preparazione su telefono o accensione radio. Non e' separazione
degli stem. La conservazione dell'intonazione non garantisce timbro identico:
transienti e voci possono presentare artefatti, soprattutto agli estremi.
Consultare TEST-REPORT.md per le misure e le prove hardware effettivamente fatte.

AMOLED 1.75 e 1.75C restano due target separati. Senza microSD questa build
funziona come versione Lite: demo integrate, drumpad, strumenti e movimento;
nessun caricamento musicale dal telefono o nuova persistenza nel FAT interno.
LCD 1.3 ed e-paper senza uscita audio non sono inclusi in questa release audio.
