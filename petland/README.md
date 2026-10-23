# Petland — due avventure nello stesso RPG esterno

Petland è un firmware esterno: si avvia dal launcher e torna al sistema
tramite l'SDK delle app. Il suo codice e il mondo non occupano spazio nel
firmware principale. Riutilizza il motore dei minigiochi per comandi,
pausa, memoria e salvataggi.

Scegli dalla schermata iniziale **Cronache del Faro**, la campagna dei
quattro sigilli, oppure **Legami selvatici**, il viaggio che comincia
scegliendo un Pokémon. Hanno salvataggi indipendenti e condividono
esplorazione, combattimenti e progressione. Il mondo deterministico è di **96 × 96
caselle**, quattro biomi, nebbia di esplorazione, minimappa, quattro
villaggi con empori, quattro santuari e una torre. Ci sono combattimenti
a turni, livelli, armi, armature, pozioni e dodici reliquie da raccogliere.
Cronache conserva dodici creature originali. Legami selvatici usa dodici
famiglie di Pokémon: **31 specie contando le evoluzioni**, normali e shiny,
con Charmander, Squirtle e Bulbasaur come starter, oltre a Pikachu e Raichu.
La squadra ha tre compagni con vita individuale. Le condizioni evolutive
restano quelle di questo RPG (livelli 5/12 e sigilli); le linee con due
stadi si fermano alla loro vera evoluzione finale.
Il protagonista è un pet generato per questa app; non modifica né sposta
i pet del sistema. Non è ancora un RPG con dialoghi ramificati o decine
di missioni: la missione del primo capitolo è riaccendere il faro.

## Come giocare

- **Touch:** trascina verso una direzione per camminare. Tocca il centro
  per interagire con un luogo vicino. Fuori dai luoghi speciali, tocchi
  ripetuti al centro mostrano obiettivo ed equipaggiamento.
- **Knob:** gira per scegliere est, sud, ovest, nord o azione centrale;
  il punto chiaro indica la scelta. Clicca per muoverti o interagire.
- **Combattimento:** sei azioni da sinistra a destra: colpo, magia,
  scudo, cura, sfera, cambio compagno. Due parate caricano un colpo triplo;
  parare recupera energia per la magia. Il testo annuncia la prossima
  mossa nemica: interrompi la carica con una magia, para il colpo pesante.
  Cambiare compagno consuma il turno. Tocca il bordo inferiore per fuggire;
  i guardiani bloccano la fuga. Puoi pensare senza limiti di tempo.
- **Cattura:** indebolisci una creatura e usa una sfera; le varianti rare
  sono più difficili. I guardiani non sono catturabili. Le creature si
  allenano combattendo: la prima evoluzione richiede livello 5 e un sigillo,
  la seconda livello 12 e tutti i sigilli (oppure un capitolo successivo).
- **Emporio:** arma, difesa, kit di pozione e sfere, campo, uscita.
  Il villaggio cura tutta la collezione. Nel campo cambi aspetto, ruolo,
  squadra e arma equipaggiata. I quattro ruoli favoriscono danno, difesa,
  energia/cure oppure esplorazione/catture. Gli slot della squadra scorrono
  le creature catturate senza duplicare un compagno.
- **Drop:** dopo una vittoria scegli quale dei tre slot occupare oppure
  vendi l'oggetto. Potenza, elemento e rarità contano: conservare armi
  diverse permette di prepararsi per guardiani diversi. Il ciclo dei
  vantaggi di Cronache è Fuoco → Erba → Roccia → Acqua → Fuoco.
  In Legami sono adattati ai cinque tipi presenti: ad esempio Acqua batte
  Fuoco/Roccia, Erba batte Acqua/Roccia, Elettro batte Acqua e non colpisce
  le famiglie Roccia/Terra incluse. I tipi secondari e tutte le mosse dei
  giochi originali non sono simulati. Il tipo conta anche quando subisci danni.
  Il titolo mostra il nuovo oggetto, la riga sotto quello dello slot scelto:
  `P` è potenza, `R` rarità. Tieni il dito su uno slot per confrontare,
  trascina per cambiarlo e rilascia per confermare. Sul Knob gira e clicca.
- **Esplorazione:** cerca le rovine fuori dai sentieri per sfere e oggetti;
  alcune nascondono imboscate. Ogni rovina si apre una volta per capitolo.
- **Uscita:** Salva ed esci. Il worker completa il salvataggio prima di
  riavviare il sistema. Anche il tasto Indietro richiede il salvataggio.

Parti dal villaggio a `12,12`. I santuari sono Aurora `38,14`, Marea
`70,40`, Radice `38,70`, Ombra `14,40`. I sentieri collegano i luoghi:
puoi raggiungerli senza una barca. Il primo sigillo sblocca la navigazione
sull'acqua. Dopo i quattro sigilli, raggiungi la torre a `46,46`.
Vincerla apre un nuovo capitolo con un nuovo mondo e nemici più forti;
equipaggiamento e collezione restano. Una sconfitta riporta al villaggio
con una piccola perdita d'oro, mantenendo i progressi.

Il salvataggio include posizione, esplorazione e combattimento in corso.
È locale al dispositivo e indipendente dai tre minigiochi persistenti.
Usa due file alternati in `game-saves` sulla memoria interna o SD; ogni
copia viene verificata prima del caricamento. Senza memoria per file si
usa il formato compresso NVS. I vecchi progressi NVS vengono importati.
Gli allarmi del sistema restano gestiti dall'appkit; il boot dell'SDK
prepara il ritorno al launcher anche in caso di riavvio inatteso.

## Compilare e installare

Apri PowerShell con l'ambiente ESP-IDF 6.1 e, dalla cartella
`watch-os-common`, esegui per il modello desiderato:

```powershell
python tools/build_petland.py --board ../ESP32-S3-Touch-AMOLED-1.75C --output ../.release-work/petland-amoled175c
```

Puoi passare le cartelle degli altri firmware supportati. Il comando crea
un progetto isolato con l'hardware del modello e produce
`sdcard/apps/petland/Petland.bin` e `manifest.json`. Copia la cartella
`petland` in `/apps/` della scheda SD e aprila dal launcher. Il firmware
esterno deve sempre essere quello compilato per quel modello.

Non flashare `Petland.bin` sopra lo slot del sistema per installarlo:
il launcher usa l'altro slot OTA e mantiene la via di ritorno. Non cambiare
le partizioni per provare il gioco. Il comando di build non flasha.

## Grafica Pokémon opzionale

Gli sprite seguono l'ispirazione di [TamaPoke](https://github.com/socquique/TamaPoke)
e provengono da [PMD SpriteCollab](https://github.com/PMDCollab/SpriteCollab).
I crediti dei singoli autori vengono conservati in `CREDITS.json` accanto
al pacchetto generato. Gli sprite seguono i termini non commerciali del
progetto sorgente; Pokémon appartiene ai rispettivi titolari. Il codice
di questo motore e il pacchetto grafico sono separati.

Per preparare la stessa grafica delle prove USB:

```powershell
python -m pip install Pillow
python tools/prepare_pokemon.py --output ../.release-work/pokemon-assets
python tools/build_petland.py --board ../ESP32-S3-Touch-AMOLED-1.75C --output ../.release-work/petland-amoled175c --pokemon-assets ../.release-work/pokemon-assets
```

Il convertitore scarica una revisione fissata, estrae fino a quattro frame
Idle per specie e li codifica in palette RGB565, a 40×40 pixel con
trasparenza. I circa 350 KB di pixel stanno nel firmware esterno:
non richiedono una SD sull'AMOLED 1.75C. `preview.png` mostra il risultato.
I file scaricati e generati restano nella cartella di output, fuori dal
repository. Senza il pacchetto, il codice usa il disegno procedurale di base.

## Modificare il gioco

La ricetta del progetto è: un mondo bello da esplorare, crescita e
personalizzazione, scoperte, bottino e difficoltà che premi la strategia.
La vastità deve offrire decisioni diverse: elementi, ruoli, rischi delle
rovine e scelta di quali armi conservare. Aumentare soltanto i punti vita
dei nemici non crea profondità. Sui nostri schermi piccoli preferiamo
regole leggibili, indizi utili e tempo per ragionare.

Le regole sono in [world.c](main/world.c); avvio e ritorno al sistema in
[main.c](main/main.c). Lo stato salvabile contiene solo interi e la mappa
delle caselle esplorate. Il mondo viene generato dalla posizione e dal
seme: non serve tenere migliaia di immagini in memoria. Il disegno cambia
quando avviene una mossa, non ridisegna il mondo a ogni timer.

`python tools/test_minigames.py` verifica anche itinerari reali fino alla
torre, combattimenti, ripresa durante una battaglia e gestione dei guasti
del salvataggio. Le prove hardware restano necessarie prima del rilascio.
