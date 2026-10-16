# 12. Flusso di lavoro dalla A alla Z

La sequenza che uso per ogni modifica, con i comandi reali. Esempio: una modifica al
sistema condiviso (`watch-os-common`) da rilasciare su tutti i dispositivi.

## A. Capire prima di toccare

1. Leggi il `CLAUDE.md` del firmware (regole) e l'header di commento dei file coinvolti.
2. Cerca dove sta la cosa da cambiare:
   ```powershell
   cd C:\Users\matte\Documents\ESP-IDF\watch-os-common
   Select-String -Path components\*\*.c,components\*\*\*.c -Pattern "remote_frame" | Select-Object -First 10
   git log --oneline -10            # cosa è cambiato di recente
   git status                       # c'è lavoro non salvato?
   ```
3. Riproduci il problema sul dispositivo (log, screenshot, script di [pagina 8](08-console-e-diagnostica.md)).

## B. Branch di lavoro

```powershell
cd C:\Users\matte\Documents\ESP-IDF\watch-os-common
git checkout main
git pull
git checkout -b fix/descrizione-breve
```

(Se tocchi anche la companion, stesso nome di branch in `Orbit-Companion`.)

## C. Modifica e compila in tutti i firmware

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
$env:WATCH_OS_COMMON_DIR = "C:/Users/matte/Documents/ESP-IDF/watch-os-common"
foreach ($d in "ESP32-S3-Amoled-Touch-1.8","ESP32-S3-Touch-AMOLED-1.75","ESP32-S3-Touch-AMOLED-1.75C","ESP32-S3-Knob-Touch-LCD-1.8","SenseCap-Watcher-W1-A") {
    Set-Location "C:\Users\matte\Documents\ESP-IDF\$d"
    idf.py build > build-local.log 2>&1
    "$d -> exit $LASTEXITCODE"
}
```

Tutti `exit 0`. Un warning nel nostro codice è un errore: correggilo.

## D. Test

```powershell
cd C:\Users\matte\Documents\ESP-IDF\watch-os-common
node tools\test_remote_frame.cjs                 # se hai toccato la dashboard
python pieno\tests\run_tests.py --deps build\ecos-checks\test-deps --output $env:TEMP\t   # se hai toccato Pieno
```

e i test vicini a ciò che hai cambiato ([pagina 7](07-test.md)). Per la companion:
`flutter analyze` e `flutter test`.

## E. Prova sui dispositivi collegati

Al 16/10/2026 sono collegati: **COM10 = AMOLED 1.75C**, **COM14 = AMOLED 1.75**
(verifica sempre con `bootlog.py`, le porte possono cambiare).

```powershell
cd C:\Users\matte\Documents\ESP-IDF\ESP32-S3-Touch-AMOLED-1.75C
python -I ..\watch-os-common\tools\dev\bootlog.py COM10 6 "Project name"   # è la scheda giusta?
idf.py -p COM10 app-flash
python -I ..\watch-os-common\tools\dev\bootlog.py COM10 12 "App version|Ready|E (|CRASH"
```

Poi la prova della funzione (screenshot, dashboard, companion, script di collaudo).
Se serve la console di sviluppo, abilitala solo nel `sdkconfig` locale ([pagina 8](08-console-e-diagnostica.md)).

## F. Documenta

- Aggiorna i documenti in `docs/` se cambia un comportamento o un'API.
- Se aggiungi una regola importante, aggiungila al `CLAUDE.md`.

## G. Commit e push della libreria

```powershell
cd C:\Users\matte\Documents\ESP-IDF\watch-os-common
git add components docs tools
git commit -m "Spiegazione breve al presente, in inglese"
git checkout main
git merge --ff-only fix/descrizione-breve
git push origin main fix/descrizione-breve
```

## H. Sposta il submodule nei firmware, note e versione

Script in [pagina 9](09-release-firmware.md) ("preparare tutti i firmware in un colpo"):
per ognuno dei 5 firmware submodule → commit della libreria, `release/notes/X.Y.Z.md`,
`PROJECT_VER`, commit, push su `main`.

Controllo veloce che tutti puntino allo stesso commit:

```powershell
foreach ($d in "ESP32-S3-Amoled-Touch-1.8","ESP32-S3-Touch-AMOLED-1.75","ESP32-S3-Touch-AMOLED-1.75C","ESP32-S3-Knob-Touch-LCD-1.8","SenseCap-Watcher-W1-A") {
    "$d " + (git -C "C:\Users\matte\Documents\ESP-IDF\$d" submodule status watch-os-common)
}
```

## I. Release

Tutti e 5 in parallelo con `tools/release.ps1` ([pagina 9](09-release-firmware.md)). Ogni log
finisce con `PUBLISHED:`. Da quel momento i dispositivi vedono l'aggiornamento in
Impostazioni → Aggiornamento.

Se c'è un dubbio sulla stabilità: prima una versione dev (`1.8.5-dev.1`), provala su un
dispositivo impostato sul canale Dev, poi la stabile `1.8.5`.

## L. Verifica dopo la release

- Un dispositivo: Impostazioni → Aggiornamento → Controlla → Installa. Al riavvio
  `App version: X.Y.Z` e `main: Ready`.
- Catalogo pubblico: `https://raw.githubusercontent.com/matteo-forcinito/watch-firmware-releases/main/<prodotto>.json`.

## M. Companion (se modificata)

Commit sul branch, merge su `main`, poi versione in `pubspec.yaml`, note e push di
`release/X.Y.Z` ([pagina 11](11-companion-flutter.md)).

## Promemoria rapido

| Voglio... | Comando |
|---|---|
| Attivare l'ambiente | `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` |
| Compilare contro la mia libreria | `$env:WATCH_OS_COMMON_DIR="C:/Users/matte/Documents/ESP-IDF/watch-os-common"; idf.py build` |
| Flashare senza perdere impostazioni | `idf.py -p COM10 app-flash` |
| Leggere il log | `idf.py -p COM10 monitor` (esci con Ctrl+]) |
| Screenshot | `python tools\devcon.py COM10 shot:x.png` (con console di sviluppo) |
| Capire un crash | `idf.py -p COM10 coredump-info` |
| Rilasciare | `powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Version X.Y.Z -Gh $gh` |
| Avviare la companion | `flutter run -d windows` |
