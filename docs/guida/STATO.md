# Stato del progetto (17/10/2026)

## Firmware: tutto rilasciato e allineato

| Firmware | Versione pubblicata | Commit `watch-os-common` nel submodule |
|---|---|---|
| AMOLED 1.8 | 1.8.4 stabile | `6269065` |
| AMOLED 1.75 | 1.8.4 stabile | `6269065` |
| AMOLED 1.75C | 1.8.4 stabile | `6269065` |
| Knob 1.8 | 1.8.4 stabile | `6269065` |
| Watcher | 1.8.4 stabile | `6269065` |

Novità 1.8.4: schermo remoto con display spento e solo aree cambiate (ORF2), nuova rete
Wi-Fi via Bluetooth. Collaudo hardware: 1.75C (streaming, input, fine sessione) e 1.75
(avvio). AMOLED 1.8, Knob e Watcher: compilati, non provati su hardware.

Dispositivi collegati al PC: COM10 = AMOLED 1.75C (1.8.4 normale), COM14 = AMOLED 1.75 (1.8.4).

Dopo la release, su `watch-os-common/main` ci sono solo documentazione (questa guida),
script `tools/dev/` e il test della dashboard aggiornato: non richiedono una nuova release.

## Companion: pronta, pubblicazione bloccata da un test Android preesistente

- Codice su `Orbit-Companion/main` (commit `bc0861d`), versione `0.4.2-dev.4+9`, note in
  `release/notes/0.4.2-dev.4.md`. Analisi pulita, 46 test superati.
- Branch `release/0.4.2-dev.4` pushato: la Action compila **Windows e Android con successo**,
  ma il job `smoke-android` (prova su emulatore) fallisce, quindi la release non viene pubblicata.
- Errore: `test01NotificationRuntimePermission` → `Platform method timed out`, poi
  `test02ForegroundServiceAndBluetoothPermissions` → "Autorizza le notifiche di Orbit...".
  **Era identico già prima** di queste modifiche (run del 16/10 ore 15:03 sul branch
  `fix/connection-stability`): riguarda la richiesta del permesso notifiche su Android 15
  nell'emulatore, non lo schermo remoto.
- In locale c'è uno `stash` con i file di plugin rigenerati da Flutter
  (`git stash list` in `Orbit-Companion`): non servono, si possono scartare con `git stash drop`.

### Come procedere

1. Guarda il test: `Orbit-Companion/android/app/src/androidTest/` (`CompanionSmokeTest`) e
   `tools/smoke_android.py`. Il primo test non riesce a concedere il permesso notifiche
   entro il tempo: controlla come lo concede (dialog di sistema o `pm grant`) su API 35.
2. Correggi, poi nuova versione (le release sono immutabili, ma questa non è stata pubblicata:
   puoi riusare lo stesso branch con un nuovo push):
   ```powershell
   cd C:\Users\matte\Documents\ESP-IDF\Orbit-Companion
   git checkout release/0.4.2-dev.4
   # ...correzione...
   git commit -am "Fix Android notification permission smoke test"
   git push
   ```
3. Segui la Action:
   ```powershell
   $gh = "C:\Users\matte\Documents\ESP-IDF\ESP32-S3-Knob-Touch-LCD-1.8\.companion-tools\gh\bin\gh.exe"
   & $gh run list -R matteo-forcinito/Orbit-Companion --limit 3
   ```
   Rilancio del solo job fallito: `& $gh run rerun <id> -R matteo-forcinito/Orbit-Companion --failed`.
4. Ricordati di riportare la correzione anche su `main` (`git checkout main; git merge release/0.4.2-dev.4`).

Nel frattempo la companion si può usare dal PC con `flutter run -d windows` ([pagina 11](11-companion-flutter.md)).

## In corso: app Bilancio (gestione del denaro)

Progetto in [docs/progetti/BILANCIO.md](../progetti/BILANCIO.md). Stato al 17/10/2026, in
`watch-os-common/bilancio/` (**non ancora committato**, non registrato in nessun firmware):

- **Passo 1 fatto**: `main/calc.c` (logica pura) + `tests/test_calc.c`. Test superato:
  ```powershell
  . C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
  cd C:\Users\matte\Documents\ESP-IDF\watch-os-common
  python bilancio\tests\run_tests.py --deps build\ecos-checks\test-deps --output $env:TEMP\bilancio-test
  ```
  Atteso: `PASS: bilancio money logic, real C in RISC-V emulator`.
- **Passo 2 fatto**: `store.c` (dati in PSRAM, `<storage>/config/bilancio.json`), `web.c` (API),
  `bilancio_dashboard.html` (Panoramica, Movimenti, Fissi e rate, Entrate, Impostazioni con saldo,
  categorie e backup). Compilati con `-Werror` usando i flag della build AMOLED 1.75C (solo sintassi);
  dashboard provata in Edge con un server finto. **Mai provata su un orologio vero**: il primo test
  reale arriva con il passo 6 (registrazione nei firmware).
- In più rispetto al progetto: voci oltre l'anno (revisione ogni 2 anni, patente ogni 10), movimenti
  previsti (rimborso 730), pagamenti collegati alle voci fisse con storico delle bollette,
  accantonamento mensile, mutuo e prestiti con rate rimaste e residuo.

**Su AMOLED 1.75C (COM10)**: build `build-bilancio` (versione ancora `1.8.4`, compilata il 17/10 alle
01:36) flashata con `app-flash`; avvio fino a `main: Ready`, unico `E (` il noto `sleep_cpu_configure`.
Schermate e dashboard da controllare a vista sull'orologio (console di sviluppo spenta: niente
screenshot automatici). Per tornare alla release: `idf.py -p COM10 app-flash` dalla build normale.

### Portare Bilancio su un firmware (fatto per AMOLED 1.75C, gli altri a mano)

La registrazione (passo 6) è già in `watch-os-common`: `components/apps/CMakeLists.txt` (blocco
`BILANCIO` dentro `CONFIG_APPKIT_HOSTED`), `apps.c` (`&bilancio_app` e
`appkit_hosted_register("Bilancio", "bilancio")`), `apps_internal.h`. Le schermate sono in
`bilancio/main/ui.c` (Home con arco, Questo mese, Spesa veloce, Menu/Dashboard). Per ogni firmware
(AMOLED 1.8, 1.75, Knob, Watcher):

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
$env:WATCH_OS_COMMON_DIR = "C:\Users\matte\Documents\ESP-IDF\watch-os-common"   # la copia con Bilancio
cd C:\Users\matte\Documents\ESP-IDF\<cartella del firmware>
idf.py -B build-bilancio build            # cartella separata: la build di release resta intatta
idf.py -B build-bilancio -p COMxx app-flash
```

Poi sull'orologio: Bilancio nell'elenco app → Menu → Dashboard → inquadra il QR. Dalla
dashboard: saldo, stipendio, fissi. Controlla la Home su schermo tondo e rettangolare (testo non
tagliato dal cerchio). Tutti e quattro hanno `CONFIG_APPKIT_HOSTED=y` (verificato il 17/10/2026).

Per una release vera: commit e push in `watch-os-common`, aggiorna il submodule in ogni firmware e
segui [pagina 9](09-release-firmware.md) (versione 1.9.0).

### Come procedere

1. **Passo 3**: `bilancio/main/ui.c` Home e Questo mese, nello stile della schermata LIVE di
   Copertura (design nel documento, sezione "Sull'orologio"). Poi `app.c` e `main.c`.
2. Passi 4–7 come da documento; il **passo 8** (nuovo) è il confronto delle tariffe luce/gas con
   dati pubblici ARERA/GME, da verificare prima di scrivere codice.
3. Prima di committare: `git status` in `watch-os-common`. I file nuovi sono `bilancio/` e le
   modifiche a `docs/progetti/BILANCIO.md`, `docs/guida/STATO.md` e `docs/guida/07-test.md`.

## Cose note, non urgenti

- Log `E sleep: sleep_cpu_configure ... Failed to enable CPU power down during light sleep`
  all'avvio: noto, nessun effetto funzionale (TODO nel `CLAUDE.md` del Watcher).
- Latenza minima dello schermo remoto circa 100 ms: con Bluetooth attivo il Wi-Fi resta
  in modem sleep (coesistenza radio).
- File `build-local.log` non tracciati nelle cartelle dei firmware: sono log delle build
  locali, si possono cancellare.
