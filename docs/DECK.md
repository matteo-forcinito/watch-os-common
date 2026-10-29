# Remote Companion (Deck) — documento tecnico

29/10/2026. Versione condivisa e commentabile: https://claude.ai/artifact/G4S3xshiZy6erBJYwtVgLu

## Obiettivo e visione

Remote diventa **Deck**: una companion del PC che sa cosa stai facendo e mette sotto il dito i comandi
giusti. Oggi Remote è un trasmettitore HID cieco; il salto è un **canale dati bidirezionale** con un
agente sul PC: il dispositivo riceve contesto (finestra attiva, brano, microfono, CPU) e invia azioni.

- **HID resta il fondamento.** Senza agente tutto ciò che c'è oggi continua a funzionare, anche al BIOS.
- **Agente opzionale, valore enorme.** Touchbar contestuale, stato reale, macro, notifiche, streaming.
- **Ogni board dà il meglio di sé.** Rotella del Knob, IMU degli AMOLED, schermo del 4848, camera e
  microfono del Watcher diventano controlli specifici.

## Stato attuale (prima del Deck)

Remote (app esterna, `remote/main`, ~4.200 righe C) è una periferica HID via BLE o USB, a senso unico.

| Area | Oggi | Limite |
| --- | --- | --- |
| Trasporto | BLE HID (tutte), USB HID (Waveshare) | Nessun canale dati; cambio trasporto riavvia l'app |
| Input | Swipe IT/EN, tastiera PC con Fn, touchpad, air mouse, gamepad | Modificatori solo per il tasto successivo |
| Comandi | Media, presentazioni, Teams/Zoom, 40 scorciatoie | Liste fisse, non contestuali |
| Stato | Nessuno | Non sa se mic, video o musica sono attivi |
| Navigazione | Menu modalità → schermata | Troppi passaggi |
| Pacchetto | 1,82–1,95 MB per board, slot secondario | Uscire = riavvio |

Pezzi riusabili: Remote Hub (`remote/hub`, ESP-NOW multi-nodo), BLE companion NUS dell'OS, web server
con mirror ORF2, Orbit Companion (Flutter, Windows/Android).

## Interna o esterna

**Raccomandazione: Deck interna** dopo un cancello di misura; finché la misura non c'è, lo sviluppo
avviene nella Remote esterna (stesso codice in `remote/main` e `hid_link`).

| Board | Immagine OS | Slot | Libero |
| --- | --- | --- | --- |
| AMOLED 1.8 | 4,32 MB | 6 MiB | 1,97 MB (31%) |
| AMOLED 1.75 | 4,29 MB | 6 MiB | 2,00 MB (32%) |
| AMOLED 1.75C | 4,25 MB | 6 MiB | 2,04 MB (32%) |
| Knob 1.8 | 4,28 MB | 6 MiB | 2,01 MB (32%) |
| Guition 4848 | 4,26 MB | 6 MiB | 2,03 MB (32%) |
| Watcher W1-A | 4,36 MB | 7 MiB | 2,98 MB (41%) |

Stime della parte propria: UI e logica 150–250 KB, dizionario swipe 150 KB (spostabile su FAT/SD),
HID su GATT 10–30 KB, TinyUSB HID 40–60 KB, font icone 20–40 KB. Da confermare con
`idf.py size-components`. Il vincolo reale è la RAM interna con BLE + Wi-Fi.

Regola di uscita: se l'immagine supera il 90% dello slot o l'heap interno scende sotto soglia, gamepad,
swipe e viewer stream restano in una Remote esterna. Kconfig proposto: `WATCH_APP_DECK` con
`DECK_USB`, `DECK_SWIPE`, `DECK_STREAM`.

## Architettura

```
 Dispositivo                 Trasporti                      PC Windows / Linux
 ┌──────────────┐            ┌──────────────────┐   HID     ┌──────────────────┐
 │ Pagine Deck  │            │ BLE              │──────────▶│ Input di sistema │
 ├──────────────┤  ┌────────▶│ HID + GATT Deck  │           └──────────────────┘
 │ Servizio Deck│──┤         ├──────────────────┤   dati    ┌──────────────────┐
 │ macro,rotella│  ├────────▶│ USB HID + vendor │──────────▶│ Agente Deck      │──▶ OBS, Agent Deck, HA
 ├──────────────┤  └────────▶│ Wi-Fi LAN (bulk) │           └──────────────────┘
 │ Profili/macro│            └──────────────────┘
 └──────────────┘
```

- **BLE:** una connessione, un abbinamento. Accanto a HID-over-GATT un servizio GATT Deck (write + notify).
- **USB:** HID composito con report vendor (usage page 0xFF00), aperto con hidapi. Da fare.
- **Wi-Fi LAN (opzionale):** copertine, mirror, spettro, via WebSocket sul web server esistente.
- Tasti, mouse e media restano report HID: l'agente non simula input (niente antivirus, finestre elevate ok).

### Protocollo

v0 (implementato): un oggetto JSON piatto per scrittura/notifica, massimo 240 byte (`HID_DECK_MAX`).
v1 (previsto): TLV binario con header `DK`, versione, tipo, sequenza, lunghezza; campi ignoti ignorati.

| Tipo | Direzione | Contenuto |
| --- | --- | --- |
| hello | watch → PC | versione, nome del dispositivo |
| ctx | PC → watch | processo in primo piano, titolo finestra |
| media | PC → watch | titolo, artista, in riproduzione |
| sys | PC → watch | CPU %, RAM %, microfono (1 attivo, 0 muto, -1 ignoto) |
| act | watch → PC | azione in allowlist: lock, display_off, mic, taskmgr |
| res | PC → watch | esito e testo breve (toast) |
| blob, profile | entrambe | copertine e profili: v1 |

Servizio `7a3e0000-5c1d-4e8b-9f2a-d3c0de0decc0`, RX `…0001…` (write, cifrato), TX `…0002…` (notify).

### Agente PC

| Capacità | Windows | Linux |
| --- | --- | --- |
| Finestra attiva | GetForegroundWindow + nome processo | xdotool (X11); Wayland: plugin compositor |
| Now Playing | SMTC (winrt) | playerctl / MPRIS |
| Volume per app | Core Audio `IAudioSessionManager2` | PipeWire/PulseAudio |
| Microfono | pycaw `IAudioEndpointVolume` | pactl |
| Notifiche | `UserNotificationListener` | D-Bus `org.freedesktop.Notifications` |
| Monitor | psutil, NVML | psutil, hwmon, NVML |

### Sicurezza

- Le caratteristiche Deck richiedono collegamento cifrato: solo l'host abbinato le usa.
- Azioni solo da allowlist; v1: classi di permesso (stato, azioni, comandi) e conferma sul dispositivo
  per spegnimento/script; codice a 6 cifre per l'abbinamento dell'agente.
- Chiavi Deck in namespace NVS proprio; l'NVS condiviso non si cancella mai.

## Catalogo funzionalità

13 aree, ognuna un'icona dell'hub futuro. Agente: No = solo HID.

| Area | Funzione | Cosa fa | Agente | Fase |
| --- | --- | --- | --- | --- |
| Touchbar | Profili per app | Comandi dedicati all'app in primo piano | Sì | 1 ✅ v0 |
| Touchbar | Contesto dal titolo | Sotto-profili da titolo/URL (YouTube, PR GitHub) | Sì | 2 |
| Touchbar | Rotella contestuale | Timeline, pennello, zoom, volume | Sì | 1 |
| Touchbar | Profilo fisso | Blocca un profilo | No | 1 |
| Scorciatoie | Griglie personalizzate | Pagine stile Stream Deck dalla companion | No | 1 |
| Scorciatoie | Accordi veri | Modificatori tenuti, più tasti | No | 1 |
| Scorciatoie | Launcher | App, cartelle, URL preferiti | Sì | 1 |
| Macro | Sequenze HID | Tasti, testo, attese, click | No | 1 |
| Macro | Azioni agente | Script, HTTP, file, audio | Sì | 2 |
| Macro | Registratore | Registra sul PC, salva come tasto | Sì | 3 |
| Macro | Trigger | Tap, pressione lunga, gesto IMU, orario, cambio app | Misto | 3 |
| Media | Now Playing reale | Titolo, artista, stato | Sì | 1 ✅ v0 |
| Media | Copertina e seek | Copertina 96×96, barra di avanzamento | Sì | 2 |
| Media | Mixer per app | Volume e mute di ogni app | Sì | 2 |
| Media | Uscita audio | Cuffie/casse/HDMI | Sì | 2 |
| Riunioni | Stato reale e mute globale | Mic di sistema reale | Sì | 1 ✅ v0 |
| Riunioni | Prossimo evento | Countdown e Partecipa | Sì | 3 |
| Stream | OBS | Scene, live, registrazione, statistiche (WebSocket 5) | Sì | 2 |
| Stream | Soundboard, marker | Suoni, replay buffer | Sì | 2–3 |
| Sistema | Monitor | CPU, RAM ✅ v0; GPU, temperature, rete | Sì | 1 |
| Sistema | Energia | Blocca ✅, schermo off ✅, sospendi, Wake-on-LAN | Misto | 1 |
| Sistema | Blocco di prossimità | Blocca quando ti allontani | Sì | 3 |
| Finestre | Selettore e layout | Elenco finestre, snap, desktop virtuali | Misto | 2 |
| Appunti | Cronologia e invio testo | Incolla da cronologia, testo in blocco | Sì | 2 |
| Notifiche | Specchio PC | Notifiche del PC sul polso | Sì | 3 |
| Sviluppo | Agent Deck, build/CI | Sessioni Claude Code, esiti build | Sì | 2–3 |
| Input | Tastiere, mouse, gamepad | Già presenti | No | — |
| Input | Dettatura | Voce trascritta sul PC | Sì | 3 |
| Focus | Pomodoro | Timer + Non disturbare sul PC | Sì | 2 |

## Sensori e touch

| Periferica | Board | Uso |
| --- | --- | --- |
| IMU QMI8658 | AMOLED 1.8, 1.75, 1.75C, Watcher | Air mouse (esiste), inclina per scorrere, faccia in giù = mute |
| Encoder | Knob, Watcher | Jog/shuttle, volume, pennello; servizio Deck sempre attivo |
| Aptica DRV2605 | Knob | Scatti per contesto, conferma macro |
| Microfono | 1.75, 1.75C, Knob, Watcher, 4848 | Push-to-talk, dettatura |
| Camera Himax + SSCMA | Watcher | Presenza (blocca/riattiva PC), gesti, QR abbinamento |
| Touch 480×480 | Guition 4848 | Griglia 4×4 e dashboard da scrivania |
| Relè | 4848 (variante) | Luce «ON AIR» in riunione o live |
| PMU / RTC | AMOLED | Modalità scrivania in carica |

Multi-device con Remote Hub: Knob come rotella, AMOLED come touchbar, 4848 come griglia verso un PC.

## Funzioni sperimentali

Specchio dello schermo, ambient light dalla copertina, visualizzatore audio, assistente AI vocale,
lavagna condivisa, puntatore laser IMU, overlay statistiche di gioco, tamagotchi del PC (Pocket Tank),
tap code segreti, wake-up desk (Wake-on-LAN + app del giorno), pagina Home Assistant.

## Navigazione futura (rimandata)

Hub a pagina unica senza scroll: griglia 3×3 sui rotondi, 4×4 sul 4848; centro = touchbar dell'app
attiva; otto celle configurabili + «Altro»; swipe dall'alto torna all'hub; font icone unico.

## Piano

1. **Fase 0** — canale dati e misure. ✅ v0: servizio GATT Deck, agente Python, pagina PC.
   Restano: misura RAM/flash per la versione interna, USB vendor report, protocollo v1 testato su host.
2. **Fase 1** — agente nella Orbit Companion, 15 profili, griglie personalizzate, macro HID, rotella.
3. **Fase 2** — mixer, OBS, finestre, appunti, Agent Deck, Pomodoro, agente Linux Wayland.
4. **Fase 3** — dettatura, presenza Watcher, prossimità, notifiche, Hub multi-device, extra; poi navigazione.

| Rischio | Contromisura |
| --- | --- |
| RAM con BLE + Wi-Fi + HID | Wi-Fi solo su richiesta, buffer in PSRAM, regola di uscita |
| Wayland senza finestra attiva | Plugin per compositor; profilo fisso |
| Windows e servizio Deck sulla periferica HID | Verificato con prototipo v0 (vedi sotto) |
| Agente con privilegi ampi | Allowlist, conferma sul dispositivo |

Punti aperti: nome definitivo (Deck o Remote); agente dentro Orbit Companion o processo in tray;
supporto macOS dell'agente; quali 15 app nei profili iniziali.

## Stato implementazione (Remote 1.2.0)

Verificato su AMOLED 1.8 il 29/10/2026: associazione Bluetooth con Windows 11, agente collegato,
selftest 11/11 (il watch conferma il profilo scelto per VS Code, Chrome, YouTube, Esplora file, Word,
Terminale, Spotify, Teams, Zoom, Discord e Blocco note).

- **Firmware** `components/hid_link/hid_deck.c`: servizio GATT Deck accanto a HID, caratteristiche
  cifrate. Con un host collegato il watch fa advertising lento non connettibile finché l'agente non si
  iscrive (serve su Linux; su Windows l'agente apre direttamente il dispositivo associato).
- **Pagina «PC (Deck)»** `remote/main/remote_deck.c`, prima voce di Remote:
  - senza agente è una guida: Remote su USB (con «Passa a Bluetooth»), PC da associare, agente da avviare;
  - con l'agente: app attiva con icona e colore del profilo, titolo finestra, 6 tasti del profilo,
    brano in riproduzione, media, microfono con stato reale, Blocca, Schermo off, CPU/RAM.
  - Profili: VS Code, Browser, YouTube (dal titolo), File, Office, Terminale, Spotify, Teams, Zoom,
    Discord, Generale. Ctrl diventa Cmd con l'opzione macOS.
- **Agente** `tools/deck_agent/`:
  - Windows: `powershell -ExecutionPolicy Bypass -File install_windows.ps1` installa le dipendenze e
    l'avvio automatico senza finestra (log in `%LOCALAPPDATA%\DeckAgent\agent.log`); `-Remove` lo toglie.
  - Manuale: `pip install -r requirements.txt` e `python deck_agent.py`; `--selftest` verifica i profili.
  - Linux: bleak + xdotool, playerctl, pactl, loginctl (X11; Wayland da fare).
  - Una sola istanza; riconnessione automatica.

Limiti noti: canale dati solo Bluetooth (con USB i tasti funzionano ma senza contesto); azioni
Blocca/Schermo/Microfono provate lato PC, non ancora con un tocco sul watch in questo collaudo;
le altre board sono compilate ma provate solo su AMOLED 1.8.
