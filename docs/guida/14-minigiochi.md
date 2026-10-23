# Il tuo primo minigioco

Non serve conoscere tutto il firmware per cominciare. Parti da **Tocca la
stella**, un gioco vero già incluso: tocchi una stella, lei cambia posizione,
guadagni punti. Dopo 20 secondi la partita finisce. Anche il Knob funziona:
giri per scegliere la corsia del pet e clicchi quando la stella è in quella
corsia.

La base comune si occupa di schermo, pet, comandi, tempo, pausa, memoria,
uscita, pulsante Riprova e richiesta del premio. Nel file del gioco scrivi
le regole e cosa disegnare.

## 1. Prova prima il gioco già pronto

Sul dispositivo apri **Pet → menu → Minigiochi → Tocca la stella**. Nel menu
può essere necessario scorrere. Il pet deve essere sveglio: usa la luna nella
pagina del pet, se serve.

Il codice completo è in
[tap_star.c](../../components/apps/minigames/games/tap_star.c). Aprilo in VS Code.
Questa è la tua piccola palestra: per iniziare non devi leggere il motore.

## 2. Crea la tua copia con un comando

Apri PowerShell nella cartella `watch-os-common`:

```powershell
python tools/new_minigame.py palloncini --title "Acchiappa i palloncini"
```

Il comando crea `components/apps/minigames/games/palloncini.c` e aggiunge il
gioco all'elenco e alla compilazione. Comparirà nello stesso menu dopo aver
compilato e flashato il firmware. Per ora sarà una copia di Tocca la stella:
il nome cambia, le regole le modifichi tu.

Il nome `palloncini` identifica il gioco nel codice: usa lettere minuscole,
numeri e `_`, da 2 a 24 caratteri, iniziando con una lettera. Il titolo è ciò
che si legge sul display. Un nome già esistente viene rifiutato senza
sovrascrivere il gioco. Il comando non compila, non flasha e non pubblica.

## 3. Il primo cambiamento: quattro numeri

All'inizio della tua copia trovi:

```c
#define TAP_DURATION_MS 20000
#define TAP_HIT_SCORE 5
#define TAP_HIT_RADIUS 130
#define TAP_WIN_HITS 10
```

`#define` dà un nome a un valore. Il programma sostituisce quel nome con il
numero indicato. Puoi iniziare cambiando solo questi:

| Nome | Che cosa significa | Primo esperimento |
|---|---|---|
| `TAP_DURATION_MS` | Durata, in millisecondi: 1000 = un secondo | `30000` per 30 secondi |
| `TAP_HIT_SCORE` | Punti per una stella | `10` per guadagnarne dieci |
| `TAP_HIT_RADIUS` | Quanto puoi sbagliare la posizione del tocco | `180` per renderlo più facile |
| `TAP_WIN_HITS` | Stelle necessarie per il messaggio di vittoria | `5` per vincere prima |

Resta fra 1 e 60 secondi; usa valori piccoli e positivi per punti e raggio.
La base termina comunque ogni partita entro 60 secondi di gioco attivo e
limita il risultato finale a 0–100. Non corregge una regola impossibile:
se chiedi mille stelle in un secondo, il gioco funzionerà ma sarà impossibile.

## 4. Il minimo di C per leggere il file

Una variabile è una scatola con un nome. `int hits;` è una scatola per un
numero intero. `bool redraw;` contiene `true` oppure `false`: sì o no.

Una `struct` raggruppa le scatole della partita:

```c
typedef struct
{
    uint32_t random;
    int elapsed, hits, target_x, target_y, lane;
    bool redraw;
} tap_state_t;
```

Qui conserviamo il tempo passato, le stelle prese, dove disegnare la stella
e la corsia del pet. La base crea questo stato e lo azzera a ogni Riprova.
Non devi usare `malloc`, `free` o creare grandi variabili globali.

Nelle funzioni trovi spesso:

```c
tap_state_t *s = state;
s->hits++;
```

La prima riga dice «lo stato ricevuto è lo stato del mio gioco». L'asterisco
indica un riferimento a quei dati, non una copia. `s->hits` significa «il
numero di stelle dentro quello stato». `++` aggiunge uno. Mantieni la prima
riga come nell'esempio finché non vuoi approfondire i puntatori.

Le graffe `{ ... }` delimitano un blocco; `;` chiude un'istruzione. Con
`if (condizione)` esegui un blocco solo se la condizione è vera. `==`
confronta due valori, `=` assegna un valore: sono cose diverse.

## 5. Le funzioni che compongono il gioco

| Funzione nell'esempio | Quando viene chiamata | Che cosa metterci |
|---|---|---|
| `start` | All'avvio e a ogni Riprova | Posizioni iniziali e contatori |
| `step` | A passi regolari di 16 millisecondi | Movimento, tempo, collisioni e regole |
| `input` | Quando arriva un comando | Che cosa fa un tocco o una rotazione |
| `draw` | Quando il display può aggiornarsi | Le figure da disegnare |
| `hud` | Circa cinque volte al secondo | La riga con punti, vite o tempo |
| `result` | Dopo ogni passo delle regole | Se la partita è finita e il punteggio |

`step` e `draw` sono separati apposta. Le regole avanzano con il tempo;
un display più lento non deve cambiare la velocità del pet o la durata.
Non far avanzare il tempo dentro `draw`.

Per esempio, questo aggiunge il tempo passato:

```c
static void step(void *state, int milliseconds)
{
    tap_state_t *s = state;
    s->elapsed += milliseconds;
}
```

Per un oggetto che scende di 100 unità al secondo puoi aggiungere un campo
`int y;` allo stato e usare `s->y += 100 * milliseconds / 1000;`. Con numeri
interi gli spostamenti molto piccoli possono arrotondarsi a zero: per
movimenti fini conserva la posizione moltiplicata, per esempio per 16, e
dividila quando disegni. Il gioco delle colline usa questa tecnica; non ti
serve per il primo gioco a tocchi.

## 6. Disegno: da 0 a 1000, su tutti i device

Non scrivere coordinate specifiche del Knob o dell'AMOLED. L'area di gioco
va da 0 a 1000: `(0, 0)` è in alto a sinistra, `(500, 500)` è il centro,
`(1000, 1000)` è in basso a destra. Titolo e pulsanti sono fuori da quest'area.

```c
mg_clear(canvas, 0x17334D);                   // Sfondo blu
mg_circle(canvas, 500, 350, 100, 0xFFD45C);  // Cerchio giallo
mg_rect(canvas, 100, 700, 300, 80, 0x46BC78); // Rettangolo verde
mg_star(canvas, 750, 350, 70, 0xFFFFFF);      // Stella bianca
mg_pet(canvas, 500, 950);                    // Il pet, piedi a y=950
```

Un colore `0xRRGGBB` è come un colore HTML: `0xFF0000` rosso, `0x00FF00`
verde, `0x0000FF` blu. Non devi convertirlo nel formato del display.

`mg_rect` riceve posizione, larghezza e altezza. `mg_circle` e `mg_star`
ricevono centro e raggio; il raggio è proporzionato al lato più corto
dell'area, per mantenere il cerchio tondo. `mg_pet` usa il pet selezionato,
con razza e colori propri. Le figure che escono dall'area vengono tagliate.

Il disegno di Tocca la stella parte con:

```c
if (!s->redraw) return;
s->redraw = false;
```

Significa «se non è cambiato niente, non ridisegnare». Quando cambi la
posizione di una figura, imposta `s->redraw = true;`. Per una scena animata
puoi disegnare a ogni aggiornamento, ma evita di cancellare continuamente
tutto lo sfondo se cambiano solo pochi oggetti. I giochi già inclusi
mostrano anche il disegno avanzato delle sole zone cambiate; non serve
copiarlo per il primo esperimento.

## 7. Comandi: touch e Knob senza conoscere i chip

Dentro `input` leggi `event->kind`:

| Comando | Significato |
|---|---|
| `MG_PRESS` | Dito appena appoggiato; `x` e `y` sono da 0 a 1000 |
| `MG_RELEASE` | Dito alzato o uscito dall'area; interrompi la pressione |
| `MG_TURN` | Rotella girata; `direction` è −1 oppure +1 |
| `MG_CLICK` | Clic del pulsante del Knob |

La base normalizza i comandi. Il gioco decide il loro significato: nelle
colline la pressione aumenta il peso, nelle stelle un tocco cambia corsia,
nell'esempio cerca di colpire il bersaglio. Prevedi anche il Knob se vuoi
che il gioco sia comodo sui device con rotella.

Quando lo schermo si spegne, arriva un `MG_RELEASE` con
`event->cancelled == true`: annulla la pressione. Se il tuo gioco lancia
qualcosa quando lasci il dito, controlla `!event->cancelled` prima del
lancio. Così mettere in pausa non diventa una mossa involontaria.

Per un esempio più avanzato guarda
[Salto quantico](../../components/apps/minigames/games/quantum.c): orbite,
gravità, traiettoria prevista e particelle usano questa stessa base. Non
crea timer, non chiama LVGL e non alloca memoria durante la partita. Lo
sfondo viene preparato solo quando cambia pianeta e poi ripristinato nelle
zone attraversate dal pet.

Per una scelta casuale usa lo stato del gioco:

```c
int corsia = (int)(mg_random(&s->random) % 3);
```

Produce 0, 1 o 2. Con lo stesso valore iniziale ottieni la stessa sequenza:
è utile per riprodurre un problema nei test.

## 8. Fine partita, premi e Riprova

`result` scrive il punteggio e restituisce `true` solo quando hai finito:

```c
out->score = s->hits * TAP_HIT_SCORE;
out->won = s->hits >= TAP_WIN_HITS;
return s->elapsed >= TAP_DURATION_MS;
```

La base limita il punteggio a 0–100, mostra il risultato e chiede il premio
una sola volta. La pagina pet salva XP e gioia nel worker di sistema,
senza scrivere in flash durante il movimento. «Premio in salvataggio»
indica che la richiesta è stata accodata; un errore viene segnalato.
Esci annulla una partita non conclusa senza premio; Riprova azzera lo stato
e ricomincia senza creare un nuovo schermo o un nuovo framebuffer.

Non chiamare `pet_command`, NVS o funzioni di rete dal file del gioco.
Non salvare puntatori a pulsanti o schermate: li gestisce la base.

## 9. Come capire se hai rotto qualcosa

Dalla cartella `watch-os-common`:

```powershell
python tools/test_minigames.py
python tools/test_pet_game.py
python tools/test_pet_store.py
```

Questi test usano il compilatore C di Visual Studio sul PC Windows:
**non** richiedono un dispositivo e **non** flashano. Se Visual Studio C++
non è installato, usa la preparazione descritta nelle guide
[ambiente](02-ambiente-build-flash.md) e [test](07-test.md).

Il primo comando compila anche tutti i giochi nella cartella `games`,
prova i disegni nelle diverse dimensioni dei display, la pausa, Riprova,
il premio unico e il generatore. È una rete di protezione, non una prova
che il tuo nuovo gioco sia divertente o che ogni sua regola sia corretta.

Poi compila e flasha **prima un dispositivo di prova**, seguendo
[Ambiente, build e flash](02-ambiente-build-flash.md). Prova anche 5–10
Riprova ed Esci, lo schermo spento/riacceso e i comandi della rotella.
Per distribuire a tutti, segui [Release](09-release-firmware.md): creare
un file sul PC non aggiorna i device da solo.

| Che cosa vedi | Che cosa controllare |
|---|---|
| Errore di compilazione con file e riga | Apri quella riga; spesso manca `;`, una graffa o un nome è scritto diversamente |
| Il nuovo gioco non compare | Hai compilato e flashato dopo il comando? Stai usando il `watch-os-common` modificato? |
| Schermo immobile | Hai impostato `redraw = true` quando cambi il disegno? Hai un ciclo che non finisce? |
| Movimento a scatti | Evita `mg_clear` a ogni frame per grandi scene; leggi i log `minigame: ... perf` e confronta frame e tempo |
| Il dispositivo si riavvia | Salva il log seriale: cerca `panic`, `assert` o `PREVIOUS RUN CRASHED`; vedi la [diagnostica](08-console-e-diagnostica.md) |
| Memoria insufficiente | Usa uno stato piccolo; non creare immagini, task o oggetti nel file del gioco |

## 10. Che cosa protegge la base, e che cosa resta a te

La base mette lo stato della partita e le immagini in PSRAM, tiene limitato
il recupero dopo un rallentamento, rilascia i comandi quando va in pausa,
taglia i disegni ai bordi e libera timer, callback e immagini alla chiusura.
Lo stato può occupare al massimo 64 KB. I device supportati usano la stessa
base; non devi conoscere pin, chip del touch o formato dei pixel.

Il C consente comunque di scrivere fuori da un array o usare un riferimento
non valido. La base non può rendere sicuro qualsiasi codice C: per iniziare
usa i campi dello stato e le funzioni di disegno dell'esempio. Evita indici
inventati negli array, `while` senza fine e chiamate a servizi bloccanti.
Se vuoi qualcosa che l'esempio non offre, aggiungi una funzione alla base
una volta, invece di copiarne una variante in ogni gioco.

Per orientarti: [motore](../../components/apps/minigames/minigame.h),
[disegno](../../components/apps/minigames/minigame_canvas.h),
[schermo condiviso](../../components/apps/minigames/minigame_screen.c),
[elenco dei giochi](../../components/apps/pet/pet_game_catalog.c).
