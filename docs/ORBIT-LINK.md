# Orbit Link

API che un dispositivo offre alla companion Orbit perché più dispositivi si comportino come un unico sistema. Il dispositivo resta semplice (disegna una scena, suona o cattura audio, segnala gli input); stato, tempi e instradamento vivono in Orbit. Codice: `components/apps/link/` (link.c, link_scene.c). Tutte le rotte passano da `web_route()`: serve il token di abbinamento.

## Rotte

| Rotta | Cosa fa |
|---|---|
| `GET /api/link` | versione, `features` (scene, events, dial, timer, speaker, mic, voice, knob), schermo, stato audio |
| `POST /api/link/scene` | `{scene, color, angle, phase, text, name, mood, action, level, value, hold_ms}` disegnata sopra ogni app |
| `GET /api/link/events?since=N` | `{seq, events:[{seq, type, value}]}`: tocchi sulle scene, `dial`, `dial_done`, `orbi` (pet/play), `tap` |
| `POST /api/link/speaker` | PCM 16 kHz mono s16le, al massimo 32 KB per richiesta; anello di 2 s in PSRAM; 409 se allarme o microfono attivi, 503 se il buffer è pieno |
| `POST /api/link/mic` | `{on}`: cattura in un anello di 4 s; si ferma da sola dopo 4 s senza letture. Sul dispositivo compare "Orbit ascolta" |
| `GET /api/link/mic?since=P` | `"OMIC"` + u32 posizione finale + campioni dopo P (al massimo 1 s) |
| `GET/POST /api/timer` | conto alla rovescia e cronometro, app aperta o chiusa: `{action: start, seconds}`, `stop`, `watch`, `watch_reset` |
| `GET/POST /api/voice` | memo vocali: stato, nomi, file in `/memos`; `{action: record, stop, play, delete, index}` |

Scene: `pulse`, `aura`, `beam` (phase out/in, angle in gradi: 0 destra, 90 giù), `text` (un tocco chiude), `light` (torcia, tiene lo schermo acceso), `orbi` (action enter/exit/idle/eat/play/love/sleep, mood happy/calm/sad/hungry/tired/sleep), `dial` (modale: manopola e dito girano l'anello, gli scatti vanno a Orbit; `lv_port_set_knob_hook()`), `clear`. Una scena per volta; quelle non modali lasciano passare i tocchi all'app sotto.

## App esterne

`POST /api/apps/open {ext}` per un'app con dashboard appkit (Pieno, Giornata, Copertura, News, Bilancio) risponde anche con `handoff: {header, token}`: il token NVS `appkit/<nome>` che l'app userà, creato se manca. Orbit lo usa sullo stesso indirizzo mentre l'app gira. `POST /api/kit/exit` (appkit_web.c) torna a Watch OS dopo la risposta. Le app con server proprio (Sentinel completa, Vault, Casa, Deck) non ricevono handoff: Vault per scelta di sicurezza.

## Memoria e consumi

Nessun task o buffer a riposo. Altoparlante: anello 64 KB PSRAM e task da 3 KB solo durante la riproduzione, chiusi dopo 1,5 s di silenzio. Microfono: anello 128 KB PSRAM e task da 3 KB solo durante la cattura. Coda eventi statica di 32 voci (circa 1 KB). Il webserver passa da 80 a 104 handler. Microfono e altoparlante sono esclusivi sullo stesso dispositivo (codec condiviso); l'interfono passa tra dispositivi diversi tramite Orbit.

## Collaudo

`python tools/orbit_link_check.py http://<ip> <codice o token>` esegue ogni rotta, suona un breve motivo e cattura un secondo di microfono.
