# Muse come app esterna di Watch OS

Firmware ufficiale di Meta (`facebookincubator/muse-gadget-sdk`, Apache 2.0)
compilato per girare in **ota_1**, accanto a Watch OS in ota_0. Il codice Muse
non è copiato qui: qui ci sono solo l'adattamento e gli script.

Supporto ufficiale Muse tra i nostri modelli: AMOLED 1.75, 1.75C, SenseCAP Watcher.
Verificato finora: AMOLED 1.75C.

## Cosa cambia rispetto a Muse originale

`apply_watch_os.py` (fallisce se il codice upstream è cambiato):

- **NVS mai cancellato.** Muse lo cancellava se pieno; da noi contiene anche
  sveglie, Wi-Fi e dati di Watch OS.
- **Impostazioni > Watch OS** riavvia nel launcher (ota_0). Muse resta installato
  e si riapre da launcher > App esterne.

`sdkconfig.watch-os` (caricato dopo i file Muse):

- nostra tabella partizioni (offset 0x8000), Muse in ota_1;
- OTA Muse spento: scriverebbe nello slot di Watch OS;
- NVS in chiaro: la cifratura programmerebbe chiavi negli eFuse (irreversibile);
- niente firma delle immagini: il bootloader resta il nostro.

## Build (ESP-IDF 6.1, non serve la 6.0.1)

```powershell
git clone https://github.com/facebookincubator/muse-gadget-sdk
python watch-os-common\muse\apply_watch_os.py muse-gadget-sdk\esp32
copy ESP32-S3-Touch-AMOLED-1.75C\partitions.csv muse-gadget-sdk\esp32\partitions_watch_os.csv
copy watch-os-common\muse\sdkconfig.watch-os muse-gadget-sdk\esp32\devices\
cd muse-gadget-sdk\esp32
idf.py -B build-watchos-175c -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-watchos-175c/sdkconfig `
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175c;devices/sdkconfig.watch-os" build
```

## Installazione via USB

```powershell
python watch-os-common\muse\install_usb.py COM10 <MAC> build-watchos-175c\muse-gadget.bin `
  build-watchos-175c\partition_table\partition-table.bin
```

Verifica il MAC, salva 0x0–0x21000 (bootloader, tabella, NVS, otadata) in
`~/.aetherion/diagnostics/muse-install-*`, scrive solo ota_1 e avvia Muse.

## Primo avvio

Servono account Muse con Developer mode attivo e un token SDK da
gadgets.muse.ai (Account > SDK tokens), più l'accettazione dei termini SDK.
Il token si invia dalla seriale con `>sdk_token=mgst_...` oppure da
Impostazioni > Muse. Poi Muse app > Settings > Devices > Add Device.

Tenere premuto il tasto per 5 secondi azzera il pairing Muse: cancella solo le
chiavi del namespace `homehub` (token, Wi-Fi di Muse), non i dati di Watch OS.
