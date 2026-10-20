# Rilascio da una build locale

`tools/release_firmware.py` mantiene il flusso Actions predefinito e accetta anche `--local-build PERCORSO_BUILD`. Questa alternativa usa il client ufficiale `gh` già autenticato, senza leggere o copiare token.

Prima del rilascio occorrono una build ESP-IDF di produzione con la versione richiesta e `CONFIG_WATCH_DEV_CONSOLE` disabilitato, sorgenti tracciati puliti e già pubblicati sul riferimento remoto scelto, submodule fissato alla revisione effettivamente compilata e note in `release/notes/VERSIONE.md` nel BSP.

Esempio, dalla cartella del BSP con l'ambiente ESP-IDF attivo:

```sh
python watch-os-common/tools/release_firmware.py --repo OWNER/REPOSITORY --product guition4848 --version 1.9.0 --project . --local-build build --verify-only
```

`--verify-only` prepara e verifica i pacchetti senza modificare GitHub. Per pubblicare si ripete lo stesso comando senza questa opzione. `--gh` permette di indicare il percorso dell'eseguibile ufficiale e `--output` sceglie la cartella dei pacchetti.

Prodotti supportati: `amoled`, `knob`, `amoled175`, `amoled175c`, `watcher`, `guition4848`. L'identità interna e la versione dell'immagine devono corrispondere al prodotto richiesto; la modalità diagnostica viene rifiutata. Vengono preparati immagine OTA, ZIP per il recupero USB, manifest con revisioni dei sorgenti e hash SHA-256. Lo ZIP usa gli indirizzi della build e non contiene immagini NVS, dati di fabbrica o immagini dell'intera flash.

La pubblicazione carica prima gli asset in una bozza, fissa il tag del repository sorgente alla revisione verificata, pubblica la release e aggiorna il catalogo OTA pubblico. Una versione già pubblicata non viene sovrascritta. Il recupero USB non richiede `erase-flash` e conserva NVS e file.
