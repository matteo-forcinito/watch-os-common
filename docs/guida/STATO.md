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

## Prossimo progetto

App **Bilancio** (gestione del denaro): progetto completo in
[docs/progetti/BILANCIO.md](../progetti/BILANCIO.md), codice da scrivere seguendo
l'"Ordine di lavoro" del documento.

## Cose note, non urgenti

- Log `E sleep: sleep_cpu_configure ... Failed to enable CPU power down during light sleep`
  all'avvio: noto, nessun effetto funzionale (TODO nel `CLAUDE.md` del Watcher).
- Latenza minima dello schermo remoto circa 100 ms: con Bluetooth attivo il Wi-Fi resta
  in modem sleep (coesistenza radio).
- File `build-local.log` non tracciati nelle cartelle dei firmware: sono log delle build
  locali, si possono cancellare.
