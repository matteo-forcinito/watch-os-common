# Solco 0.1.0-dev.8 - spazio per suonare, vinile e loop

24 ottobre 2026. Build sperimentale per AMOLED 1.8, Knob e SenseCAP Watcher. Distribuzione USB
su COM3/COM6 e pacchetti Market Dev per le tre schede; nessun aggiornamento OTA del sistema principale. APK Orbit invariato (0.5.4).
Il gracchiamento dello speaker rimane aperto, come ultima priorita' richiesta.

## Interfaccia

Otto pad/note conservati: AMOLED da 128x46 a 168x60 unita' logiche (circa
+71% di superficie), Knob da 128x39 a 140x50 (+40%). Le dimensioni fisiche
AMOLED scalano con il pannello 368x448. PLAY/REC restano in basso; ... apre
kit, tempo e cancellazione del pattern. Strumenti mantiene il selettore patch.
Volume disponibile dal titolo; tutti i comandi precedenti restano accessibili.
SOLCO + ora ha uno sfondo che ne rende evidente il ruolo di ingresso Studio.

Vinile da 250 a 360 px sul Knob, da circa 327 a 368 su AMOLED. Il Knob usa
l'intero display circolare. Il centro apre/chiude il menu rapido con trasporto,
file, ritorno mixer, loop e volume. Il gesto resta sulla corona esterna.
Il passaggio nel centro non conserva un angolo ambiguo. La ghiera rimane
utilizzabile per lo scratch; un comando nel menu alterna vinile/volume.

Una prima implementazione di questa dev.8 ridisegnava i solchi vettoriali
per intero: misurati 441 ms come massimo passaggio UI durante la prova.
Non consegnata come versione finale. Sostituita con solchi RGB565 generati
una volta in PSRAM (259.200 byte Knob / 270.848 byte AMOLED), e invalidazione
delle sole aree del vecchio e nuovo indicatore. Nessuna allocazione per frame.

## Scratch e loop

Cambio di verso: il bersaglio ora riparte dalla posizione audio attuale,
senza continuare a inseguire quello accumulato nel verso precedente.
Test host di inversione con bersaglio ancora lontano, EOF e ghiera passati.

IN segna l'inizio; OUT attiva il loop; EXIT/LOOP disattiva/riattiva.
1/2 e x2 ridimensionano dal punto IN, entro la fine del file. Minimo 256 frame.
CUE/cambio traccia cancellano i punti. Nessuna quantizzazione o analisi BPM:
sono loop manuali, non beat-sync. Il bordo usa un taper di 64 campioni,
che attenua il taglio arbitrario ma non rende musicalmente perfetto ogni loop.

Quattro pagine PSRAM per deck (16 KiB/deck) conservano i bordi del loop,
indipendentemente dalla cache SD bidirezionale principale da 512 KiB/deck.
IN avvia gia' il precaricamento. Nessuna lettura SD nel task audio.
Una prova preliminare ha rilevato due blocchi senza campioni al dimezzamento:
un cursore fuori dal nuovo intervallo finiva in un punto interno non in cache.
Corretto il salto: ridimensionamento/attivazione e rilascio scratch fuori range
ripartono da IN, gia' precaricato. Il test host copre esplicitamente questo caso.

## Movimento

IMPARA COLPO attiva BATTERIA e azzera il riferimento. Tenere fermo il device,
poi fare un colpo in avanti (silenzioso): il display conferma l'apprendimento.
I colpi successivi richiedono onset nella direzione appresa e frenata entro
20-240 ms; ritorno alla quiete per riarmare e almeno 180 ms tra i colpi.
MARACAS conserva rilevamento bidirezionale e cooldown 90 ms.

Giroscopio e accelerometro stimano la gravita'; inclinazione relativa al
riferimento iniziale sceglie snare al centro oppure hi-hat/open hat/kick/tom.
La selezione viene fissata all'inizio del colpo. Non e' tracciamento spaziale,
non rileva la posizione assoluta del piatto, e traiettorie di ritorno complesse
richiedono ancora prove reali. Test sintetici di onset/frenata, rifiuto del
ritorno semplice, cooldown e cambio strumento con tilt passati. L'ergonomia
non e' validata dall'ascolto dell'utente. Knob resta privo di IMU.

## Radio e limiti

Nessun avvio BLE, Wi-Fi o ESP-NOW da Solco. Hub e upload canzoni BLE ancora
non implementati; l'APK 0.5.4 apre dal sistema via BLE, senza accendere Wi-Fi.
Landscape AMOLED, EQ, pitch, auto-sync, MIDI e altri modelli non inclusi.
Nessuna modifica dei gain analogici o dei timbri per tentare di mascherare
lo scrocchio. Preferenza utente per il kick KIT A registrata.

## Trasporto a velocita' selezionabile

Aggiunto menu centrale -> VELOCITA: +/-0,5x, +/-1x, +/-1,5x, +/-2x per ciascun deck.
Selezione indipendente da PLAY/pausa. CUE e cambio brano conservano il rate;
lo scratch lo scavalca fino al rilascio. Fine/inizio WAV fermano il trasporto
nel verso appropriato; demo cicliche. Velocita' e intonazione restano accoppiate.
Non incluso time-stretch a pitch costante, ne' separazione automatica stem.
Test host di tutti gli otto rate, pause e ritorno dal gesto al rate scelto passati.

### Preparazione atomica dei bordi e recupero IMU

Aggiunte quattro pagine di staging per deck (16 KiB): i nuovi IN/OUT diventano
attivi solo dopo il precaricamento completo, mentre il vecchio intervallo suona.
Il cambio aggiorna atomicamente i bordi e preserva il rate/trasporto corrente.
Corretto anche il loop reverse con IN=0: non deve essere bloccato dal limite
iniziale del file. Test host esplicito del reverse su intervallo da zero passato.

Osservato un avvio IMU fallito dopo reset con sensore ancora attivo. Il profilo
Solco ora abilita WATCH_QMI8658_RECOVER_SAME_PROFILE=1: solo se registri di range,
ODR, filtro ed enable coincidono esattamente col suo profilo, lo riprende e salva
un ripristino inattivo. Il driver normale conserva la protezione precedente;
nessun takeover di profili diversi. Il firmware principale non e' stato aggiornato.

Transizione tra rate: rampa esponenziale di circa 120 ms; pausa e scratch
conservano il controllo rapido. Il passaggio per velocita quasi zero attenua
il segnale per non mantenere un campione DC. Nessun Key Lock incluso.

## Verifica finale su hardware

AMOLED 1.8 COM3, prima variante con riserva a quattro pagine: suite USB di navigazione, pad/note, tempo,
scratch avanti/indietro in pausa, loop WAV SD oltre 3 secondi, dimezzamento,
raddoppio e uscita PASS. Otto rate selezionati dalla UI, loop SD +2x/-2x/+0,5x,
ridimensionamento in reverse e ritorno +1x PASS. Nessun aumento di empty/errors/
clips/late; volume utente conservato al 52%. Massimi mix 1916 us, ciclo 8738 us.
UI scratch massimo 15 ms, ingresso vinile 82 ms. Il primo test del tempo era
errato al limite di 200 BPM: corretto il test per variare e ripristinare anche
ai limiti, senza modifiche al firmware.

Recupero movimento verificato su AMOLED: reset a caldo con BATTERIA attiva,
riavvio della stessa app, riattivazione e 186 letture in circa 2 secondi,
mode=2, errors=0. Movimento poi disattivato. Questo verifica l'avvio del
sensore; non certifica ancora la precisione musicale dei gesti reali.

Knob: precedente immagine dev.8 USB con tutte le modifiche DSP ha passato
suite completa e otto rate, incluso loop reverse e ridimensionamento, con
empty/errors/clips/late=0; massimi mix 1875 us e ciclo 8385 us. L'immagine
finale cambia la didascalia IMU non supportata in "NESSUNA IMU" e parametrizza
la riserva loop, ora estesa a sedici pagine anche sul Knob. Questa ultima
variante non ha ricevuto prove sul Knob fisico. Compilata ma non reinstallata: Knob scollegato. Il pacchetto
include la ricevuta USB precedente esplicitamente identificata come tale.


## Adattamento SenseCAP Watcher

Porta COM6 tramite CH342, ES8311 mono e microSD SPI. Nessuna IMU:
MOVIMENTO resta indicato come non supportato. Vinile largo 412 pixel,
cache grafica RGB565 339488 byte. La partizione applicazione secondaria
parte da 0x800000; non sono stati scritti sistema principale, bootloader,
tabella partizioni o NVS factory. Backup preventivi di metadati e prefisso
slot conservati solo localmente.

La prima suite completa e' passata, ma il test isolato del loop WAV in
reverse a -2x ha trovato quattro blocchi senza campioni, senza errori I2S,
clipping o deadline audio saltate. Aumentata la riserva
loop da 4 a 16 pagine per deck: otto dall'inizio e otto dalla fine, 64 KiB
attivi e 64 KiB di staging per deck. La ripetizione ha individuato lo stesso rischio anche su AMOLED: riserva
estesa a sedici pagine su tutte e tre le schede. La cache principale bidirezionale resta 512 KiB.
Il test viene ripetuto su questa build prima della pubblicazione.


SenseCAP, immagine con riserva finale a sedici pagine: suite completa USB
PASS; otto rate UI PASS; loop SD +2x/-2x/+0,5x e ridimensionamento durante
reverse PASS. empty/errors/clips/late tutti zero, volume conservato a zero
(prova automatica muta, nessuna valutazione acustica dello speaker).
Massimi mix 1795 us, ciclo audio 8490 us. UI scratch 16 ms, ingresso 91 ms.
Il primo tentativo di harness sulla UART usava erroneamente 9600 baud:
corretto a 115200. Corretto anche il parser che trattava @ERROR (stato
applicativo) come @ERR (fine comando), causando risposte seriali disallineate.


AMOLED, immagine finale con sedici pagine: suite USB completa PASS e otto
rate UI PASS, incluso reverse SD -2x, ridimensionamento, +0,5x e ripristino
+1x. empty/errors/clips/late=0, volume conservato a zero; massimi mix 2213 us,
ciclo audio 7430 us. Screenshot reali USB di drumpad e vinile controllati
su AMOLED e SenseCAP; sul Watcher nessun testo di intestazione tagliato
dalla sagoma rotonda. Le prove automatiche misurano integrita del flusso e
risposta UI, non sostituiscono ascolto e prove musicali prolungate.

Nessun firmware del sistema principale e' stato aggiornato tramite Wi-Fi.
Sono pubblicati solo i pacchetti applicazione specifici nel canale Market Dev.
