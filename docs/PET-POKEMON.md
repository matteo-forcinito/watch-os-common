# Pet Pokémon — attività avviata il 25 ottobre 2026

## Direzione della pagina

Riferimento richiesto: [socquique/TamaPoke](https://github.com/socquique/TamaPoke).
Il progetto mostra un compagno animato in un ambiente che segue orario e bioma,
con cure, allenamento, statistiche e collezione. Riprendiamo questa impostazione
visiva nella pagina del child esistente. Non importiamo il firmware Arduino.

La pagina proposta mantiene il nome del child e il livello in alto, tre
indicatori leggibili per sazietà, energia e felicità, il Pokémon al centro
con il terreno allineato ai piedi e un fondale che segue giorno e meteo.
In basso: cibo, gioco, bagno, sonno e scheda. La scheda offre Aspetto,
personalità, allenamento, collezione e viaggi. Sul Knob gli stessi comandi
seguono l'ordine di focus; i display tondi mantengono i controlli entro
il cerchio utilizzabile. Fame ed energia continuano a essere quelle del child.

Il selettore Aspetto mostra le specie disponibili nel pacchetto,
la variante normale/shiny e il comando per tornare all'aspetto originale.
Le forme evolute sono aspetti espliciti: non cambiano automaticamente
al livello 5/12 dell'RPG. Il pet conserva nome, seed, XP, provenienza e cure.
L'aspetto va registrato separatamente e indicizzato per seed, senza cambiare
il blob NVS dei child. La scelta si applica al pet di origine presente a casa.

## Integrazione disponibile

- Esportatore dei dati grafici già generati per Petland, senza altri download.
- Pacchetto file autonomo: 31 specie, 62 varianti, 360.398 byte sul roster attuale.
- Lettore C portabile registrato nel componente `orbi`, indipendente da LVGL.
- Validazione di versione, CRC32, ordine delle identità, dimensioni e offset.
- Lettura di un frame per ID nazionale e shiny; normalizzazione da 40×40
  a 32×32 ARGB8888, pronta per le dimensioni dei canvas esistenti.
- Nessuna sprite aggiunta ai sorgenti Git.
  Il formato file rimane disponibile per future distribuzioni su storage.
- Selettore nella scheda del pet: anteprima, specie precedente/successiva,
  variante shiny, applicazione e ripristino originale. Tutti i comandi sono
  raggiungibili con il focus del Knob.
- Persistenza in `pet_looks/v1`, separata da `orbi_children/state`, con scrittura
  sul worker e aggiornamento del disegno soltanto dopo un commit riuscito.
- Scena con compagno più grande, pannello dei bisogni e comandi di cura con testo.
- Aspetto coerente fra scena, miniature, compagnia negli angoli e minigiochi locali.

Per i dispositivi senza SD la build può includere direttamente i dati generati
di Petland in flash tramite `WATCH_PET_POKEMON_ASSETS`. I pixel sono costanti
letti dalla flash mappata: nessun file, worker di decodifica o cache di frame
durante l'animazione. La palette indicizzata usa fino a 256 posizioni, con
indice zero trasparente. Il binario cresce di circa 360 KB; i dati non vengono
copiati in RAM. Il formato PKP1 resta una via alternativa per il futuro, non
un file necessario sui dispositivi che usano la build con asset inclusi.

La scelta cambia solo il vestito del child e si applica a un pet di origine
presente sul dispositivo, senza viaggi in corso. Sui destinatari di un viaggio
e nel companion telefono resta il disegno originale: non è stato cambiato
il protocollo radio. Al ritorno il proprietario ritrova il proprio aspetto.
Questa differenza è indicata anche nella scheda del pet.

## Generazione e verifica

```powershell
python tools/export_pet_pokemon.py --source ../.release-work/pokemon-assets --output ../.release-work/pet-pokemon-pack
python tools/test_pokemon_pack.py ../.release-work/pet-pokemon-pack/pokemon.pkp
```

L'origine deve essere un pacchetto generato da `tools/prepare_pokemon.py`.
`pokemon.pkp` e `CREDITS.json` vanno distribuiti insieme. I crediti degli artisti
e le condizioni SpriteCollab restano quelli di Petland; TamaPoke è il riferimento
di interfaccia, non la fonte dei file esportati.

Il test compila il lettore reale con MSVC `/W4 /WX`, verifica trasparenza,
RGB565, sequenza dei frame, specie assente, chiusura ripetuta e 13 file
malformati. Con il percorso aggiuntivo decodifica anche tutte le varianti
del pacchetto reale e quattro frame per variante.

Per verificare anche renderer e persistenza effettivamente usati nel sistema:

```powershell
python tools/test_pet_pokemon.py ../.release-work/pokemon-assets
python tools/test_pet_store.py
python tools/test_minigames.py
```

Il primo test controlla 62 varianti e 496 frame, riavvio, fallimento del commit,
identità invariata, ripristino originale e capienza dell'archivio. I minigiochi
verificano anche indici oltre i dodici colori del disegno precedente.

Prima di configurare la build di ciascun modello:

```powershell
$env:WATCH_OS_COMMON_DIR="C:/Users/matte/Documents/ESP-IDF/watch-os-common"
$env:WATCH_PET_POKEMON_ASSETS="C:/Users/matte/Documents/ESP-IDF/.release-work/pokemon-assets"
$env:FW_VERSION="1.15.0-dev.1"
```

Distribuire `CREDITS.json` insieme ai firmware che includono gli asset.
Senza questa opzione di build restano disponibili i pet originali.

Per le prove USB locali si può impostare `WATCH_LOCAL_QA_CONSOLE=1` prima
della configurazione. Abilita la console soltanto nei sorgenti diagnostici,
senza cambiare la configurazione SDK; ometterlo nelle build di distribuzione.
Su Windows `tools/dev/devcon_noreset.py --dtr` mantiene aperto il canale dati
USB senza attivare RTS. La console include `pet` e `pet appearance <dex> <shiny>`;
`dex=0` ripristina il disegno originale.

## Contratto del lettore file alternativo

Aprire un handle `pokemon_pack_t` inizializzato a zero su un worker di storage;
`open` valida l'intero file una volta. Il worker legge e conserva i frame
visibili in PSRAM. Nessun accesso file nel timer grafico: l'UI riceve soltanto
frame pronti. Un frame decodificato occupa 4.096 byte; lettura e conversione
usano circa 2,2 KB di stack oltre al buffer del chiamante. Dimensionare e
misurare lo stack del worker prima dell'uso sul dispositivo.

Se apertura o lettura falliscono, mantenere il disegno originale. Una lettura
fallita non modifica il buffer di uscita. Il file va tenuto immutabile durante
l'apertura; aggiornamenti e accesso USB richiedono chiusura e riapertura
coordinate. Lo stesso handle non è utilizzabile da più task contemporaneamente.

La prima versione del pack contiene solo Idle/Walk estratti da Petland:
mangiare, dormire e bagno usano gli effetti della scena, finché non esistono
asset dedicati. Non dichiarare sei animazioni diverse dai frame disponibili.

I viaggi restano invariati in questa fase. Prima di trasmettere un aspetto
servono metadati opzionali versionati e negoziati, conservati insieme alla
transazione. Companion e miniature devono adottare la stessa identità visiva
prima del rilascio. Vedere [la valutazione precedente](POKEMON-INTEGRAZIONE.md).

## Formato PKP1

Tutti i numeri sono little endian. Header di 20 byte: magic `PKP1`, versione
u16=1, numero voci u16 (1..302), offset indice u32=20, offset dati u32,
CRC32 u32 dell'intero contenuto dopo l'header.

Ogni voce occupa 12 byte: dex u16 (1..151), shiny u8 (0/1), frame u8 (1..4),
offset assoluto dati u32, colori u16 (1..255), riservato u16=0. Le voci sono
ordinate per `(dex, shiny)` senza duplicati. Ogni blocco contiene una palette
RGB565 u16 e `frame × 1.600` indici u8. L'indice 255 è trasparente. I blocchi
sono contigui e terminano esattamente alla fine del file, limitato a 4 MiB.
