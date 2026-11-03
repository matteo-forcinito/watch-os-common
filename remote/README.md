# Remote 1.4.0

App esterna condivisa da Watcher W1-A, AMOLED 1.8, AMOLED 1.75 e Knob 1.8.
Ogni modello richiede il proprio binario: i pacchetti non sono intercambiabili.
Il codice comune è in `watch-os-common/remote/main`; il firmware principale non lo include.

## Installazione

- **Con microSD:** estrarre il pacchetto del proprio modello nella radice della scheda,
  ottenendo `apps/Remote/Remote.bin`, `manifest.json` e `icon.png`.
- **AMOLED 1.75:** aggiornare prima il firmware principale con il launcher per memoria
  interna. Aprire la pagina Web del dispositivo, caricare `Remote.bin` come app **Remote**,
  poi selezionarla nel launcher. Non occorre una scheda SD.
- Il launcher copia l'app nello slot secondario. Uscire da Remote o riavviare torna al sistema.
  Per aggiornare un'app nella piccola memoria interna può essere necessario rimuovere il
  vecchio file dalla pagina File: l'upload conserva la copia precedente fino al completamento.

## Collegamento

Associare **Watcher Remote**, **AMOLED Remote**, **Round Remote** oppure **Knob Remote**
nelle impostazioni Bluetooth del computer. Rimuovere e rifare una precedente associazione
dopo questo aggiornamento: il profilo HID ora comprende mouse e gamepad.

**Batteria.** In alto su ogni schermata c'è la batteria dell'orologio; via Bluetooth il livello arriva anche al computer (servizio Batteria). Uscire da Remote chiede conferma.

**Più computer.** Remote ricorda fino a 3 dispositivi associati. In Remote > Computer si
sceglie quello da usare: Remote si collega solo a quello e, se ne era collegato un altro, lo
lascia subito (cambio a caldo, senza toccare i PC). «Qualsiasi» torna al comportamento
precedente (vince il primo che si collega). «Aggiungi computer» accetta un solo computer nuovo,
che diventa quello scelto; tenere premuto un computer per dimenticarlo. I nomi arrivano
dall'agente Watch Deck sul PC; senza agente compare «Computer» con le ultime cifre
dell'indirizzo.

Sui tre modelli Waveshare è disponibile anche USB HID: scegliere USB in Remote e collegare
il cavo alla porta USB nativa. Il Watcher usa Bluetooth; la sua USB-C è una porta seriale.
Il cambio collegamento riavvia l'app. La console USB ritorna uscendo nel sistema principale.

In **Opzioni**, scegliere layout Italiano/US uguale al computer, sistema Windows/Linux o
macOS, Teams/Zoom e velocità del puntatore. Le app del computer possono personalizzare o
intercettare le scorciatoie; i comandi riunione sono predisposti per le app desktop.

## Modalità

| Modalità | Comandi |
|---|---|
| PC (Deck) | Touchbar che cambia con l'app in primo piano (VS Code, browser, file, Office, terminale, musica, chiamate), media, microfono, blocco PC, schermo spento, CPU/RAM e brano. Nelle pagine Sistema: «Cambia app» (Alt+Tab con Alt tenuto finché tocchi di seguito), «App aperte» (elenco delle finestre del PC, tocco = in primo piano), vista attività, desktop virtuali, Game bar, sospensione. Contesto e stato richiedono l'agente `tools/deck_agent` sul PC (Bluetooth o cavo USB; su Windows `install_windows.ps1`). Senza agente la pagina guida al collegamento. Vedi `docs/DECK.md` |
| Tastiera swipe | Lettere, numeri, simboli, accenti, maiuscole, suggerimenti IT/EN, spazio, cancellazione, Invio |
| Tastiera PC | QWERTY, numeri/simboli, Ctrl, Alt, AltGr, Shift, Win/Cmd, Tab, Esc, Invio e spazio; pagina Fn con F1–F12, frecce, Home/End, Ins/Del, PgUp/PgDn, Caps, Print, Scroll, Pause e Menu |
| Mouse | Tutto lo schermo è il touchpad, senza pulsanti: tocco = clic, due tocchi = doppio clic, tieni premuto = destro, tocca e poi trascina = trascinamento. Lungo i bordi: destro scorre in verticale, inferiore in orizzontale, sinistro regola il volume, superiore la luminosità (portatili). Un bordo agisce solo se il dito parte lì e si muove lungo il bordo; sui rotondi conta la fascia esterna. ≡ in alto: clic centrale, avanti/indietro |
| Air mouse | Il polso muove il puntatore mentre il dito resta sullo schermo (o sempre, con «Sempre attivo»). Tocco = clic, dito e polso fermi 0,6 s = destro, con «Sempre attivo» dito + polso = trascinamento. Bordi come nel mouse. La calibrazione si fa al primo avvio e resta salvata; ≡ in alto: opzioni e ricalibrazione. Solo sui modelli con giroscopio |
| Media | Play/pausa, precedente/successivo, volume con ripetizione, mute, stop ed espulsione se supportata dal computer |
| Presentazioni | Avvio PowerPoint, avvio dalla slide corrente, precedente/successiva, schermo nero/bianco, prima slide e termine |
| Riunioni | Teams/Zoom: microfono, video, mano, condivisione, toolbar/chat e uscita |
| Scorciatoie | 40 comandi: copia/incolla/taglia, testo e file, browser, navigazione e finestre; adattati a Windows/Linux e macOS |
| Gamepad HID | Assi X/Y, croce direzionale con STOP, A/B/X/Y a rombo, L/R, Select/Start; direzioni premute o fisse, stato visibile; configurare la mappatura nel gioco |

La tastiera PC seleziona i modificatori per **il prossimo tasto** e li rilascia insieme a esso.
Il pulsante **123 → Fn → abc** cambia pagina. Non è un terminale e non legge il testo sul PC.
Sui display rotondi, righe e controlli seguono le corde del cerchio.

Il pulsante ≡ in alto a sinistra apre il selettore: tutte le modalità a colpo d'occhio, la corrente evidenziata.
I layout seguono lo schermo come è orientato (anche ruotato in orizzontale).

Sui dispositivi con rotella: mouse = scorrimento, media = volume, presentazioni = slide.
Quando è attiva la navigazione tramite focus, la rotella continua a selezionare i controlli.

## Limiti e affidabilità

- HID invia comandi; non può leggere lo stato di microfono, camera, riproduzione o slide.
- Il gamepad è HID generico, non XInput. La compatibilità dipende dal gioco e dal sistema.
  Con **Direzioni: fisse**, toccare una freccia per mantenerla mentre si usa A/B/X/Y;
  ritoccarla o premere STOP la rilascia. Anche uscita e disconnessione rilasciano i controlli.
  Il touchscreen a un contatto limita le altre combinazioni simultanee.
- La coda è non bloccante: un comando rifiutato mostra un messaggio. Una frase o correzione
  swipe viene accettata nella coda tutta insieme. HID non fornisce una conferma del testo
  effettivamente scritto dall'applicazione sul computer.
- A riconnessione i report tornano neutri; tasti e pulsanti vengono rilasciati anche in caso
  di errore del trasporto. Le transazioni di una connessione precedente vengono scartate.
- Remote mantiene il gestore sveglie e la melodia del sistema. Non avvia Wi-Fi né Gadgetbridge
  contemporaneamente al collegamento HID.
- Le associazioni e le preferenze sono in NVS. L'app non invia dati a servizi cloud.

## Compilazione

Dal progetto di qualsiasi modello:

```powershell
tools/build_remote.ps1 -Board watcher
tools/build_remote.ps1 -Board amoled
tools/build_remote.ps1 -Board amoled175
tools/build_remote.ps1 -Board amoled175c
tools/build_remote.ps1 -Board knob
```

I pacchetti e gli SHA-256 vengono scritti in `dist` del modello scelto.
`-Development` abilita la console di sviluppo nella variante Bluetooth; non produce pacchetti stabili.

Le scorciatoie sono basate sulle documentazioni ufficiali di
[Microsoft Teams](https://support.microsoft.com/en-us/accessibility/teams/keyboard-shortcuts-for-microsoft-teams),
[Zoom](https://support.zoom.com/hc/en/article?id=zm_kb&sysparm_article=KB0067050) e
[PowerPoint](https://support.microsoft.com/en-us/powerpoint/present-your-slide-show).

## Altre fonti delle scorciatoie

[Chrome](https://support.google.com/chrome/answer/157179?hl=en),
[Windows](https://support.microsoft.com/en-us/accessibility/windows/keyboard-shortcuts-in-windows),
[macOS](https://support.apple.com/en-us/102650).
Le impostazioni delle singole applicazioni possono cambiare questi comandi.

Le prove eseguite e quelle ancora necessarie sono in `VALIDATION.md` nel progetto condiviso Remote.
