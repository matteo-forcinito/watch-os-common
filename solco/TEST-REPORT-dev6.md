# Solco 0.1.0-dev.6 — vinile immersivo e ghiera scratch

24 ottobre 2026. Prototipo esterno per AMOLED 1.8 e Knob, installazione USB.
La companion Orbit Solco Test 0.5.2 e il suo upload MP3/WAV restano compatibili.

## Interfaccia e trasporto

- Disco Knob da 116 a 250 px; AMOLED da circa 164 a 327 px. Solchi,
  etichetta centrale colorata per deck e indicatore angolare, senza bitmap.
- PLAY/CUE/FILE in primo piano; VOL apre un pannello master e CHIUDI lo
  richiude. Il volume completo resta visibile nel mixer e nella libreria.
- Knob: rotazione nella vista deck controlla il PCM, anche quello preparato
  da MP3. Un detent vale 2.205 frame, cioè 50 ms di sorgente. Limite ±4×.
  Rilascio automatico dopo 120 ms senza eventi, nel task audio; conserva PLAY/pausa.
- Etichetta centrale per alternare GHIERA/VINILE e GHIERA/VOLUME. Mixer,
  libreria e pannello volume aperto danno alla ghiera il controllo del volume.
  Il pannello aperto mostra il ruolo VOLUME e sospende il gesto touch sul disco.
- Un dito appoggiato sul disco mantiene il controllo anche se viene girata
  la ghiera. Il timeout della ghiera non rilascia il dito. Corretto anche
  l'angolo visuale quando una demo viene scratchata prima dello zero.
- Cache raddoppiata: 128 pagine da 1.024 frame, 512 KiB/deck, circa 2,97 s
  bidirezionali. Nuova diagnostica dell'ultimo frame/tag mancante.

## Stabilità e logging

Una build preliminare è rimasta accesa per circa 150 minuti. Sul Knob è
stato osservato un singolo intervallo tra scritture di 52.466 µs, late=1,
mentre empty/errors/clips restavano zero. Non equivale a un dropout analogico
misurato. Il normale ciclo audio conteneva ancora la stampa periodica:
lo stdout USB dell'SDK può attendere fino a 50 ms quando il computer non legge
(TX_FLUSH_TIMEOUT_US in esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c).

La stampa periodica è stata spostata sul sys worker, con richiesta non
bloccante dal task storage. Nessun log periodico, file o allocazione nel
normale ciclo audio. La prova successiva con riproduzione continua e porta
USB chiusa per 22 s ha lasciato late/empty/errors a zero sul Knob.
È una verifica mirata, non una nuova certificazione di ore della build finale.
Il profilo DMA dev.5 resta 6×128 frame, circa 17,4 ms di capacità, non latenza finale.

## Test

- Host MSVC /W4 /WX: test trasporto esistenti più avanti/indietro con detent,
  estensione del timeout, rilascio in PLAY/pausa, wrap del contatore ms e
  priorità del dito rispetto all'autorilascio: PASS.
- Build ESP-IDF 6.1 completate per entrambi i target. Anteprime reali delle
  due viste verificate; volume e ghiera verificati sulla scheda Knob.
- Knob, test USB `test_jog_usb.py`: MP3 reale, inverso 62.976 → 54.174 frame,
  diretto → 62.976; volume invariato in VINILE, cambiato e ripristinato in
  VOLUME e nel pannello VOL; ripresa PLAY dopo jog; EOF a 352.800 frame.
  Nessun empty/error/clip/late rilevato. Input encoder iniettato nella stessa
  pipeline UI del driver; la sensazione meccanica va confermata dall'utente.
- Knob, sei cicli di due file SD e conversione a freddo mentre B legge SD:
  PASS. Burst rapidi ±16 detent: 169.728 → 272.643 → 192.256 frame, con B
  demo in PLAY; attraversata la cache. Nessun dato mancante o errore.
- AMOLED: test touch MP3 sulla nuova geometria, rilascio in pausa e CUE:
  PASS sulla nuova geometria e ricontrollato sulla build finale.
- I tentativi con tocchi USB persi o stato manuale incompatibile sono conservati
  nei log e non vengono classificati PASS. Pressioni di prova da 220 ms per
  distinguere la risposta USB dalla consegna effettiva dell'evento alla UI.

I test numerici MP3/FFmpeg e companion della dev.4 non sono stati ripetuti:
quei sorgenti non cambiano in questa versione. Verbali storici inclusi.

## Correzione del bordo di fine traccia

Il nuovo test host usa una sorgente che rifiuta gli indici fuori dal file.
Prima della correzione fallisce: scratch inverso da cursor==length richiede
il campione oltre EOF e resta bloccato, perch? un cache miss conserva la posizione.
Ora il movimento inverso entra nell'ultimo frame valido prima della lettura;
le direzioni verso l'esterno del brano vengono mantenute silenziose ai bordi.
Il test host prima/fix fallisce e dopo/fix passa, senza disabilitare assert.
Questo corregge un caso concreto che poteva incrementare empty durante lo scratch.
Il collaudo USB Knob finale e PASS: EOF a 352.800 frame, quindi movimento
inverso a 344.007, held=0, PLAY conservato in pausa, empty/errors/clips/late=0.
Anche lo scratch AMOLED e PASS sul binario finale verificato contro la flash.
Massimi finali osservati: mixer Knob 1.545 us / AMOLED 1.562 us;
intervallo scritture Knob 8.411 us / AMOLED 10.552 us, late=0.
Log conclusivi: solco-knob-dev6-eof-verified.* e solco-amoled-dev6-verified-final.*.

## Installazione e ripristino

Solo slot secondario e selezione di boot, senza aggiornamento firmware Wi-Fi.
Sistema principale e tabella delle partizioni non riscritti. Il flasher non
scrive direttamente NVS; all'avvio Solco aggiorna i propri metadati del launcher.
L'installer ora mantiene il dispositivo in download mode tra verifica,
scrittura e selezione del boot, per evitare che un launcher cambi lo slot
nel mezzo dell'operazione. Riavvio soltanto alla fine. select_slot_usb.py adatta l'inizializzazione
SDK usando argomenti con nome: OtatoolTarget 6.1 inoltrava ancora argomenti
posizionali alla firma piu recente di ParttoolTarget, ignorando di fatto i
parametri di reset. L'adattatore non modifica l'SDK e riusa l'algoritmo SDK
per sequenze/CRC. Verificato nei log che ogni operazione mantiene il bootloader
fino all'unico riavvio conclusivo.

Durante il lavoro lo slot AMOLED è cambiato fra Casa 1.0.1 e Solco: le letture
in più parti non erano coerenti e sono state scartate dopo verifica MD5 fallita.
Il file misto è esplicitamente marcato NOT-A-BACKUP e non va ripristinato.
Una nuova lettura ROM contigua da 1,5 MiB, senza avviare applicazioni nel mezzo,
è stata verificata contro la flash; contiene Solco. Inoltre è stata conservata
l'immagine completa Casa 1.0.1 dal build originale, il cui header da 4 KiB
coincide con quello Casa osservato sul dispositivo. Non è un backup della flash intera.

## Limiti

Interpolazione scratch lineare; antialias a velocità alte, key lock, pitch,
EQ, loop, FX, MIDI e hub restano futuri. Nessuna misura analogica di latenza,
SNR, THD o qualità del gesto. Nessuna OTA generale agli altri modelli.

Upload da telefono: sistema principale → Orbit Solco Test → Musica Solco,
trasferimento via Wi-Fi, poi App esterne → Solco → FILE. Non è ancora un
trasferimento solo BLE né un upload mentre il mixer è in riproduzione.
Nessun telefono ADB collegato per un collaudo fisico end-to-end dell'APK.
