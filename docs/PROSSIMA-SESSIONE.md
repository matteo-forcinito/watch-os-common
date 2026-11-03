# Prossima sessione — Git Deck + CI, Telecamera Watcher, Orbit 2.0

Documento di partenza per una sessione nuova (3 novembre 2026). Si legge da solo: contesto,
regole, stato, i tre lavori con il progetto e i criteri per dirli finiti. Prima di iniziare leggere
anche `CLAUDE.md` della radice del workspace e quello del repo della board toccata.

## Obiettivo e livello di qualità

L'utente vuole tutto **bellissimo, moderno, fluido e affidabile (nessun crash)**. In pratica:

- niente funzioni "quasi finite": ogni lavoro si chiude con prova sul dispositivo vero;
- animazioni a frame rate costante (misurare, non stimare), niente scatti né attese percepibili;
- nessun blocco: ogni attesa su rete, Bluetooth, Windows, Himax ha un limite di tempo;
- grafica dai token del tema (`UI_COLOR_*`, `ui_accent()`), layout da `remote_screen_w/h()` o dalla
  risoluzione LVGL, mai dalle misure fisiche della board (gli schermi possono essere ruotati);
- testi in italiano sul dispositivo, codice e commenti in inglese.

## Regole del workspace (da rispettare sempre)

- Rispondere in italiano. Build ESP-IDF solo da PowerShell (`CLAUDE.md` radice ha il comando).
- Dopo gli sviluppi: flash sul dispositivo collegato + push diretto su `main` con riga
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Release/Market solo su richiesta.
- NVS è condivisa tra sistema e app esterne: nessuna app la cancella; non allungare `settings_t`
  (le app esterne già compilate non leggerebbero più le impostazioni): valori nuovi in chiavi a parte.
- Flash: solo gli slot app (`write-flash --after no-reset <slot>`), poi `select_slot_usb.py`.
  Leggere prima l'header degli slot. Sul Watcher aprire la seriale (CH342) **riavvia la scheda**.

## Lezioni recenti (non ripetere)

1. **Mai terminare a forza l'agente Watch Deck** (Stop-Process, taskkill, Task manager): Windows
   lascia il servizio GATT del Deck occupato (AccessDenied, stato 3) finché non si riavvia
   `bthserv` o il PC. Per provare l'agente usare un runner che chiama `deck_agent.shutdown()`
   (esempio: chiude dopo N secondi). L'utente aggiorna con «Esci» dal tray e poi il nuovo exe.
2. **Ogni await su WinRT va limitato con `bounded()`** (in `deck_agent.py`): `asyncio.wait_for` non
   basta, alcune operazioni Bluetooth/media di Windows ignorano l'annullamento e bloccano l'agente
   (sintomo sul watch: «Avvia Watch Deck»). È successo con la scoperta GATT e con la sessione media
   di Edge (Mediaset Infinity).
3. **Testi esterni sempre UTF-8 valido**: un carattere tagliato a metà manda in loop il disegno delle
   label LVGL e il watch si riavvia per «lvgl task stuck». Il watch pulisce in `deck_json_text`,
   l'agente taglia in byte con `clean()`. Vale per ogni nuovo testo che arriva da PC, rete o SD.
4. Dopo un flash dei watch verificare sempre che l'agente si ricolleghi
   (`%LOCALAPPDATA%\DeckAgent\agent.log`). Per diagnosticare un agente fermo: `py-spy dump --pid`.
5. Coredump: le board hanno la partizione `coredump`; si decodifica con
   `python -m esp_coredump --gdb-timeout-sec 20 info_corefile -t raw -c core.bin remote.elf`
   (ELF della build esatta che girava).
6. Voci nuove dell'interfaccia sempre raggiungibili (anche in Opzioni): l'utente non deve cercarle.

## Stato al 3/11/2026

- **Remote 1.4.1** sul Market per 6 board (amoled_watch, amoled175_watch, amoled175c_watch,
  knob_os, watcher_os, guition4848_panel). Pubblicazione: `remote/tools/publish_stable.py`.
  Catalogo Market a **49 083 / 49 152 byte**: per nuove app serve prima liberare spazio
  (vedi discussione: alzare il limite degli script a 128 KB se tutti i device sono ≥ firmware del
  14/10, oppure compattare le voci).
- Deck: sezioni Le mie / App / Avvio / Sistema, indice dal titolo, App aperte, Cambia app;
  protocollo in `docs/DECK.md` (sezione 1.4.0). Agente: `tools/deck_agent/` (`deck_agent.py`,
  `deck_config.py`, `deck_ui.py`, `deck_tray.py`), exe con `build_exe.ps1`.
- Watcher: rotella 2 fronti per scatto e «Rotella: inverti» nel sistema (non ancora rilasciato
  come firmware 1.17.x); Remote 1.4.1 con inversione scorrimento.
- Dispositivi di prova: AMOLED 1.8 (COM3, USB nativa), Guition 4848 (COM7, CH340),
  Watcher (COM6 = ESP32, COM4 = Himax, CH342).

---

## Lavoro 1 — Git Deck e CI Watch dentro Remote

**Idea.** Quando VS Code è in primo piano il Deck mostra una sezione **Git**; in ogni momento una
sezione **CI** con lo stato delle pipeline. Niente app separata: vive nel Deck di Remote (e quindi
anche nella Deck Bar, che usa lo stesso `deck_core`).

**Git (agente).**
- Repo: dal titolo della finestra di VS Code (`<file> - <cartella> - Visual Studio Code`) si ricava
  la cartella; l'agente cerca il percorso tra le cartelle recenti di VS Code
  (`%APPDATA%\Code\User\globalStorage\storage.json`) o lo chiede una volta nella finestra Watch Deck.
- Stato inviato al watch (lista nuova, es. `"git"`): branch, file modificati, commit avanti/indietro.
- Azioni: Pull, Push, Fetch, Commit rapido (messaggio predefinito o dettato), Stash/Pop, cambio branch
  (lista dei branch locali), apri il diff in VS Code. Ogni comando con timeout e risultato in toast.
- Sicurezza: niente `--force`, niente reset; push solo se non ci sono conflitti; conferma sul watch
  per Commit e Push.

**CI.**
- GitHub Actions via API REST (token da `git credential fill`, come `publish_stable.py`), GitLab
  facoltativo. Repo da controllare scelti nella finestra Watch Deck.
- Lista `"ci"`: repo, branch, stato (in corso / ok / fallita), durata. Polling ogni 60 s, più veloce
  mentre una run è in corso.
- Sul watch: sezione CI con semafori; vibrazione + toast quando una run fallisce o finisce; azione
  «Riavvia run» (`gh api` equivalente via REST) con conferma.

**Criteri di fine.** Su AMOLED 1.8 e Guition: sezione Git compare solo con VS Code davanti, Pull/Push
reali su un repo di prova, CI che passa da in corso a ok/fallita con avviso; agente che regge
riavvii del watch e del PC senza bloccarsi; nessun crash con nomi di branch/commit lunghi o accentati.

## Lavoro 2 — Telecamera del Watcher (sorveglianza + camera completa)

**Base esistente.** App Camera interna del Watcher (`SenseCap-Watcher-W1-A`, `components/hardware/camera.c`):
Himax HX6538 + OV5647 via protocollo SSCMA su SPI, JPEG VGA, foto e galleria su SD, clip MJPEG AVI
(~1 fps). Documenti: `docs/VALIDATION-CAMERA.md`, `docs/CAMERA-HOMEASSISTANT.md` nel repo Watcher.

**Sorveglianza (usa il coprocessore AI, facoltativo).**
- Himax con modello di rilevamento persone (SSCMA `INVOKE`): l'ESP32 dorme finché il Himax non segnala
  una persona → foto/clip su SD, notifica al telefono (companion) e/o Home Assistant (MQTT/webhook).
- Senza modello AI (o se non caricato): rilevamento movimento per differenza tra frame JPEG a bassa
  risoluzione sull'ESP32 (più consumo, stessa interfaccia).
- Zone di esclusione, sensibilità, orari, «armato/disarmato» dalla rotella; registro eventi con
  miniature.

**Camera completa.**
- Anteprima a frame rate massimo possibile su SSCMA, scatto, raffica, timer, griglia, flash LED
  (WS2813), risoluzioni disponibili, galleria con zoom, cancellazione sicura.
- Clip più fluide se il Himax lo consente (verificare limiti reali: oggi ~1 fps).

**Compatibilità.** Le altre board non hanno camera: eventualmente solo un visualizzatore degli eventi
del Watcher (via rete). Non promettere funzioni che l'hardware non regge: misurare fps e consumo.

**Criteri di fine.** Sul Watcher: allarme persona reale → foto + notifica entro pochi secondi; 1 ora
armato senza crash né perdita di memoria (heap log); camera manuale con scatto e galleria fluidi.

## Lavoro 3 — Orbit 2.0 (respirazione, meditazione, suoni)

**Oggi.** App esterna `watch-os-common/orbit/` (`main/orbit.c`, ~460 righe): pagine scenica/controlli/
sessione, modalità focus, pausa, respirazione. Il pianeta che respira è bello ma **scatta**.

**Causa del lag (da `orbit.c`).**
- la scala del pianeta cambia una volta al secondo (`phase = elapsed % 10`, gradini di 7/256): il
  movimento è a scatti per costruzione;
- il pianeta ha un'ombra larga (`shadow_width 28`) e `transform_scale`: ogni frame ridisegna un'area
  grande con ombra trasformata, costoso in software;
- un timer da 60 ms riallinea anche le 4 lune con `lv_obj_align` (invalidazioni sparse).

**Correzione.** Animazione continua con `lv_anim` (curva ease in/out sul tempo reale, non sul secondo
intero), pianeta disegnato senza ombra trasformata (alone come cerchio separato a opacità variabile,
o immagine pre-renderizzata in PSRAM), lune su un solo oggetto/canvas; misurare fps e tempo di
render con il log di `lv_port` su AMOLED 1.8, Knob e Watcher. Obiettivo: nessun frame oltre ~33 ms.

**Nuove funzioni.**
- Pattern di respirazione: coerenza 5-5, box 4-4-4-4, 4-7-8, rilassamento lungo; durata e cicli.
- Feedback: vibrazione/suono a ogni cambio di fase (dove c'è lo speaker), voce o tono guida.
- **Meditazioni guidate**: testi brevi a tappe sullo schermo con tono di passaggio; facoltativo
  audio parlato da SD (file WAV/MP3) dove c'è lo speaker.
- **Suoni ambientali** per dormire/lavorare: generati in tempo reale (rumore bianco/rosa/marrone,
  pioggia, onde, vento sintetici: niente file, poca flash) + file da SD facoltativi; timer di
  spegnimento con dissolvenza; volume dalla rotella.
- Statistiche semplici (minuti per giorno) in NVS dell'app.

**Audio.** Oggi `services/sound.h` ha solo click, notifica e sveglia: serve un piccolo mixer/stream
PCM continuo verso il codec (ES8311) senza bloccare l'interfaccia (task dedicato, buffer DMA, nessuna
allocazione nel ciclo). Verificare quali board hanno speaker prima di mostrare le opzioni audio.

**Criteri di fine.** Pianeta fluido su tutte le board di Orbit (misurato), sessioni da 30 min con
suono ambientale senza crash né glitch audio, guide leggibili sui round, consumi annotati.

## Ordine consigliato

1. Orbit 2.0 (la correzione del lag è veloce e visibile; poi audio e guide).
2. Git Deck + CI (riusa agente e Deck, valore immediato per il lavoro quotidiano).
3. Telecamera Watcher (più ricerca sul Himax: modelli SSCMA disponibili, consumi).

Ogni lavoro: progetto breve con l'utente se ci sono scelte di gusto, implementazione, prova sul
dispositivo, revisione del diff, flash + push. Pubblicazione sul Market solo quando l'utente lo chiede.
