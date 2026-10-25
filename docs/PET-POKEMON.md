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

Il selettore Aspetto dovrà mostrare le specie disponibili nel pacchetto,
la variante normale/shiny e il comando per tornare all'aspetto originale.
Le forme evolute sono aspetti espliciti: non cambiano automaticamente
al livello 5/12 dell'RPG. Il pet conserva nome, seed, XP, provenienza e cure.
L'aspetto va registrato separatamente e indicizzato per seed, senza cambiare
il blob NVS dei child. La scelta si applica al pet di origine presente a casa.

## Prima implementazione disponibile

- Esportatore dei dati grafici già generati per Petland, senza altri download.
- Pacchetto file autonomo: 31 specie, 62 varianti, 360.398 byte sul roster attuale.
- Lettore C portabile registrato nel componente `orbi`, indipendente da LVGL.
- Validazione di versione, CRC32, ordine delle identità, dimensioni e offset.
- Lettura di un frame per ID nazionale e shiny; normalizzazione da 40×40
  a 32×32 ARGB8888, pronta per le dimensioni dei canvas esistenti.
- Nessuna sprite aggiunta al binario di sistema o ai sorgenti Git.

Il lettore **non è ancora collegato alla pagina**. Il nuovo layout, il selettore,
la persistenza dell'aspetto e il worker/cache sono i prossimi passi;
non è stata compilata o installata una nuova immagine firmware per questa attività.
Il pet esistente continua quindi a mostrare l'animale originale.

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

## Contratto per l'integrazione

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
