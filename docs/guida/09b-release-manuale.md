# Release manuale senza GitHub Actions

Questa procedura compila sul PC e pubblica con GitHub CLI. Non avvia workflow e
non usa minuti di GitHub Actions. Servono ESP-IDF configurato, Python, Git,
accesso ai repository sorgenti e permessi di scrittura su `watch-firmware-releases`.

## 1. Controllare le operazioni in corso

Monitor pronto all'uso dalla cartella `ESP-IDF`:

```powershell
python watch-os-common/tools/build_status.py
python watch-os-common/tools/build_status.py --watch --interval 10
# Per le build nelle cartelle originali anziché nelle worktree:
python watch-os-common/tools/build_status.py --root . --watch
```

Uscire con Ctrl+C. Il monitor legge processi e log senza modificare le build.
Mostra l'ultimo passo Ninja, la sua percentuale e l'età dell'ultimo aggiornamento.
La percentuale rappresenta il numero di passi, non una stima del tempo residuo;
può ripartire durante sottobuild. `ESITO NON CERTO` indica che non ci sono prove
sufficienti del completamento o di un processo attribuibile al progetto: controllare
i log e i PID indicati. `COMPLETATA` indica il controllo finale delle dimensioni
nel log, non la verifica dei pacchetti né l'avvenuta pubblicazione.

In PowerShell:

```powershell
Get-CimInstance Win32_Process |
    Where-Object { $_.Name -match 'python|ninja|cmake' -and
        $_.CommandLine -match 'idf.py|build-release|release_firmware.py' } |
    Select-Object ProcessId, ParentProcessId, Name, CommandLine
```

Se una build o una pubblicazione è già attiva per lo stesso progetto, aspettare
che termini. Non compilare due volte nella stessa cartella. I log ESP-IDF sono in
`build-release/log/`; un log vuoto da solo non dimostra che il processo sia bloccato.

## 2. Preparare i sorgenti

Per ogni firmware: impostare `PROJECT_VER` in `CMakeLists.txt`, aggiungere
`release/notes/<versione>.md`, aggiornare il submodule `watch-os-common`, committare
e fare push sul branch scelto. Anche il commit della libreria deve essere pushato.
La prima riga delle note deve essere un riassunto leggibile di massimo 79 caratteri.
Usare una cartella sorgente pulita, preferibilmente una worktree dedicata.

Per preparare i branch con lo strumento esistente senza avviare Actions, usare
`tools/dev/release_dev.ps1` con `-NoPublish` (e `-Promote` per la stabile).
Leggere prima la [guida generale](09-release-firmware.md): la preparazione esegue
commit e push. Il normale `tools/release.ps1` avvia invece Actions: per questa
procedura usare direttamente `release_firmware.py --local-build`.

| Prodotto | Repository/cartella | Nome immagine |
|---|---|---|
| `amoled` | `ESP32-S3-Amoled-Touch-1.8` | `amoled_watch.bin` |
| `amoled175` | `ESP32-S3-Touch-AMOLED-1.75` | `amoled175_watch.bin` |
| `amoled175c` | `ESP32-S3-Touch-AMOLED-1.75C` | `amoled175c_watch.bin` |
| `knob` | `ESP32-S3-Knob-Touch-LCD-1.8` | `knob_os.bin` |
| `watcher` | `SenseCap-Watcher-W1-A` | `watcher_os.bin` |
| `guition4848` | `ESP32-4848S040C-I` | `guition4848_panel.bin` |

## 3. Compilare sul PC

Aprire il terminale ESP-IDF della versione prevista dal progetto (le build 1.9.4
usano ESP-IDF 6.1). Esempio per AMOLED 1.8, nella worktree già preparata:

```powershell
$root = 'C:\Users\matte\Documents\ESP-IDF'
$project = Join-Path $root '.release-work\ESP32-S3-Amoled-Touch-1.8'
Set-Location $project
idf.py -B build-release menuconfig
# Disabilitare CONFIG_WATCH_DEV_CONSOLE se presente e salvare.
# Se sdkconfig è tracciato e cambia, committarlo e pusharlo prima della build.
idf.py -B build-release build
if ($LASTEXITCODE -ne 0) { throw 'Build fallita: non pubblicare' }
```

Usare la configurazione di produzione del progetto. Se si modifica un sorgente,
il submodule o la configurazione, ricompilare prima di confezionare i pacchetti.
Provare il firmware su almeno un dispositivo prima della pubblicazione stabile.

## 4. Verificare i pacchetti senza pubblicare

Nello stesso terminale:

```powershell
$version = '1.9.4'
$product = 'amoled'
$repo = 'matteo-forcinito/ESP32-S3-Amoled-Touch-1.8'
$gh = Join-Path $root '.release-work\github-cli\bin\gh.exe'
$tool = Join-Path $root 'watch-os-common\tools\release_firmware.py'
& $gh auth status
if ($LASTEXITCODE -ne 0) { throw 'Eseguire gh auth login' }
$releaseArgs = @(
    $tool, '--repo', $repo, '--product', $product, '--version', $version,
    '--source-ref', 'main', '--project', $project,
    '--local-build', (Join-Path $project 'build-release'), '--gh', $gh,
    '--output', (Join-Path $root "release-output\manual-$version")
)
python @releaseArgs --verify-only
if ($LASTEXITCODE -ne 0) { throw 'Verifica fallita: non pubblicare' }
```

Se `gh` è installato nel PATH, usare `$gh = 'gh'`. Per una versione dev usare il
branch `dev` al posto di `main` e un numero come `1.9.5-dev.1`.

Il risultato deve terminare con `VERIFIED:` e il percorso dei pacchetti. La
verifica controlla sorgenti tracciati puliti, HEAD uguale al commit remoto,
submodule effettivamente usato dalla build uguale a quello fissato nel sorgente,
console diagnostica disabilitata, versione e identità nell'immagine, checksum e
archivio USB. Richiede rete per controllare il commit remoto, ma non pubblica.

## 5. Pubblicare manualmente

Solo quando le build e le verifiche richieste sono finite, nello stesso terminale:

```powershell
python @releaseArgs
if ($LASTEXITCODE -ne 0) { throw 'Rilascio non completato: leggere il messaggio' }
```

Lo script ricontrolla la build, crea una bozza nel repository pubblico, carica
`firmware.bin`, ZIP USB, `release-manifest.json` e `SHA256SUMS.txt`, tagga il commit
sorgente, pubblica e aggiorna `<prodotto>.json`. I dispositivi vedono l'aggiornamento
quando il catalogo viene aggiornato. L'output deve terminare con `PUBLISHED:`.

Ripetere i passi 3–5 cambiando `$project`, `$product` e `$repo` secondo la tabella.
È preferibile pubblicare in sequenza per rendere semplice il controllo dei risultati.
Il manifest indica `build_type: local` e il commit sorgente: non serve un run Actions.

## 6. Controllare il risultato

```powershell
$public = 'matteo-forcinito/watch-firmware-releases'
$tag = "$product-v$version"
& $gh release view $tag -R $public --json url,isDraft,assets
& $gh api "repos/$public/contents/$product.json" --jq '.download_url'
```

Aprire il catalogo all'URL restituito e verificare versione, URL e SHA-256 della
nuova voce; verificare anche il canale stabile/dev e provare la ricerca aggiornamenti
dal dispositivo. Conservare i pacchetti e il manifest come traccia del rilascio.

## Errori e ripresa

- **Sorgenti sporchi / commit remoto diverso:** committare e pushare le modifiche
  corrette, verificare branch e submodule, poi ricompilare e riverificare.
- **Console diagnostica abilitata:** disabilitarla nella configurazione di
  produzione, salvare/committare se necessario e ricompilare.
- **Bozza incompleta:** correggere il problema e rilanciare il comando; lo script
  può riutilizzare la bozza e ricaricare gli asset.
- **Versione già pubblicata:** lo script rifiuta di sovrascriverla. Non cancellare
  la release per aggirare il controllo; usare una nuova versione per correzioni.
- **Release pubblicata ma catalogo fallito:** la release è già pubblica ma i
  dispositivi potrebbero non vederla. Il comando attuale rifiuta anche questo
  secondo tentativo perché la release non è più una bozza: occorre recuperare
  separatamente il solo catalogo con `tools/update_catalog.py` e pubblicarne il
  contenuto verificato. Non cancellare né sostituire gli asset della release.
- **GitHub Actions senza minuti:** non usare `tools/release.ps1` o la modalità
  senza `--local-build`; nessun aumento del piano è necessario per compilare sul PC.

Lo ZIP USB esclude immagini NVS e immagini merged. Per un recupero seguire il suo
README ed evitare `erase-flash`, così si conservano impostazioni e dati di fabbrica.
