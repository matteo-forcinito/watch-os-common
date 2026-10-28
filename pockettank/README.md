# Pocket Tank (app esterna)

Acquario-tamagotchi di [mediacutlet/pocket-tank](https://github.com/mediacutlet/pocket-tank)
(MIT, `LICENSE`): un transformer da 14M parametri (Q4, 7,56 MB) gira sull'ESP32-S3 e decide
il comportamento dei pesci, senza rete.

Upstream vendorizzato al commit `3df10498f2814f40f5ca7066535f3d44985e0ec2` (v0.3.8.1):
`common/`, `firmware/main`, `firmware/components`, `assets/sounds/sounds.bin`.
`Kconfig.projbuild` è stato spostato in `firmware/components/tankcore`.

## Adattamenti a Watch OS (`PT_WATCH_OS`, `firmware/main/pt_watch_os.c`)

- `extapp_sdk_init()` all'avvio: ogni reset torna al launcher, tranne il deep sleep
  notturno, che si risveglia nell'acquario.
- **BOOT** (pressione breve) salva l'acquario e torna al launcher. PWR resta quello upstream
  (breve = sonno, lungo = spegnimento).
- NVS condiviso: mai cancellato; il namespace Wi-Fi di Pocket Tank è `ptwifi` (`wifi` è dell'OS).
  Il salvataggio resta in `tank`.
- Aggiornamenti Wi-Fi e provisioning disattivati: scriverebbero su ota_0 (il launcher).
- Il modello non ha una partizione: sta in due zone non partizionate, mappate come un
  blocco contiguo con `spi_flash_mmap_pages` e verificato con SHA-256 a ogni avvio
  (`0x880000-0xC20000`, coda di ota_1, e `0xC60000-0xFFF000`, trailer a `0xFFF000`).
  L'app deve restare sotto `0x260000` byte. Un'altra app esterna più grande o un
  aggiornamento dell'OS possono sovrascrivere la prima zona: l'acquario parte comunque
  (pesci a regole) e il modello va reinstallato.

## Build e installazione (AMOLED 1.8)

```powershell
cd ESP32-S3-Amoled-Touch-1.8\external_apps\pockettank
idf.py -B build build
python ..\..\watch-os-common\pockettank\tools\install_usb.py COM3 --app build\pockettank_amoled.bin `
  --expected-mac 10:20:ba:46:46:3c --backup-dir <cartella backup>
```

Lo script scarica il modello dal commit upstream fissato (o `--model file`), ne verifica lo
SHA-256, fa il backup di `0x0-0x21000`, scrive app + modello + trailer e seleziona ota_1.
`--skip-model` aggiorna solo l'app.
