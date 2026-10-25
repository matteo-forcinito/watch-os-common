# Solco 0.1.0-dev.9 - tono fisso

25 ottobre 2026. Release di sviluppo, non certificata per uso professionale.
Obiettivo: cambiare velocita e verso senza trasporre l'intonazione della voce.

## Implementazione

WSOLA originale in dominio temporale: grani stereo di 2048 frame, hop di 1024
(23,2 ms a 44,1 kHz), ricerca +/-512 frame con correlazione normalizzata,
ricerca grossolana e rifinitura, sovrapposizione smoothstep complementare.
Entrambi i canali condividono lo stesso offset; la correlazione considera
entrambi separatamente per non cancellare contenuti in opposizione di fase.
Memoria circa 24 KiB/deck allocata una volta in PSRAM all'avvio. Nessuna
allocazione o operazione su file nel render. I grani usano la cache PCM esistente.
Nessuna radio viene avviata. Il normale resampling viene saltato quando il
segnale elaborato raggiunge il 100%, riducendo il carico nel caso continuo.

TONO FISSO attivo di default, indipendente per deck, pulsante nel menu velocita.
Otto rate +/-0,5/1/1,5/2x. Crossfade breve al cambio modalita; continuita della
coda OLA al cambio di direzione o intervallo loop. Scratch touch/ghiera torna
al comportamento vinile e al rilascio ripristina il tono fisso. CUE e cambio
brano conservano la preferenza ma azzerano i buffer DSP.

Il trasporto mantiene il proprio cursore e la rampa gia presente, cosi il tempo
della traccia avanza alla velocita selezionata. Reverse significa audio invertito,
non parole comprensibili in ordine normale. Nessuna separazione stem.

## Prove host

Test C /O2 /W4 /WX: 48 combinazioni delle frequenze 40,55,80,110,220,440 Hz con
ciascuno degli otto rate. Intonazione entro 3 Hz dal riferimento, tempo del
cursore corretto, stereo in opposizione di fase conservato entro 2 unita PCM,
nessun clipping, scratch e bypass, continuita a cambio verso/intervallo PASS.
Regressioni vinile (loop, EOF, ghiera) e Studio (sintesi, clock, movimento) PASS.

Frase italiana generata localmente con sintesi vocale Windows, nessuna voce
personale registrata. Render offline con lo stesso codice DSP e WAV di confronto
0,5x,1,5x,2x,reverse1,5x e controllo vinile1,5x. Il test verifica durata e stima
indipendente dell'intonazione; non sostituisce l'ascolto di musica reale.

## Schede

AMOLED 1.8 e Knob: nessuna porta USB disponibile alla verifica finale.
Pubblicazione autorizzata esplicitamente dall'utente anche senza questa prova.
Le immagini finali sono compilate, non installate o verificate fisicamente.
Sulla variante precedente all'ultimo scambio buffer: Knob ha superato suite,
velocita e stress Key Lock con due deck SD (-2x/+0,5x), senza errori,
clipping, cache vuota o deadline superate (massimo ciclo 16559 us).
AMOLED ha superato la suite funzionale, ma nella prova velocita ha registrato
un ritardo (ciclo massimo 18484 us). L'ultima modifica elimina la copia
64 KiB sotto mutex; il suo effetto sul dispositivo resta da verificare.
SenseCAP, AMOLED 1.75, AMOLED 1.75C e Guition 4848: build specifiche; dispositivi
non collegati, quindi nessuna nuova verifica fisica dichiarata per queste schede.
1.75 e 1.75C hanno immagini separate e, senza SD, solo funzioni Lite (demo,
pad, strumenti, movimento). Nessun upload musicale BLE implementato.
LCD 1.3 ed e-paper non inclusi: questa release richiede un'uscita audio.

## Distribuzione e limiti

App esterne, nessuna modifica al sistema principale o aggiornamento Wi-Fi sui
dispositivi collegati. Sull'AMOLED lo slot era occupato da Pieno: backup completo
del suo slot prima della sostituzione. Gli altri file/app sulla SD sono conservati.
Market schema2 attuale limitato a 64 voci totali: preservando le altre 59 app,
Solco puo coprire cinque schede nel catalogo. Guition viene pubblicato nella
stessa release come pacchetto diretto per SD; nessuna voce altrui rimossa.

Tono fisso non significa timbro identico: possibili artefatti su transienti,
percussioni e voce, specialmente 0,5x/2x. Scrocchio speaker preesistente ancora
aperto. Nessuna nuova funzione hub, MIDI, BLE upload o Key Lock durante scratch.

Riferimento algoritmico generale (nessun codice importato):
https://www.surina.net/soundtouch/readme.html (time stretching WSOLA-like).


Stima indipendente autocorrelazione sulla frase sintetica: riferimento mediano
232,1 Hz; 0,5x e 1,5x locked 232,1 Hz; 2x locked 238,4 Hz; reverse1,5x locked
232,1 Hz. Controllo vinile1,5x: 352,8 Hz. I valori della frase variano per prosodia
e finestre vocalizzate selezionate: non sono una misura di errore costante del DSP.
I toni stazionari dei test C restano entro 3 Hz. Durata riferimento 9,498 s;
0,5x 18,996 s; 1,5x 6,332 s; 2x 4,749 s. WAV di confronto nel pacchetto QA voce.


Una prova intermedia AMOLED ha registrato un ciclo da17562us (late1), quindi
non accettata. Render lungo protetto da mutex con priority inheritance, senza
mascherare IRQ. Cache normale O(1) prima della ricerca bordi; wrapping dei grani
incrementale, evitando divisioni per campione. Pubblicazione dei bordi loop
tramite scambio dei due buffer di pari capacita, senza copia64KiB sotto mutex.
Nessuna misura hardware finale disponibile per questa variante: rilascio
sperimentale con rischio residuo di ritardi audio, non dichiarato risolto.
