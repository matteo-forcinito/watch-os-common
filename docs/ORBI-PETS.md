# Pet di Orbi: animali in pixel art, ambiente e vita insieme

Strategia tecnica e piano di sviluppo per la nuova esperienza dei Children
(firmware 1.10). Completa [ORBI-CHILDREN.md](ORBI-CHILDREN.md) (archivio,
viaggi, API) e [ORBI-GENOME.md](ORBI-GENOME.md) (seed e genoma).

## Obiettivo

La pagina del pet diventa un piccolo mondo, come un Tamagotchi vero:

- il pet è un **animale riconoscibile** (cane, gatto, pesce, camaleonte, geco,
  gallina, capra, coniglio) disegnato in **pixel art procedurale**;
- **cresce davvero** con il livello: proporzioni, dettagli e taglia cambiano
  in continuo (il pulcino diventa gallina, al capretto spuntano le corna);
- vive in un **ambiente** che segue il meteo reale e l'ora: cielo, sole o
  luna, nuvole, pioggia, neve, nebbia, temporale, prato o neve a terra;
- più pet sullo stesso device **si muovono e interagiscono**: si avvicinano,
  si salutano, giocano a rincorrersi, dormono vicini;
- **entrate e uscite** animate per i viaggi (portale), e **visita di Orbi**
  (il papà) quando la companion è collegata;
- **razze scoperte** salvate sul device; **qualità e personalità** che si
  rivelano man mano che si passa tempo insieme.

Vincoli: sistema fluido e affidabile su tutti i firmware (ESP32-S3, 8 MB
PSRAM, schermi da 240 a 480 px, rotondi e rettangolari), nessuna regressione
della RAM interna (il Bluetooth ha bisogno di 40 KiB contigui), formato di
archivio e protocollo dei viaggi invariati.

## Fattibilità e budget

| Risorsa | Uso | Note |
|---|---|---|
| CPU | rasterizzazione 32x32 ≈ 1-2 mila operazioni intere per fotogramma | si ridisegna solo quando cambia il fotogramma (2-8 fps), non a ogni tick |
| Display | sprite da 96-160 px, spostati di pochi pixel | aree invalidate piccole; 15 fps massimo, pausa durante il tocco |
| PSRAM | buffer ARGB8888 per attore: (32·k)² · 4 byte, k = scala | k = 4 → 64 KiB; 3 attori + effetti < 300 KiB su ~7 MB liberi |
| RAM interna | nessuna allocazione nuova | LVGL e buffer in PSRAM; stato statico in `EXT_RAM_BSS_ATTR` |
| Flash (NVS) | blob `orbi_dex` < 512 byte | scritto solo quando si scopre una razza nuova |
| Flash (codice) | ~25-35 KiB | tutto procedurale: nessuna immagine nel firmware |

Conclusione: fattibile senza compromessi sulle risorse. Il rischio vero è la
fluidità: lo si controlla con aggiornamenti solo-se-cambia, timer unico per
la scena, pausa quando la pagina non è visibile o lo schermo è spento.

## Architettura

```
components/orbi/pet_sprite.c     modello puro C, senza LVGL (testabile su PC)
    seed + livello -> pet_look_t (specie, razza, colori, motivo, crescita)
    pet_look_t + posa + fotogramma -> griglia 32x32 di indici di palette
    personalità rivelata (qualità) dal genoma + tempo insieme
components/apps/pet/pet_render.c LVGL: griglia -> lv_canvas in PSRAM, scala k
components/apps/pet/pet_scene.c  ambiente meteo, attori, comportamento, effetti
components/apps/pet/pet_app.c    pagina, HUD, barra azioni, menu, razze
components/apps/pet/pet_store.c  archivio (invariato) + nuove azioni + dex + eventi
```

### Sprite procedurali (`pet_sprite`)

- Griglia 32x32, vista laterale verso destra, piedi sulla riga 30.
- Primitive: ellisse piena con ombreggiatura, rettangolo, linea, triangolo.
- Passata di **contorno**: ogni pixel vuoto vicino (4-connessi) a un pixel
  pieno diventa contorno, colore scuro tinto sul corpo (non nero puro).
- **Palette** di 12 indici: trasparente, contorno, corpo, ombra, ventre,
  macchie, occhio, riflesso, accento (naso/becco), cheek, vetro (bolla del
  pesce, con alfa), accento 2 (cresta/corna/zoccoli).
- **Specie** (8) dal seed con uno stream SplitMix64 dedicato
  (`seed ^ 0x5045545350454331`, "PETSPEC1"): non tocca il genoma esistente.
- **Razza** = specie + variante di colore (5-6 per specie, con nome:
  "Biscotto", "Tigrato", ...) + motivo (pieno, macchie, strisce, calzini,
  pezza). È la chiave salvata nelle razze scoperte.
- **Crescita** `g` in 0..1 da livello (`1 + sqrt(xp/40)`, saturazione al
  livello 15): taglia, testa/corpo (i cuccioli hanno la testa grande),
  lunghezza delle zampe, occhi più grandi nei piccoli, dettagli che compaiono
  (corna, cresta, barba, coda lunga, pinne). Stadi mostrati: Cucciolo (1-4),
  Giovane (5-9), Adulto (10-19), Saggio (20+).
- **Pose**: riposo (respiro), cammino (4 fasi), sonno, mangia, felice
  (salto), triste. Specchiatura orizzontale per la direzione.
- Il camaleonte cambia colore con l'umore; il pesce vive in una bolla
  d'acqua e "rotola" sul prato.

### Rendering (`pet_render`)

- `lv_canvas` con buffer ARGB8888 in PSRAM, lato 32·k (k da larghezza
  schermo); i pixel sono blocchi k×k pieni: niente filtri, niente scaling
  LVGL a ogni fotogramma.
- Cache dell'ultimo (posa, fotogramma, verso, umore): se non cambia non si
  ridisegna.
- Miniature (menu, compagnia nell'angolo) usano lo stesso renderer con k
  piccolo e un solo fotogramma.

### Ambiente (`pet_scene`)

- Cielo: gradiente verticale scelto da codice WMO + giorno/notte
  (`weather_get`; se il meteo manca, l'ora locale).
- Sole/luna, stelle di notte, 2-3 nuvole che scorrono lentamente.
- Particelle con numero fisso (≤ 16): pioggia, neve; lampo per il temporale;
  velo di nebbia.
- Terreno in pixel art (prato, terra, neve), ciuffi d'erba fissi.
- Aggiornamento del meteo all'apertura e ogni 10 minuti.

### Attori e comportamento

- Fino a 3 pet attivi in scena (selezionato + 2 ospiti), più Orbi in visita.
- Macchina a stati per attore: riposo, passeggio, avvicinamento, saluto,
  rincorsa, mangia, gioca, dorme, entra, esce.
- La personalità guida il comportamento: energia → velocità e frequenza dei
  giri, curiosità → esplorazione, socievolezza → avvicinarsi agli altri,
  giocosità → rincorse, appetito → festa per il cibo.
- Due pet che si salutano guadagnano un po' di gioia (azione `meet`, con
  attesa di 10 minuti per coppia, solo a pagina aperta).
- **Entrata**: quando un viaggio arriva (`arrive`) si apre un portale sul
  bordo, il pet entra, gli altri lo raggiungono e lo salutano. Se la pagina è
  chiusa, l'animazione parte alla prossima apertura.
- **Uscita**: `reserve`/`depart` → il pet saluta, cammina nel portale e sparisce.
- **Orbi**: se Orbit ha interrogato il device negli ultimi 30 s, Orbi scende
  dall'alto e fluttua sopra il prato; i children corrono sotto di lui e
  fanno festa (cuori). Se ne va quando la companion si scollega.

### Interazione

- Tocco sul pet: coccola (cuori). Tocco su un altro pet: lo seleziona.
- Barra azioni: Cibo, Gioco, Bagno, Nanna, Altro.
- Altro: Snack, Allenamento, Personalità, Razze scoperte, cambio pet,
  "Piccola compagnia", aiuto per i viaggi.
- Nuove azioni nell'archivio (stessi campi, nessuna migrazione):
  - `wash`: gioia +8, XP +4;
  - `snack`: sazietà +12, gioia +10, XP +2;
  - `train`: energia -15, XP +18 (serve energia ≥ 20);
  - `meet`: gioia +4, XP +2.

### Personalità e qualità

- Dal genoma: curiosità, energia, socievolezza, appetito, giocosità
  (20-100). Ogni asse produce una qualità: alta, bassa o "equilibrato".
- Ordine di scoperta deterministico dal seed. Qualità rivelate:
  1 al livello 2, poi ai livelli 4, 7, 11 e dopo 3 giorni insieme.
- Le non ancora scoperte compaiono come "???".

### Razze scoperte

- Blob NVS `orbi_dex` (namespace `orbi_children`), versione 1, fino a 64
  voci `{specie, variante, motivo, data}`.
- Si registra una razza quando un pet nasce o arriva sul device, e quando si
  apre la sua pagina.
- Pagina dedicata: griglia di miniature, quelle mancanti come sagome.

### API (compatibile)

`GET /api/orbi` aggiunge per ogni pet: `species`, `breed`, `level`,
`growth`, `stage_name`, `traits` (solo rivelate), e `dex` (razze scoperte).
Le companion fino alla 0.5.1 ignorano i campi nuovi. Le nuove azioni passano
dallo stesso `POST /api/orbi`.

## Affidabilità

- Un solo `lv_timer` per la scena, cancellato in `destroy`; nessun task nuovo.
- Il tick non lavora se la pagina non è visibile, lo schermo è spento, o un
  dito è sullo schermo.
- Ogni allocazione è in PSRAM e controllata; se fallisce l'attore non appare
  e la pagina resta usabile.
- Nessuna scrittura in flash periodica: solo azioni dell'utente, `meet` con
  cooldown, razze nuove.
- Il modello (`pet_sprite`) è puro C e ha test sul PC: determinismo, limiti
  della griglia, tutte le specie/pose/crescite, anteprima PNG.

## Piano

1. **1.10.0** (questo lavoro): sprite procedurali, crescita, ambiente meteo,
   pagina nuova, più pet in scena, entrata/uscita, visita di Orbi, nuove
   azioni, personalità, razze scoperte, miniature nuove.
2. **1.11 - Passatempo**
   - **Viaggi ESP-NOW** device-device senza companion né Wi-Fi: stesso
     protocollo a passi (`reserve` → `receive` → `depart` → `arrive` →
     `forget`) incapsulato in frame ESP-NOW cifrati (PMK/LMK derivati
     dall'accoppiamento Orbit), ritrasmissione con ack e timeout; la
     scoperta dei vicini avviene sul canale del Wi-Fi corrente.
   - **Ritorno a casa**: azione "Torna a casa" sul device ospite; il pet
     conserva `origin` e riparte verso il device nativo (ESP-NOW se vicino,
     altrimenti in coda finché Orbit non lo vede).
   - **Visite**: un pet può andare "in visita" per un tempo limitato e
     tornare da solo con un ricordo (oggetto o razza scoperta).
   - **Minigiochi insieme** sulla scena: acchiapparella (tocco), salto con
     knob/IMU, memory dei versi sui device con speaker; due device vicini
     giocano in coppia via ESP-NOW (punteggi e premi in XP).
   - Oggetti nell'ambiente (cuccia, ciotola, giochi) e ricordi dei viaggi.
3. **Companion 0.6**: porting Dart di `pet_sprite` con vettori di test
   (hash delle griglie) e Children con gli animali nuovi.
4. Dopo: oggetti nell'ambiente (cuccia, ciotola, giochi), stagioni, suoni
   sui device con speaker, minigiochi con knob/IMU.

## Verifiche

- `python tools/pet_sprite_preview.py`: compila il modello con MSVC, controlla
  il determinismo e scrive `build-preview/pet_sprites.png` con tutte le
  specie, varianti, crescite e pose.
- `python tools/test_pet_store.py`: archivio, viaggi e nuove azioni.
- Su device: log `pet:` con il tempo di rendering e la RAM all'apertura.
