# Solco 0.1.0-dev.3 — scratch PCM e collaudo USB

24 ottobre 2026. L’utente ha confermato che l’audio dall’altoparlante AMOLED
si sente con la dev.2. La dev.3 mantiene il volume unico e l’avvio a zero.
L’utente ha poi confermato sulla dev.3 AMOLED: «Si sente e segue il gesto».
È una verifica funzionale soggettiva, non una misura strumentale della latenza.

## Modifiche

Scratch reale su demo e WAV PCM16 44,1 kHz, mono/stereo: cursore Q16,
interpolazione lineare bidirezionale, servo di posizione circa 25 ms e
velocità limitata a ±4×. Un giro del disco corrisponde a 1,8 secondi di sorgente
(33 1/3 RPM). Il dito fermo frena la testina; il rilascio conserva PLAY/pausa.
Un piccolo inviluppo riduce il residuo quando il vinile è fermo.

Ogni deck usa 64 pagine da 1.024 frame stereo, 256 KiB in PSRAM: circa
1,49 secondi di audio, riforniti prima e dopo la testina. Il WAV intero resta
sulla microSD. Il task audio legge la cache senza file, allocazioni o rete.
In caso di cache miss emette silenzio e conserva la posizione, contando il
blocco nel contatore empty. Le pagine hanno tag assoluti per evitare alias
di vecchi dati quando la cache circolare viene riutilizzata.

Vista del vinile ingrandita su AMOLED; titoli limitati a una riga con ellissi;
misuratore spostato per lasciare libero lo slider volume. Le demo rimangono
incorporate. La copia SD di Solco viene aggiornata all’avvio del binario USB.

## Test

- Build ESP-IDF 6.1 per AMOLED 1.8 e Knob completate. Binari con identità
  specifica della scheda, verifica hash dopo la scrittura USB.
- Nuovi test host del trasporto passati con MSVC /W4 /WX: movimento inverso,
  convergenza a dito fermo, rilascio in PLAY e in pausa, limiti del WAV,
  loop attraversato all’indietro e cache miss senza salto di posizione.
- I test del mixer/parser delle versioni precedenti restano pertinenti;
  mix.c e wav.c non sono stati modificati in questa versione.
- Creato via console un WAV originale da 12 s e 2.116.844 byte sulla microSD
  di ciascuna scheda, con O_EXCL (mai sovrascrittura):
  /music/Solco-original-scratch-test.wav. A contiene il WAV, B una demo continua.
- Verificata lettura oltre la capacità della cache e fine file a 529.200 frame.
- Test USB touch completato su entrambi, con volume sempre 0: attivazione,
  movimento circolare inverso e diretto sul WAV, rilascio del WAV in pausa,
  presa/rilascio della demo B mentre è in PLAY. Al termine entrambi in pausa,
  velocità 0 e nessun vinile rimasto agganciato al dito.

| Target | Posizione WAV prima | Dopo gesto inverso | Dopo gesto diretto | Mix massimo osservato |
| --- | ---: | ---: | ---: | ---: |
| AMOLED 1.8 | 190.720 | 170.878 | 190.719 | 1.460 us |
| Knob | 279.296 | 259.455 | 279.295 | 1.520 us |

Budget per blocco: circa 5.805 µs. In entrambe le prove empty=0, errors=0,
clips=0. Il volume zero rende il controllo clipping in questa prova limitato:
non sostituisce il test numerico di somma a fondo scala già presente.
Campioni finali di salute: memoria interna libera 177 KiB AMOLED / 201 KiB
Knob; PSRAM libera circa 7.626 / 7.634 KiB; nessun flush IRQ perso. Le
misure UI raccolte includono avvio e screenshot, quindi non vengono usate
per dichiarare gli FPS.

Il tempo massimo comprende anche la fase di avvio; non è una misura della
latenza touch/analogica. I test non sono un soak di ore.

Alcuni tentativi preliminari dello script hanno ricevuto risposte USB parziali.
La loro attesa ha fatto raggiungere l’EOF prima del gesto in avanti, causando
il fallimento dell’asserzione del test. Lo script finale ricompone lo stato
mediante tentativi limitati, ferma la prova se l’utente modifica il volume e
separa i casi WAV/EOF dalla ripresa su loop. I log preliminari non sono stati
usati per dichiarare un PASS dello scratch.

## Distribuzione e limiti

Installazione di sviluppo via USB su COM3/COM9, senza firmware via Wi-Fi.
Lo slot principale non è stato riscritto. La copia SD resta sotto /apps/Solco.
Le ricevute e i log sono in progress-files/solco-*. I pacchetti includono
impronte binario, configurazione e sorgenti in build.json.

BLE e Wi-Fi non sono avviati da questa app esterna. I brani da telefono si
caricano dal firmware principale, Orbit → File → /music; Orbit usa il Wi-Fi
per il contenuto. Il trasferimento di brani interamente BLE, il controllo
Orbit dentro Solco e il collegamento hub/controller non sono implementati.
Entrambi i dispositivi ESP32-S3 supportano BLE, non Bluetooth Classic.

Scratch sperimentale: interpolazione lineare, senza filtro antialias dedicato
alle velocità elevate. Non sono stati misurati latenza end-to-end, rumore,
THD, SNR o dropout analogici. Il buffering DMA del driver rimane quello
preesistente (AMOLED 6×480 frame, circa 65 ms): non dichiarare latenza da
scratch professionale. Il materiale SD è sintetico originale, non una libreria
musicale di produzione. Schede lente/errori I/O restano da collaudare.
MP3, pitch indipendente, key lock, EQ, loop, sampler, FX e MIDI restano futuri.
Nessuna release OTA generale pubblicata. Console di sviluppo attiva nei pacchetti.

Storico: TEST-REPORT-dev1.md e TEST-REPORT-dev2.md.

Riscontro utente successivo al collaudo: scratch AMOLED udibile e coerente con
il gesto. Nessuna conferma equivalente dell’uscita AUX Knob ancora raccolta.
