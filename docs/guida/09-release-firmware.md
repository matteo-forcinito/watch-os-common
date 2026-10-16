# 9. Release dei firmware

Una **release** è una versione pubblicata che i dispositivi scaricano da soli dal cloud
(Impostazioni → Aggiornamento).

## Come si aggiorna un dispositivo

1. L'orologio scarica il **catalogo** del suo prodotto, per esempio
   `https://raw.githubusercontent.com/matteo-forcinito/watch-firmware-releases/main/amoled175c.json`.
2. Guarda il **canale** scelto: *Stabile* o *Dev* (dev riceve anche le stabili più nuove).
3. Se la versione è più alta di quella installata, scarica `firmware.bin` dalla release
   GitHub, controlla chip, **nome progetto** e SHA-256, lo scrive nello slot libero.
4. Riavvia nella nuova versione. Se crasha prima di finire l'avvio
   (`fw_update_confirm_boot()` in fondo a `main.c`), il bootloader torna alla precedente.

Le impostazioni restano (sono in NVS, non nel firmware).

## Versioni

| Formato | Canale | Esempio |
|---|---|---|
| `X.Y.Z` | stabile | `1.8.4` |
| `X.Y.Z-dev.N` | dev (pre-release) | `1.8.5-dev.1` |

Ordine: `1.8.4 < 1.8.5-dev.1 < 1.8.5-dev.2 < 1.8.5`. I dispositivi installano da soli
solo versioni **più alte**. Una versione pubblicata è **immutabile**: per correggere se ne
fa una nuova. Tutti i firmware dello stesso giro usano lo stesso numero (es. tutti 1.8.4).

## Preparare la release (checklist)

1. Tutte le modifiche di `watch-os-common` committate e pushate su `main`.
2. Build locale di **tutti** i firmware ok, prova su almeno un dispositivo ([pagina 7](07-test.md)).
3. In **ogni** firmware:
   - submodule spostato sul commit della libreria;
   - `release/notes/X.Y.Z.md` scritto. **La prima riga** (massimo 79 caratteri) è il
     riassunto che l'orologio mostra in "Tutte le versioni"; il resto va nella pagina
     della release;
   - `set(PROJECT_VER "X.Y.Z")` aggiornato nel `CMakeLists.txt`;
   - commit e push su `main`.

Esempio di note (`release/notes/1.8.4.md`):

```markdown
Schermo remoto fluido: trasmissione delle sole aree cambiate, display spento.

Lo schermo remoto cambia logica. Quando la dashboard web o la companion avviano la
trasmissione, il pannello del dispositivo si spegne...

Nessuna formattazione o cancellazione dati durante l'aggiornamento.
```

### Script: preparare tutti i firmware in un colpo

```powershell
$v = "1.8.5"
$lib = (git -C C:\Users\matte\Documents\ESP-IDF\watch-os-common rev-parse HEAD)
foreach ($d in "ESP32-S3-Amoled-Touch-1.8","ESP32-S3-Touch-AMOLED-1.75","ESP32-S3-Touch-AMOLED-1.75C","ESP32-S3-Knob-Touch-LCD-1.8","SenseCap-Watcher-W1-A") {
    Set-Location "C:\Users\matte\Documents\ESP-IDF\$d"
    git -C watch-os-common fetch -q origin
    git -C watch-os-common checkout -q $lib
    Copy-Item C:\percorso\note-$v.md "release\notes\$v.md"
    # solo la versione numerica: la riga "$ENV{FW_VERSION}" deve restare com'è
    (Get-Content CMakeLists.txt) -replace 'set\(PROJECT_VER "\d[^"]*"\)', "set(PROJECT_VER `"$v`")" | Set-Content -Encoding utf8 CMakeLists.txt
    git add watch-os-common "release/notes/$v.md" CMakeLists.txt
    git commit -m "Release ${v}: <riassunto>"
    git push origin main
}
```

## Pubblicare

Ogni firmware ha `tools/release.ps1`. Lui:

1. avvia la GitHub Action `release.yml` sul commit di `main`, con la versione;
2. aspetta la build e scarica i pacchetti verificati;
3. ricontrolla versione e **identità** dentro `firmware.bin`, gli SHA-256 e lo zip USB;
4. crea la release `<prodotto>-vX.Y.Z` nel repository pubblico `watch-firmware-releases`;
5. aggiorna il catalogo `<prodotto>.json`: **da qui i dispositivi la vedono**;
6. mette il tag `<prodotto>-vX.Y.Z` sul commit sorgente.

```powershell
$gh = "C:\Users\matte\Documents\ESP-IDF\ESP32-S3-Knob-Touch-LCD-1.8\.companion-tools\gh\bin\gh.exe"
cd C:\Users\matte\Documents\ESP-IDF\ESP32-S3-Touch-AMOLED-1.75C
powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Version 1.8.5 -Gh $gh
```

Output finale:

```
Build: https://github.com/matteo-forcinito/ESP32-S3-Touch-AMOLED-1.75C/actions/runs/...
catalog: ...amoled175c.json -> 1.8.5 stable | stable 1.8.5 | dev None
PUBLISHED: https://github.com/matteo-forcinito/watch-firmware-releases/releases/tag/amoled175c-v1.8.5
```

### Tutti e 5 in parallelo (circa 15–25 minuti)

```powershell
$gh = "C:\Users\matte\Documents\ESP-IDF\ESP32-S3-Knob-Touch-LCD-1.8\.companion-tools\gh\bin\gh.exe"
$v = "1.8.5"
$jobs = foreach ($d in "ESP32-S3-Amoled-Touch-1.8","ESP32-S3-Touch-AMOLED-1.75","ESP32-S3-Touch-AMOLED-1.75C","ESP32-S3-Knob-Touch-LCD-1.8","SenseCap-Watcher-W1-A") {
    Start-Job -Name $d {
        param($d, $gh, $v)
        Set-Location "C:\Users\matte\Documents\ESP-IDF\$d"
        & powershell -NoProfile -ExecutionPolicy Bypass -File tools\release.ps1 -Version $v -Gh $gh *> "$env:TEMP\release-$d.log"
        "$d exit=$LASTEXITCODE"
    } -ArgumentList $d, $gh, $v
}
$jobs | Wait-Job | Receive-Job
Get-ChildItem "$env:TEMP\release-*.log" | ForEach-Object { "== $($_.Name)"; Get-Content $_ -Tail 3 }
```

Ogni log deve finire con `PUBLISHED:`.

## Se qualcosa va storto

| Errore | Cosa fare |
|---|---|
| `Release notes are missing` | Manca `release/notes/X.Y.Z.md` nel firmware |
| `Build ended without verified release packages: failure` | Apri il link `Build:` e leggi il passo fallito. Spesso: submodule non pushato, oppure secret `REPOS_TOKEN` scaduto |
| Submodule non scaricato (`fatal: could not read Username`) | Rigenera il token *fine-grained* `REPOS_TOKEN` (lettura `watch-os-common`) e aggiornalo in Settings → Secrets del repo firmware |
| `Wrong firmware version or hardware identity` | Il nome progetto o la versione nell'immagine non corrispondono: controlla `project(...)` |
| `Published versions are immutable` | Versione già pubblicata: usa un numero nuovo |
| `Release published but catalog update failed` | Rilancia lo stesso comando: la release esiste già (bozza completata), riprova il catalogo |
| Build ok ma pubblicazione interrotta | Rilancia riusando la build: `python ..\watch-os-common\tools\release_firmware.py --repo matteo-forcinito/<repo> --product <prodotto> --version X.Y.Z --project . --gh $gh --run-id <numero run>` |

Prodotti validi per `--product`: `amoled`, `amoled175`, `amoled175c`, `knob`, `watcher`.

## Flash via USB di una release (recupero)

Ogni release ha uno zip `<prodotto>-usb-flash-X.Y.Z.zip`. Estrailo e:

```powershell
python -m esptool --chip esp32s3 -p COM10 -b 921600 write-flash "@flash_args"
```

Non contiene NVS: le impostazioni restano. Sul Watcher non usare mai immagini "merged"
(cancellerebbero `nvsfactory`).

## La sveglia e-paper

`esp32-s3-epaper-1.54` è separata e non usa `watch-os-common`: ha il suo
`.github/workflows/release.yml` e il catalogo `epaper.json`. Vedi il suo `README.md`.
