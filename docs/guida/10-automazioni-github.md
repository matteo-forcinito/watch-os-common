# 10. Automazioni GitHub (GitHub Actions)

Una **GitHub Action** è uno script che GitHub esegue sui suoi server (un PC Linux o
Windows usa-e-getta) quando succede qualcosa: un push, un comando manuale. Si descrive
in un file YAML dentro `.github/workflows/` del repository.

Perché la usiamo: la build di release è **riproducibile**. Parte sempre da zero, dal
commit pubblicato, con ESP-IDF 6.1 pulito e solo `sdkconfig.defaults`, quindi senza
opzioni locali dimenticate (console di sviluppo, ecc.).

## Dove vederle

`https://github.com/matteo-forcinito/<repo>/actions`, oppure da terminale:

```powershell
$gh = "C:\Users\matte\Documents\ESP-IDF\ESP32-S3-Knob-Touch-LCD-1.8\.companion-tools\gh\bin\gh.exe"
& $gh run list -R matteo-forcinito/ESP32-S3-Touch-AMOLED-1.75C --limit 5
& $gh run view 38957208598 -R matteo-forcinito/ESP32-S3-Touch-AMOLED-1.75C --log-failed   # solo i passi falliti
```

## I workflow del progetto

### Firmware: `release.yml` (in ogni repo firmware)

Si avvia con `tools/release.ps1` (comando manuale "workflow_dispatch" con la versione) o
con un push su un branch `release/X.Y.Z`. Passi:

```
1. Version from the branch name   → legge X.Y.Z, decide il canale (stable/dev)
2. Checkout                       → scarica il repo
3. Submodule watch-os-common      → scarica la libreria privata con il secret REPOS_TOKEN
4. ESP-IDF v6.1 (cache)           → installa ESP-IDF (o lo riprende dalla cache)
5. Build                          → rm sdkconfig; idf.py build; controlla la versione nell'immagine
6. Build and package <app>        → (AMOLED 1.8) compila Remote, Orbit, WebRadio come zip
7. Package                        → firmware.bin, zip per flash USB, SHA256SUMS.txt
8. Preserve verified packages     → carica tutto come "artifact" (conservato 7 giorni)
```

La Action **non pubblica**: produce i pacchetti. La pubblicazione la fa
`tools/release.ps1` dal tuo PC con `gh` autenticato, dopo averli riverificati
([pagina 9](09-release-firmware.md)). Così nessun token con permessi di scrittura sul
repository pubblico sta su GitHub Actions.

Secret necessario: `REPOS_TOKEN` (token *fine-grained*, sola lettura su `watch-os-common`).
Quando scade, la build fallisce al passo 3: rigeneralo su GitHub → Settings → Developer
settings → Personal access tokens, e aggiornalo in ogni repo firmware → Settings →
Secrets and variables → Actions.

### `watch-os-common`: test automatici

| Workflow | Quando parte | Cosa fa |
|---|---|---|
| `hub-tests.yml` | push che tocca `remote/hub/**` | compila e lancia i test C dello stato Remote Hub con i sanitizer |
| `sentinel-tests.yml` | push che tocca `sentinel/**` | test dei pacchetti Sentinel |
| `vault-tests.yml` | push che tocca `vault/**` | test del formato Vault |

Partono da soli: se diventano rossi, qualcosa nel codice di quell'app si è rotto.

### Companion: `release.yml` e `validate.yml`

| Workflow | Quando parte | Cosa fa |
|---|---|---|
| `validate.yml` | manuale | analisi, test, build Windows e Android di prova |
| `release.yml` | push di `release/X.Y.Z` (versione uguale a `pubspec.yaml`) | analisi + test, build Windows (zip) e Android (APK firmato), prova su emulatore Android, poi **pubblica** la release `vX.Y.Z` nello stesso repository |

Secret: `ORBIT_ANDROID_KEYSTORE_BASE64`, `ORBIT_ANDROID_KEYSTORE_PASSWORD` (la firma
Android). Secondo `Orbit-Companion/docs/RELEASE.md` il backup locale della chiave va in
`Orbit-Companion\.signing\` (ignorata da git): conservalo, perché senza la stessa firma
gli APK nuovi non aggiornano quelli installati.

### E-paper: `release.yml`

Push di un branch `release/X.Y.Z` → build e pubblicazione **direttamente dalla Action**
nel catalogo `epaper.json`. Usa il secret `RELEASES_TOKEN` (lettura e scrittura su
`watch-firmware-releases`).

## Leggere un workflow YAML

```yaml
on:
  push:
    branches: ["release/**"]     # quando: push su un branch che inizia con release/
jobs:
  release:
    runs-on: ubuntu-latest       # su quale macchina
    steps:
      - name: Checkout           # un passo con un nome
        uses: actions/checkout@v4      # azione pronta di GitHub
      - name: Build
        run: |                   # comandi shell
          idf.py build
```

## Avviare un workflow a mano

```powershell
& $gh workflow run release.yml -R matteo-forcinito/ESP32-S3-Touch-AMOLED-1.75C --ref main --field version=1.8.5-dev.1
& $gh workflow run validate.yml -R matteo-forcinito/Orbit-Companion --ref fix/connection-stability
```
